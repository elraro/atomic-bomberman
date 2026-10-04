#include "game/world.hpp"

#include <algorithm>
#include <cstdlib>

namespace ab {

namespace {

constexpr int kStartFreezeValue = 30;  // frames of input freeze at round start
constexpr int kOutsurviveFrames = 25;  // frames a dying player still counts as a contender

bool isOverpowered(int type) { return type == kPowPunch || type == kPowGrab || type == kPowSuperDisease; }

}  // namespace

// ---------------------------------------------------------------- Scheme

Scheme Scheme::pillars() {
    Scheme s;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                (x % 2 == 1 && y % 2 == 1) ? Tile::Solid : Tile::Blank;
    // Default start cells (original value ids 600-619).
    s.start = {Cell{0, 0}, Cell{14, 10}, Cell{0, 10}, Cell{14, 0}, Cell{6, 4},
               Cell{8, 0}, Cell{12, 4},  Cell{2, 6},  Cell{10, 8}, Cell{6, 10}};
    return s;
}

Scheme Scheme::fromRows(const std::array<const char*, kGridH>& rows) {
    Scheme s = pillars();
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            const char ch = rows[static_cast<std::size_t>(y)][x];
            s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                ch == '#' ? Tile::Solid : ch == ':' ? Tile::Brick : Tile::Blank;
        }
    return s;
}

// ----------------------------------------------------------------- World

World::World(const Values& values, std::uint32_t seed) : values_(values), rng_(seed) {
    if (!values_.has(kOutsurviveFrames)) values_.set(kOutsurviveFrames, 20);
    frameMs_ = 1000 / values_.get(vid::kFramesPerSecond);
    bombs_.resize(kMaxBombs);
}

void World::startRound(const Scheme& scheme, bool generatePowerups) {
    tickCount_ = 0;
    roundMs_ = 0;
    startFreezeMs_ = values_.get(kStartFreezeValue) * frameMs_;
    contenders_ = 0;
    roundLimitMs_ = values_.get(vid::kRoundSeconds) * 1000;
    enclosementDepth_ = values_.get(vid::kEnclosementDepth);
    walls_ = {};
    wallsClosed_ = 0;
    players_ = {};
    startCells_ = scheme.start;
    std::fill(bombs_.begin(), bombs_.end(), Bomb{});
    flames_ = {};
    powerups_ = {};
    pending_.clear();

    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            Tile t = scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            // Each brick of the scheme is kept with probability density / 100.
            if (t == Tile::Brick && rng_.below(100) >= scheme.brickDensity) t = Tile::Blank;
            tiles_[index({x, y})] = t;
        }

    if (!generatePowerups) return;
    for (int type = 0; type <= kPowRandom; ++type) {
        const int n = values_.get(vid::kLevelCount + type);
        const bool chancy = n < 0;  // negative: |n| attempts at 1 in 10
        for (int k = 0; k < std::abs(n); ++k) {
            if (chancy && rng_.below(10) != 0) continue;
            for (int attempt = 0; attempt < 200; ++attempt) {
                const int cx = rng_.below(kGridW);
                const int cy = rng_.below(kGridH);
                const Cell c{cx, cy};
                if (tile(c) == Tile::Brick && powerups_[index(c)].state == PowerupState::None) {
                    powerups_[index(c)] = {PowerupState::Hidden, type};
                    break;
                }
            }
        }
    }
}

void World::addPlayer(int i) {
    Player& p = players_[static_cast<std::size_t>(i)];
    p = Player{};
    p.present = true;
    p.alive = true;
    const Cell c = startCells_[static_cast<std::size_t>(i)];
    p.x = cellToPixelX(c.x);
    p.y = cellToPixelY(c.y);
    p.facing = 2;
    p.baseSpeed = values_.get(vid::kBaseSpeed);
    p.fuseFrames = values_.get(vid::kFuseFrames);
    for (int k = 0; k < kPowTypeCount; ++k)
        p.inventory[static_cast<std::size_t>(k)] = values_.get(vid::kStartInventory + k);

    // The start cell and its non-solid orthogonal neighbours are cleared.
    // (The original does this on the player's first update.)
    auto clear = [this](Cell cc) {
        if (!inGrid(cc) || tile(cc) == Tile::Solid) return;
        setTile(cc, Tile::Blank);
        powerups_[index(cc)] = {};
    };
    if (inGrid(c)) {
        setTile(c, Tile::Blank);
        powerups_[index(c)] = {};
    }
    for (Dir d = 0; d < 4; ++d) clear(step(c, d));
    contenders_ = alivePlayers();
}

Tile World::tile(Cell c) const { return inGrid(c) ? tiles_[index(c)] : Tile::Solid; }

void World::setTile(Cell c, Tile t) {
    if (inGrid(c)) tiles_[index(c)] = t;
}

void World::placePowerup(Cell c, int type, PowerupState state) {
    if (inGrid(c)) powerups_[index(c)] = {state, type};
}

int World::alivePlayers() const {
    return static_cast<int>(std::count_if(players_.begin(), players_.end(),
                                           [](const Player& p) { return p.present && p.alive; }));
}

int World::activeBombs() const {
    return static_cast<int>(std::count_if(bombs_.begin(), bombs_.end(), [](const Bomb& b) { return b.active; }));
}

int World::bombsOwnedBy(int playerIndex) const {
    return static_cast<int>(std::count_if(bombs_.begin(), bombs_.end(), [playerIndex](const Bomb& b) {
        return b.active && b.owner == playerIndex;
    }));
}

const Bomb* World::bombAt(Cell c) const {
    for (const Bomb& b : bombs_)
        if (b.active && pixelToCell(b.x, b.y) == c) return &b;
    return nullptr;
}

Bomb* World::findBomb(Cell c) { return const_cast<Bomb*>(bombAt(c)); }

bool World::anyPlayerAt(Cell c) const {
    return std::any_of(players_.begin(), players_.end(), [c](const Player& p) {
        return p.present && p.alive && pixelToCell(p.x, p.y) == c;
    });
}

bool World::playerPassable(Cell c) const { return bombAt(c) == nullptr && tile(c) == Tile::Blank; }

void World::tick(int dtMs, const std::array<PlayerInput, kMaxPlayers>& input) {
    const int dt = std::clamp(dtMs, 0, values_.get(vid::kMaxTickMs));
    ++tickCount_;
    roundMs_ += dt;
    updateBombs(dt);
    updateFlames(dt);
    updateEnclosement(dt);
    updatePlayers(dt, input);
}

// ----------------------------------------------------------------- Bombs

bool World::createBomb(int owner, Cell c, BombType type, int range, int fuseFrames) {
    for (Bomb& b : bombs_) {
        if (b.active) continue;
        b = Bomb{};
        b.active = true;
        b.owner = owner;
        b.type = type;
        b.x = cellToPixelX(c.x);
        b.y = cellToPixelY(c.y);
        b.range = range;
        b.fuseMs = fuseFrames * frameMs_;
        b.createdTick = tickCount_;
        return true;
    }
    return false;
}

void World::queueDetonation(Bomb& b, int arrivedFrom) {
    if (pending_.size() >= kMaxBombs) return;
    pending_.push_back({static_cast<int>(&b - bombs_.data()), arrivedFrom});
}

void World::updateBombs(int dt) {
    // Bombs queued during the previous tick are armed now and explode in
    // this tick's pass: every link of a chain reaction costs one tick.
    for (const Pending& q : pending_) {
        Bomb& b = bombs_[static_cast<std::size_t>(q.bomb)];
        if (!b.active) continue;
        b.elapsedMs = b.fuseMs;
        b.arrivedFrom = q.arrivedFrom;
    }
    pending_.clear();

    for (Bomb& b : bombs_) {
        if (!b.active) continue;
        if (b.mode == BombMode::Sliding) slideBomb(b, dt);
        // Fuses only run, and bombs only explode, while the round is undecided.
        if (contenders_ > 1) {
            if (b.type != BombType::Trigger) b.elapsedMs += dt;
            if (b.fuseMs <= b.elapsedMs) detonate(b);
        }
    }
}

bool World::bombCanSlideInto(Cell c) {
    if (bombAt(c) != nullptr) return false;
    if (anyPlayerAt(c)) return false;
    if (inGrid(c) && powerups_[index(c)].state == PowerupState::Revealed) destroyPowerup(c);
    return tile(c) == Tile::Blank;
}

void World::slideBomb(Bomb& b, int dt) {
    const auto d = static_cast<unsigned>(b.dir);
    b.moveAcc += b.speed * dt / frameMs_;
    // The original re-examines the current position first: it steps back one
    // pixel and credits one step before entering the loop.
    b.moveAcc += 100;
    b.x -= kDx[d];
    b.y -= kDy[d];
    while (b.moveAcc > 0) {
        int nx = b.x + kDx[d];
        int ny = b.y + kDy[d];
        const int ox = offsetInCellX(nx);
        const int oy = offsetInCellY(ny);
        const int fwd = ox * kDx[d] + oy * kDy[d];
        const Cell cell = pixelToCell(nx, ny);
        const Cell next = step(cell, b.dir);

        bool stop = false;
        const bool flameAhead = inGrid(next) && flames_[index(next)].active;
        if (flameAhead) {
            if (!flames_[index(next)].burningBrick) queueDetonation(b, 0);
            stop = true;
        } else {
            if (!bombCanSlideInto(next)) b.stopRequested = true;
            stop = b.stopRequested && fwd >= 0;
        }
        if (stop) {
            b.stopRequested = false;
            nx = cellToPixelX(cell.x);
            ny = cellToPixelY(cell.y);
            b.moveAcc = 0;
            if (b.type == BombType::Jelly)
                b.dir = opposite(b.dir);
            else
                b.mode = BombMode::Resting;
        }
        b.x = nx;
        b.y = ny;
        b.moveAcc -= 100;
        if (stop) break;
    }
}

void World::createFlame(Cell c, int owner, bool burningBrick, Dir dir, bool tip) {
    if (!inGrid(c)) return;
    flames_[index(c)] = {true, burningBrick, owner, 0, dir, tip};
}

void World::detonate(Bomb& b) {
    b.active = false;
    const Cell origin = pixelToCell(b.x, b.y);
    for (Dir d = 0; d < 4; ++d) {
        if (b.arrivedFrom != 0 && d + 1 == b.arrivedFrom) continue;
        createFlame(origin, b.owner, false);
        if (inGrid(origin) && powerups_[index(origin)].state == PowerupState::Revealed) destroyPowerup(origin);

        Cell c = origin;
        for (int n = 0; n < b.range; ++n) {
            c = step(c, d);
            if (Bomb* other = findBomb(c)) {
                other->owner = b.owner;  // kill credit follows the chain
                queueDetonation(*other, opposite(d) + 1);
                break;
            }
            if (inGrid(c) && powerups_[index(c)].state == PowerupState::Revealed) {
                destroyPowerup(c);
                break;
            }
            const Tile t = tile(c);
            if (t == Tile::Solid) break;
            if (t == Tile::Brick) {
                createFlame(c, -1, true, d);  // the brick becomes blank when this flame ends
                revealPowerup(c);
                break;
            }
            createFlame(c, b.owner, false, d, n == b.range - 1);
        }
    }
}

// ---------------------------------------------------------------- Flames

void World::updateFlames(int dt) {
    const int regular = values_.get(vid::kFlameFrames) * frameMs_;
    const int brick = values_.get(vid::kBrickBurnFrames) * frameMs_;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            Flame& f = flames_[index({x, y})];
            if (!f.active) continue;
            f.ageMs += dt;
            if (f.ageMs > (f.burningBrick ? brick : regular)) {
                if (f.burningBrick && tile({x, y}) == Tile::Brick) setTile({x, y}, Tile::Blank);
                f.active = false;
            }
        }
}

// -------------------------------------------------------------- Powerups

void World::scatterPowerup(int type) {
    for (int outer = 0; outer < 100; ++outer) {
        Cell c{};
        int inner = 100;
        do {
            c = {rng_.below(kGridW), rng_.below(kGridH)};
            if (--inner < 1) return;
        } while (tile(c) != Tile::Blank);
        if (bombAt(c) == nullptr && powerups_[index(c)].state == PowerupState::None && !anyPlayerAt(c)) {
            powerups_[index(c)] = {PowerupState::Revealed, type};
            return;
        }
    }
}

void World::destroyPowerup(Cell c) {
    const int type = powerups_[index(c)].type;
    powerups_[index(c)] = {};
    if (type == kPowDisease && values_.get(vid::kDiseasesDestroyable) == 0) scatterPowerup(type);
}

void World::revealPowerup(Cell c) {
    Powerup& here = powerups_[index(c)];
    if (here.state != PowerupState::Hidden) return;

    // Early in the round, punch, grab and super disease are swapped away.
    if (roundMs_ / 1000 < values_.get(vid::kOverpowerSeconds) && isOverpowered(here.type)) {
        bool swapped = false;
        for (int attempt = 0; attempt < 200 && !swapped; ++attempt) {
            const Cell o{rng_.below(kGridW), rng_.below(kGridH)};
            Powerup& other = powerups_[index(o)];
            if (tile(o) != Tile::Brick || other.state == PowerupState::None) continue;
            if (isOverpowered(other.type)) continue;
            std::swap(here, other);
            swapped = true;
        }
        if (!swapped) {
            for (int attempt = 0; attempt < 200; ++attempt) {
                const Cell o{rng_.below(kGridW), rng_.below(kGridH)};
                if (tile(o) == Tile::Brick && powerups_[index(o)].state == PowerupState::None) {
                    powerups_[index(o)] = here;
                    here = {};
                    return;
                }
            }
        }
    }
    if (here.state == PowerupState::Hidden) here.state = PowerupState::Revealed;
}

void World::removeFromInventory(Player& p, int type) {
    int& have = p.inventory[static_cast<std::size_t>(type)];
    const int start = values_.get(vid::kStartInventory + type);
    while (have > start) {
        scatterPowerup(type);
        --have;
    }
}

void World::pickUp(Player& p, int type) {
    if (type == kPowRandom) {
        // Becomes a random type 0-11 (scheme "forbidden" flags are not modelled yet).
        pickUp(p, rng_.below(12));
        return;
    }
    if (type == kPowDisease || type == kPowSuperDisease) return;  // diseases: not implemented

    ++p.inventory[static_cast<std::size_t>(type)];
    switch (type) {
        case kPowPunch: removeFromInventory(p, kPowTrigger); break;
        case kPowGrab: removeFromInventory(p, kPowSpooge); break;
        case kPowSpooge: removeFromInventory(p, kPowGrab); break;
        case kPowTrigger:
            p.triggerBombsLaid = 0;
            removeFromInventory(p, kPowPunch);
            removeFromInventory(p, kPowJelly);
            break;
        case kPowJelly: removeFromInventory(p, kPowTrigger); break;
        default: break;
    }
    const int cap = values_.get(vid::kInventoryCap + type);
    int& have = p.inventory[static_cast<std::size_t>(type)];
    if (cap != 0 && have > cap) have = cap;
}

// --------------------------------------------------------------- Players

void World::updatePlayers(int dt, const std::array<PlayerInput, kMaxPlayers>& input) {
    if (startFreezeMs_ > 0) startFreezeMs_ = std::max(0, startFreezeMs_ - dt);

    int contenders = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = players_[static_cast<std::size_t>(i)];
        if (!p.present) continue;
        if (p.alive) updatePlayer(i, dt, input[static_cast<std::size_t>(i)]);
        if (!p.alive && p.dying) {
            // A dying player still counts as a contender for a short while,
            // so two players dying close together produce a draw. The count
            // starts in the tick of death, as in the original.
            p.dyingAcc += dt;
            while (p.dyingAcc > 0) {
                ++p.dyingFrames;
                p.dyingAcc -= frameMs_;
            }
        }
        if (p.alive || (p.dying && p.dyingFrames < values_.get(kOutsurviveFrames))) ++contenders;
    }
    contenders_ = contenders;
}

bool World::checkFlameDeath(int i) {
    const Player& p = players_[static_cast<std::size_t>(i)];
    const Cell c = pixelToCell(p.x, p.y);
    if (!inGrid(c) || !flames_[index(c)].active) return false;
    killPlayer(i, flames_[index(c)].owner);
    return true;
}

void World::killPlayer(int i, int killer) {
    Player& p = players_[static_cast<std::size_t>(i)];
    if (!p.alive) return;
    p.alive = false;
    p.dying = true;
    p.killedBy = killer;
    if (killer >= 0 && killer < kMaxPlayers) {
        if (killer == i)
            --p.kills;
        else
            ++players_[static_cast<std::size_t>(killer)].kills;
    }
}

// ------------------------------------------------------- Clock and walls

int World::secondsLeft() const {
    if (roundLimitMs_ < 0) return -1;
    return std::max(0, (roundLimitMs_ - roundMs_) / 1000);
}

bool World::hurry() const {
    const int left = secondsLeft();
    const int at = values_.get(vid::kHurrySeconds);
    return left >= 0 && left < at && left > at - 5;
}

int World::winner() const {
    for (int i = 0; i < kMaxPlayers; ++i)
        if (players_[static_cast<std::size_t>(i)].present && players_[static_cast<std::size_t>(i)].alive) return i;
    return -1;
}

void World::closeCell(Cell c) {
    setTile(c, Tile::Solid);
    ++wallsClosed_;
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = players_[static_cast<std::size_t>(i)];
        if (p.present && p.alive && pixelToCell(p.x, p.y) == c) killPlayer(i, -1);
    }
    powerups_[index(c)] = {};
    if (Bomb* b = findBomb(c)) {
        if (values_.get(vid::kWallsDetonateBombs) != 0)
            queueDetonation(*b, -1);
        else
            b->active = false;
    }
    flames_[index(c)] = {};
}

void World::updateEnclosement(int dt) {
    if (contenders_ <= 1) return;
    const int left = secondsLeft();
    if (left < 0 || left > values_.get(vid::kHurrySeconds) - 5) {
        walls_ = {};
        return;
    }
    if (!walls_.armed) {
        walls_.armed = true;
        walls_.timerMs = -dt;  // the 250 ms count starts on the arming tick
        walls_.cursor = {0, 0};
        walls_.dir = 1;
        walls_.ring = 0;
    }
    const int rings = enclosementDepth_ * 2;
    if (walls_.ring >= rings) return;

    walls_.timerMs += dt;
    // One cell per 250 ms, at most four per tick.
    for (int n = 0; n < 4 && walls_.timerMs > 250; ++n) {
        walls_.timerMs -= 250;
        closeCell(walls_.cursor);

        Cell next = step(walls_.cursor, walls_.dir);
        const int r = walls_.ring;
        if (next.x >= kGridW - r || next.y >= kGridH - r || next.x < r || next.y < r) {
            // Turn. The corner cell is visited again on the next slot, as in the original.
            walls_.dir = turnRight(walls_.dir);
            next = walls_.cursor;
            if (walls_.dir == 1) {
                if (rings <= walls_.ring) break;
                ++walls_.ring;
                next = {walls_.cursor.x + 1, walls_.cursor.y + 1};
                if (walls_.ring >= rings) {
                    walls_.cursor = next;
                    break;
                }
            }
        }
        walls_.cursor = next;
    }
}

void World::checkPickup(int i) {
    Player& p = players_[static_cast<std::size_t>(i)];
    const Cell c = pixelToCell(p.x, p.y);
    if (!inGrid(c) || powerups_[index(c)].state != PowerupState::Revealed) return;
    const int type = powerups_[index(c)].type;
    powerups_[index(c)] = {};
    pickUp(p, type);
}

Dir World::chooseDirection(const Player& p, const PlayerInput& in) const {
    std::array<bool, 4> held = in.dir;
    const Cell here = pixelToCell(p.x, p.y);
    if (std::count(held.begin(), held.end(), true) > 1) {
        bool anyOpen = false;
        for (Dir d = 0; d < 4; ++d)
            if (held[static_cast<unsigned>(d)] && playerPassable(step(here, d))) anyOpen = true;
        if (anyOpen)
            for (Dir d = 0; d < 4; ++d)
                if (!playerPassable(step(here, d))) held[static_cast<unsigned>(d)] = false;
    }
    Dir chosen = kNoDir;
    for (Dir d = 0; d < 4; ++d)
        if (held[static_cast<unsigned>(d)]) chosen = d;  // highest index wins: W > S > E > N
    return chosen;
}

int World::playerSpeed(const Player& p) const {
    return p.baseSpeed + p.inventory[kPowSkate] * values_.get(vid::kSkateSpeed) -
           p.inventory[kPowClog] * values_.get(vid::kClogSpeed);
}

void World::kickBomb(Bomb& b, Dir d) {
    if (b.mode == BombMode::Sliding && b.dir != d) {
        const Cell c = pixelToCell(b.x, b.y);
        b.x = cellToPixelX(c.x);
        b.y = cellToPixelY(c.y);
        b.mode = BombMode::Resting;
        b.moveAcc = 0;
    }
    b.dir = d;
    b.mode = BombMode::Sliding;
    b.speed = values_.get(vid::kKickSpeed);
}

bool World::movePlayer(int i, Dir requested) {
    Player& p = players_[static_cast<std::size_t>(i)];
    const auto d = static_cast<unsigned>(requested);
    while (p.moveAcc > 0) {
        p.facing = requested;
        const int ox = offsetInCellX(p.x);
        const int oy = offsetInCellY(p.y);
        const int fwd = ox * kDx[d] + oy * kDy[d];    // < 0 before the cell centre
        const int side = oy * kDx[d] - ox * kDy[d];   // offset across the direction of travel
        const Cell here = pixelToCell(p.x, p.y);
        const Cell next = step(here, requested);

        if (fwd == 0 && p.inventory[kPowKicker] > 0) {
            if (Bomb* b = findBomb(next); b != nullptr && playerPassable(step(next, requested))) kickBomb(*b, requested);
        }

        int mx = 0;
        int my = 0;
        if (fwd < 0 || playerPassable(next)) {
            mx = kDx[d];
            my = kDy[d];
            if (side != 0) {
                // Diagonal step toward the centre line of the lane.
                const Dir s = side < 0 ? turnRight(requested) : turnLeft(requested);
                mx += kDx[static_cast<unsigned>(s)];
                my += kDy[static_cast<unsigned>(s)];
                p.facing = s;
            }
        } else if (side != 0) {
            // Blocked while off-centre: slide around the corner if the
            // neighbouring lane and the cell ahead in it are both open.
            const Dir s = side < 0 ? turnLeft(requested) : turnRight(requested);
            const Cell lane = step(here, s);
            if (playerPassable(lane) && playerPassable(step(lane, requested))) {
                mx = kDx[static_cast<unsigned>(s)];
                my = kDy[static_cast<unsigned>(s)];
            }
        } else if (fwd > 0) {
            const auto back = static_cast<unsigned>(opposite(requested));
            mx = kDx[back] * fwd;
            my = kDy[back] * fwd;
        }

        p.x += mx;
        p.y += my;
        ++p.animCounter;
        if (mx != 0 || my != 0) p.moving = true;
        if (checkFlameDeath(i)) return true;
        checkPickup(i);
        p.moveAcc -= 100;
    }
    return false;
}

void World::dropBomb(int i) {
    Player& p = players_[static_cast<std::size_t>(i)];
    BombType type = p.inventory[kPowJelly] > 0 ? BombType::Jelly : BombType::Regular;
    if (p.inventory[kPowTrigger] > 0 && p.triggerBombsLaid < p.inventory[kPowBomb]) {
        type = BombType::Trigger;
        ++p.triggerBombsLaid;
    }
    const int range = p.inventory[kPowGoldflame] > 0 ? std::max(kGridW, kGridH) : p.inventory[kPowFlame];
    createBomb(i, pixelToCell(p.x, p.y), type, range, p.fuseFrames);
}

void World::handleButtons(int i, const PlayerInput& in) {
    Player& p = players_[static_cast<std::size_t>(i)];
    const bool press1 = in.button1 && !p.prevButton1;
    const bool press2 = in.button2 && !p.prevButton2;
    p.prevButton1 = in.button1;
    p.prevButton2 = in.button2;

    if (press2) {
        if (p.inventory[kPowKicker] > 0)
            for (Bomb& b : bombs_)
                if (b.active && b.owner == i && b.type != BombType::Jelly && b.mode == BombMode::Sliding)
                    b.stopRequested = true;
        if (p.inventory[kPowTrigger] > 0) {
            Bomb* oldest = nullptr;
            int oldestTick = tickCount_;
            for (Bomb& b : bombs_)
                if (b.active && b.owner == i && b.type == BombType::Trigger && b.createdTick < oldestTick) {
                    oldest = &b;
                    oldestTick = b.createdTick;
                }
            if (oldest != nullptr) queueDetonation(*oldest, 0);
        }
    }

    if (press1) {
        const Cell here = pixelToCell(p.x, p.y);
        if (bombsOwnedBy(i) < p.inventory[kPowBomb] && playerPassable(here)) dropBomb(i);
    }
}

void World::updatePlayer(int i, int dt, const PlayerInput& in) {
    Player& p = players_[static_cast<std::size_t>(i)];
    if (checkFlameDeath(i)) return;
    checkPickup(i);

    const bool frozen = startFreezeMs_ > 0;
    const PlayerInput effective = frozen ? PlayerInput{} : in;

    const Dir requested = chooseDirection(p, effective);
    p.moving = false;
    if (requested != kNoDir) {
        p.moveAcc += playerSpeed(p) * dt / frameMs_;
        if (movePlayer(i, requested)) return;
    }
    handleButtons(i, effective);
}

}  // namespace ab
