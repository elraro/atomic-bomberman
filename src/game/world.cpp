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
    nextDudMs_ = 1000LL * (values_.get(vid::kDudMinSeconds) + rng_.below(std::max(1, values_.get(vid::kDudRandomSeconds))));
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
    hurryAnnounced_ = false;
    regenerationMs_ = 0;
    aliens_.clear();
    campaignResult_ = 0;
    campaignRetry_ = false;
    campaignClearMs_ = 0;
    extras_.clear();
    events_.clear();
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

void World::setTeamPlay(bool on, const std::array<int, kMaxPlayers>& teams) {
    teamPlay_ = on;
    teams_ = teams;
}

int World::displayColour(int i) const {
    if (!teamPlay_ || roundMs_ < values_.get(32) * frameMs_) return i;
    return players_[static_cast<std::size_t>(i)].team != 0 ? 2 : 0;  // team 0 white, team 1 red
}

int World::winningTeam() const {
    if (!teamPlay_ || contenders_ > 1) return -1;
    for (const Player& p : players_)
        if (p.present && p.alive) return p.team;
    return -1;
}

void World::clearStartArea(Cell c) {
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
}

// ----------------------------------------------------------------- Campaign

namespace {
// Frames in each of the original's 24 death animations ("die green N"): a dead player
// leaves play, and in campaign mode may come back, when its animation has run.
constexpr std::array<int, 24> kDeathFrames{83, 62, 63, 93, 17, 79, 14, 12, 28, 77, 36, 63,
                                           75, 36, 35, 69, 63, 85, 48, 20, 59, 54, 41, 75};
}  // namespace

void World::setHuman(int i, bool human) {
    Player& p = players_[static_cast<std::size_t>(i)];
    p.human = human;
    if (!human) p.lives = 0;
}

bool World::alienPassable(AlienType type, Cell c) const {
    if (!inGrid(c) || bombAt(c) != nullptr) return false;
    return type == AlienType::Ghost ? tile(c) != Tile::Solid : tile(c) == Tile::Blank;  // ghosts pass bricks
}

void World::spawnAliens(AlienType type, int count, int speed) {
    for (int n = 0; n < count && aliens_.size() < 100; ++n) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const Cell c{rng_.below(kGridW), rng_.below(kGridH)};
            if (tile(c) == Tile::Solid) continue;
            bool clear = true;
            for (const Player& p : players_) {
                if (!p.present) continue;
                const Cell pc = pixelToCell(p.x, p.y);
                if (std::abs(pc.x - c.x) + std::abs(pc.y - c.y) <= 3) clear = false;
            }
            if (!clear) continue;
            Alien a;
            a.active = true;
            a.type = type;
            a.x = cellToPixelX(c.x);
            a.y = cellToPixelY(c.y);
            a.speed = speed;
            aliens_.push_back(a);
            break;
        }
    }
}

void World::updateAliens(int dt) {
    for (Alien& a : aliens_) {
        if (!a.active) continue;
        if (a.type == AlienType::Rover && !a.clearedStart) {
            a.clearedStart = true;
            clearStartArea(pixelToCell(a.x, a.y));
        }
        if (a.dead) {
            a.active = false;
            continue;
        }
        // The current pixel is looked at again every update (one step back, 100 extra).
        a.moveAcc += dt * a.speed / frameMs_ + 100;
        a.x -= kDx[static_cast<unsigned>(a.dir)];
        a.y -= kDy[static_cast<unsigned>(a.dir)];
        while (a.moveAcc > 0) {
            const int nx = a.x + kDx[static_cast<unsigned>(a.dir)];
            const int ny = a.y + kDy[static_cast<unsigned>(a.dir)];
            ++a.anim;
            a.moveAcc -= 100;
            const Cell here = pixelToCell(nx, ny);
            const int along = offsetInCellX(nx) * kDx[static_cast<unsigned>(a.dir)] + offsetInCellY(ny) * kDy[static_cast<unsigned>(a.dir)];
            if (along == 0) {
                // At a cell centre: stop at an obstacle, otherwise 1 chance in value 1200 to turn.
                bool turn = false;
                if (!alienPassable(a.type, step(here, a.dir))) {
                    a.moveAcc = 0;
                    turn = true;
                } else if (rng_.below(std::max(1, values_.get(vid::kAlienTurnChance))) == 0) {
                    turn = true;
                }
                if (turn) {
                    a.dir = (a.dir + (rng_.below(2) == 0 ? 3 : 1)) & 3;
                    if (!alienPassable(a.type, step(here, a.dir))) a.moveAcc = 0;
                }
            }
            if (inGrid(here) && flames_[index(here)].active) {
                a.dead = true;
                const int owner = flames_[index(here)].owner;
                if (owner >= 0 && owner < kMaxPlayers)
                    players_[static_cast<std::size_t>(owner)].score +=
                        values_.get(a.type == AlienType::Rover ? vid::kPointsRover : vid::kPointsGhost);
            }
            // Touching an enemy kills human players; computer players are left alone.
            for (int i = 0; i < kMaxPlayers; ++i) {
                const Player& p = players_[static_cast<std::size_t>(i)];
                if (p.present && p.alive && p.human && pixelToCell(p.x, p.y) == here) killPlayer(i, -1);
            }
            a.x = nx;
            a.y = ny;
        }
    }
}

void World::updateCampaign(int dt) {
    updateAliens(dt);
    const int left = secondsLeft();
    if (left >= 0 && left <= 1) campaignResult_ = 2;
    const bool anyAlien = std::any_of(aliens_.begin(), aliens_.end(), [](const Alien& a) { return a.active; });
    if (!anyAlien) {
        campaignClearMs_ += dt;
        if (campaignClearMs_ > values_.get(kOutsurviveFrames) * frameMs_ * 2) campaignResult_ = 1;
    } else {
        campaignClearMs_ = 0;
    }
    // With no human player in play the stage is lost and will be played again.
    for (const Player& p : players_) {
        if (!p.present || !p.human) continue;
        if (p.alive || (p.dying && p.dyingFrames < kDeathFrames[static_cast<std::size_t>(std::clamp(p.deathAnim, 1, 24) - 1)])) return;
    }
    campaignResult_ = 2;
    campaignRetry_ = true;
}

void World::grantPrize(int i, int powerupType) {
    Player& p = players_[static_cast<std::size_t>(i)];
    if (!p.present || powerupType < 0 || powerupType >= kPowTypeCount) return;
    ++p.inventory[static_cast<std::size_t>(powerupType)];
    p.gold = true;
}

bool World::goldTwinkling() const {
    const int seconds = values_.get(vid::kGoldTwinkleSeconds);
    return seconds == 0 || roundMs_ < seconds * 1000;
}

void World::addPlayer(int i) {
    Player& p = players_[static_cast<std::size_t>(i)];
    p = Player{};
    p.team = teamPlay_ ? teams_[static_cast<std::size_t>(i)] : 0;
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
    clearStartArea(c);
    p.lives = campaign_ ? 1 : 0;
    p.dirHistory.fill({0, kNoDir});
    if (teamPlay_) {
        std::array<bool, 2> alive{};
        for (const Player& q : players_)
            if (q.present && q.alive) alive[static_cast<std::size_t>(q.team & 1)] = true;
        contenders_ = (alive[0] ? 1 : 0) + (alive[1] ? 1 : 0);
    } else {
        contenders_ = alivePlayers();
    }
}

void World::setExtras(const std::vector<Extra>& extras, int conveyorSpeedSetting) {
    extras_ = extras;
    // Extras without a fixed position go on a random cell that is not solid and not taken.
    for (Extra& e : extras_) {
        if (inGrid(e.cell)) continue;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const Cell c{rng_.below(kGridW), rng_.below(kGridH)};
            bool taken = tile(c) == Tile::Solid;
            for (const Extra& o : extras_) taken = taken || (&o != &e && o.cell == c);
            if (!taken) {
                e.cell = c;
                break;
            }
        }
    }
    extras_.erase(std::remove_if(extras_.begin(), extras_.end(), [](const Extra& e) { return !inGrid(e.cell); }),
                  extras_.end());
    conveyorSpeed_ = values_.get(vid::kConveyorSpeed + std::clamp(conveyorSpeedSetting, 0, 2));
}

const Extra* World::extraAt(Cell c) const {
    for (const Extra& e : extras_)
        if (e.cell == c) return &e;
    return nullptr;
}

void World::updateExtras() {
    for (Extra& e : extras_) {
        if (e.type == ExtraType::Warp && !e.prepared) {
            // A warp always stands on open ground with at least one open neighbour.
            e.prepared = true;
            setTile(e.cell, Tile::Blank);
            Cell n{};
            do {
                n = step(e.cell, rng_.below(4));
            } while (!inGrid(n));
            setTile(n, Tile::Blank);
        }
        if (e.type == ExtraType::Trampoline && e.animFrame != 0 && ++e.animFrame > 12) e.animFrame = 0;
    }
}

// Trampoline flight and warp travel: the player is out of play until it ends.
void World::updateSpecial(int i, int dt) {
    Player& p = players_[static_cast<std::size_t>(i)];
    p.specialAcc += dt;
    while (p.specialAcc > 0) {
        ++p.specialFrames;
        p.specialAcc -= frameMs_;
    }
    if (p.special == Special::Trampoline) {
        const int total = values_.get(vid::kTrampolineFrames);
        if (p.specialFrames >= total / 2 && p.warpX == 0) {
            // At the top of the jump: come down on a random free cell nearby,
            // in a different row and a different column.
            p.warpX = 1;
            const Cell from = pixelToCell(p.x, p.y);
            for (int attempt = 0; attempt < 100; ++attempt) {
                const Cell c{from.x + rng_.below(5) - 2, from.y + rng_.below(5) - 2};
                if (c.x == from.x || c.y == from.y) continue;
                if (tile(c) != Tile::Blank || bombAt(c) != nullptr) continue;
                p.x = cellToPixelX(c.x);
                p.y = cellToPixelY(c.y);
                break;
            }
        }
        if (p.specialFrames >= total) p.special = Special::None;
    } else if (p.special == Special::WarpOut) {
        if (p.specialFrames > 8) {
            p.x = p.warpX;
            p.y = p.warpY;
            p.special = Special::WarpIn;
            p.specialFrames = 0;
        }
    } else if (p.special == Special::WarpIn) {
        if (p.specialFrames > 8) p.special = Special::None;
    }
}

std::vector<Event> World::takeEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
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
    // Bombs in the air or in a player's hands are not "in" a cell.
    for (const Bomb& b : bombs_)
        if (b.active && b.mode != BombMode::Flying && b.mode != BombMode::Held && pixelToCell(b.x, b.y) == c) return &b;
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
    totalMs_ += dt;
    updateExtras();
    updateBombs(dt);
    updateFlames(dt);
    regenerateTile(dt);
    updateEnclosement(dt);
    updatePlayers(dt, input);
    if (campaign_ && campaignResult_ == 0) updateCampaign(dt);
    if (hurry() && !hurryAnnounced_) {
        hurryAnnounced_ = true;
        emit(EventKind::Hurry);
    }
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
        if (b.mode == BombMode::Resting) {
            // A resting bomb on a conveyor is carried along like a sliding one.
            const Extra* e = extraAt(pixelToCell(b.x, b.y));
            if (e != nullptr && e->type == ExtraType::Conveyor) {
                const BombType type = b.type;
                b.dir = e->dir;
                b.speed = conveyorSpeed_;
                b.mode = BombMode::Sliding;
                b.type = BombType::Regular;  // no jelly bounce while merely carried
                slideBomb(b, dt);
                b.type = type;
                if (b.mode == BombMode::Sliding) b.mode = BombMode::Resting;
            }
        } else if (b.mode == BombMode::Sliding) {
            slideBomb(b, dt);
        }
        if (b.mode == BombMode::Flying) flyBomb(b, dt);
        // Fuses only run, and bombs only explode, while the round is undecided.
        if (b.dud) {
            // A dud sputters for value 323 frames, then becomes an ordinary bomb.
            b.dudAcc += dt;
            while (b.dudAcc > 0) {
                ++b.dudFrames;
                b.dudAcc -= frameMs_;
            }
            if (b.dudFrames > values_.get(vid::kDudFrames)) {
                b.dud = false;
                b.createdTick = tickCount_;  // its animation starts over
            }
        }
        if (contenders_ > 1) {
            const bool airborne = b.mode == BombMode::Flying || b.mode == BombMode::Held;
            if (b.type != BombType::Trigger && !airborne && !b.dud) b.elapsedMs += dt;
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
        if (ox == 0 && oy == 0) {
            // At a cell centre an arrow turns the bomb.
            const Extra* e = extraAt(cell);
            if (e != nullptr && e->type == ExtraType::Arrow && e->dir != b.dir) {
                b.dir = e->dir;
                b.stopRequested = false;
                b.x = nx;
                b.y = ny;
                b.moveAcc -= 100;
                slideBomb(b, 0);  // continue in the new direction with what is left
                return;
            }
        }
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
            if (b.type == BombType::Jelly) {
                b.dir = opposite(b.dir);
                emit(EventKind::BombBounced, b.owner);
            } else {
                b.mode = BombMode::Resting;
                emit(EventKind::BombStopped, b.owner);
            }
        }
        b.x = nx;
        b.y = ny;
        b.moveAcc -= 100;
        if (stop) break;
    }
}

int World::playerAt(Cell c) const {
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = players_[static_cast<std::size_t>(i)];
        if (p.present && p.alive && pixelToCell(p.x, p.y) == c) return i;
    }
    return -1;
}

// Punch or throw: the bomb leaves from the centre of its cell and flies over everything.
void World::launchBomb(Bomb& b, Dir d) {
    const Cell c = pixelToCell(b.x, b.y);
    b.x = cellToPixelX(c.x);
    b.y = cellToPixelY(c.y);
    b.moveAcc = 0;
    b.dir = d;
    b.mode = BombMode::Flying;
    b.speed = values_.get(vid::kPunchSpeed);
    b.stopRequested = false;
    b.hops = 0;
    b.flightPx = 0;
    if (b.holder >= 0) players_[static_cast<std::size_t>(b.holder)].holding = -1;
    b.holder = -1;
}

void World::flyBomb(Bomb& b, int dt) {
    b.moveAcc += b.speed * dt / frameMs_;
    while (b.moveAcc > 0) {
        const auto d = static_cast<unsigned>(b.dir);
        int nx = b.x + kDx[d];
        int ny = b.y + kDy[d];
        bool landed = false;
        if (offsetInCellX(nx) == 0 && offsetInCellY(ny) == 0) {
            Cell c = pixelToCell(nx, ny);
            // Beyond one cell outside the field the bomb re-enters from the opposite side.
            if (c.x >= kGridW + 2) nx -= (kGridW + 3) * kCellW;
            if (c.x < -1) nx += (kGridW + 3) * kCellW;
            if (c.y >= kGridH + 2) ny -= (kGridH + 3) * kCellH;
            if (c.y < -1) ny += (kGridH + 3) * kCellH;
            ++b.hops;
            if (b.hops >= 3) {
                // A bounce point: try to land here.
                if (b.type == BombType::Jelly && inGrid(c) && rng_.below(std::max(1, values_.get(vid::kJellyTurnChance))) == 0)
                    b.dir = (b.dir + rng_.below(2) * 2 - 1) & 3;
                b.flightPx = 0;
                emit(EventKind::BombBounced, b.owner);
                const bool blocked = tile(c) != Tile::Blank || bombAt(c) != nullptr ||
                                     (inGrid(c) && powerups_[index(c)].state != PowerupState::None);
                if (!blocked) {
                    if (const int victim = playerAt(c); victim >= 0) {
                        hitOnHead(victim);  // and bounce on
                    } else {
                        landed = true;
                    }
                }
            }
        }
        b.x = nx;
        b.y = ny;
        if (landed) {
            b.moveAcc = 0;
            b.mode = BombMode::Resting;
            const Cell c = pixelToCell(b.x, b.y);
            if (inGrid(c) && flames_[index(c)].active) queueDetonation(b, 0);
            break;
        }
        b.moveAcc -= 100;
        ++b.flightPx;
    }
}

// A flying bomb came down on a player: stun, and some powerups are knocked loose.
void World::hitOnHead(int playerIndex) {
    Player& p = players_[static_cast<std::size_t>(playerIndex)];
    p.stunTicks = 16;
    emit(EventKind::HeadHit, playerIndex);
    int lose = values_.get(vid::kHeadHitLossMin) + rng_.below(std::max(1, values_.get(vid::kHeadHitLossRandom)));
    while (lose-- > 0) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const int type = rng_.below(kPowTypeCount);
            int& have = p.inventory[static_cast<std::size_t>(type)];
            if (have > values_.get(vid::kStartInventory + type)) {
                scatterPowerup(type);
                --have;
                break;
            }
        }
    }
}

void World::createFlame(Cell c, int owner, bool burningBrick, Dir dir, bool tip) {
    if (!inGrid(c)) return;
    flames_[index(c)] = {true, burningBrick, owner, 0, dir, tip};
}

void World::detonate(Bomb& b) {
    b.active = false;
    emit(EventKind::BombExploded, b.owner);
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

void World::cureDiseases(Player& p) {
    p.disease = {};
    p.diseaseMs = 0;
    p.diseaseDurationMs = 0;
}

void World::giveDisease(int playerIndex) {
    Player& p = players_[static_cast<std::size_t>(playerIndex)];
    const int d = rng_.below(kDiseaseCount);
    emit(EventKind::DiseaseGot, playerIndex, d);
    if (d == kDisSwap) {
        // Trade places with a random other living player.
        for (int attempt = 0; attempt < 200; ++attempt) {
            const int o = rng_.below(kMaxPlayers);
            Player& other = players_[static_cast<std::size_t>(o)];
            if (o == playerIndex || !other.present || !other.alive) continue;
            std::swap(p.x, other.x);
            std::swap(p.y, other.y);
            break;
        }
        return;
    }
    p.disease[static_cast<std::size_t>(d)] = true;
    p.diseaseMs = 1;
    p.diseaseDurationMs = values_.get(vid::kDiseaseFrames + d) * frameMs_;
    p.diseaseCooldown = values_.get(vid::kDiseasePassCooldown);
}

void World::updateDisease(int playerIndex, int dt) {
    Player& p = players_[static_cast<std::size_t>(playerIndex)];
    if (p.diseaseCooldown > 0) --p.diseaseCooldown;
    if (p.diseaseMs == 0) return;
    p.diseaseMs += dt;
    if (p.diseaseDurationMs < p.diseaseMs) {
        cureDiseases(p);
        return;
    }
    if (p.diseaseCooldown != 0) return;
    // Passed on by proximity to any other living, healthy player.
    for (int o = 0; o < kMaxPlayers; ++o) {
        Player& other = players_[static_cast<std::size_t>(o)];
        if (o == playerIndex || !other.present || !other.alive || other.diseaseMs != 0) continue;
        if (std::abs(other.x - p.x) > kCellW - 10 || std::abs(other.y - p.y) > kCellH - 10) continue;
        other.diseaseCooldown = values_.get(vid::kDiseasePassCooldown);
        other.diseaseMs = p.diseaseMs;
        other.diseaseDurationMs = p.diseaseDurationMs;
        other.disease = p.disease;
        if (values_.get(vid::kDiseasesMultiply) == 0) {
            cureDiseases(p);  // handed over rather than copied
            break;
        }
    }
}

void World::pickUp(int playerIndex, int type) {
    Player& p = players_[static_cast<std::size_t>(playerIndex)];
    // Any pickup may cure the player's diseases.
    if (values_.get(vid::kDiseasesCurable) != 0 &&
        rng_.below(std::max(1, values_.get(vid::kDiseaseCureChance))) == 0)
        cureDiseases(p);

    if (type == kPowRandom) {
        // Re-rolled until it lands on a type the scheme does not forbid (original 0x41E480).
        for (int tries = 0; tries < 1000; ++tries) {
            type = rng_.below(12);
            if (!forbiddenRandom_[static_cast<std::size_t>(type)]) break;
        }
    }
    if (type == kPowDisease) {
        giveDisease(playerIndex);
        return;
    }
    if (type == kPowSuperDisease) {
        for (int n = 0; n < 3; ++n) giveDisease(playerIndex);
        return;
    }

    // Sound: the jelly has its own; the 7th good pickup of the round and every 5th after
    // it is "awesome" (original 0x41E4FB; the count wraps from 51 back to 7).
    ++p.goodPickups;
    const bool awesome = p.goodPickups >= 7 && (p.goodPickups - 7) % 5 == 0;
    if (p.goodPickups > 50) p.goodPickups = 7;
    emit(awesome ? EventKind::PickupAwesome : type == kPowJelly ? EventKind::PickupJelly : EventKind::Pickup, playerIndex);

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
    std::array<bool, 2> teamAlive{};
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = players_[static_cast<std::size_t>(i)];
        if (!p.present) continue;
        if (p.alive) updatePlayer(i, dt, input[static_cast<std::size_t>(i)]);
        if (!p.alive && p.dying) {
            // A dying player still counts as a contender for a short while,
            // so two players dying close together produce a draw. The count
            // starts in the tick of death, as in the original.
            // Its trigger bombs become ordinary bombs: their fuses start (original 0x424C47).
            for (Bomb& b : bombs_)
                if (b.active && b.owner == i && b.type == BombType::Trigger) {
                    b.type = BombType::Regular;
                    b.createdTick = tickCount_;
                }
            p.dyingAcc += dt;
            while (p.dyingAcc > 0) {
                ++p.dyingFrames;
                p.dyingAcc -= frameMs_;
                if (p.dyingFrames == 25) emit(EventKind::DeathTaunt, i);  // original 0x41F4EB
            }
            // When the death animation has run, everything the player held beyond the
            // starting amounts goes back onto the field as collectable powerups (original
            // 0x41DBFE): one each of punch, grab, spooge, trigger and jelly, and one per
            // extra item of the other kinds.
            if (!p.scattered && p.dyingFrames >= kDeathFrames[static_cast<std::size_t>(std::clamp(p.deathAnim, 1, 24) - 1)]) {
                p.scattered = true;
                for (int type = 0; type < kPowTypeCount; ++type) {
                    int& have = p.inventory[static_cast<std::size_t>(type)];
                    const int start = values_.has(vid::kStartInventory + type) ? values_.get(vid::kStartInventory + type) : 0;
                    const bool single = type == kPowPunch || type == kPowGrab || type == kPowSpooge || type == kPowTrigger || type == kPowJelly;
                    if (single) {
                        if (have > start) scatterPowerup(type);
                        have = std::min(have, start);
                    } else {
                        for (; have > start; --have) scatterPowerup(type);
                    }
                }
            }
        }
        // Campaign: a human whose death animation has run comes back at the start cell while
        // a life is left; a new life is granted as long as the clock is not in its last
        // stretch (original 0x41F2FA-0x41F383).
        if (campaign_ && !p.alive && p.dying && p.human && p.lives > 0 &&
            p.dyingFrames >= kDeathFrames[static_cast<std::size_t>(std::clamp(p.deathAnim, 1, 24) - 1)]) {
            --p.lives;
            p.alive = true;
            p.dying = false;
            p.scattered = false;
            p.dyingFrames = 0;
            p.dyingAcc = 0;
            p.special = Special::None;
            p.action = 0;
            const Cell start = startCells_[static_cast<std::size_t>(i)];
            p.x = cellToPixelX(start.x);
            p.y = cellToPixelY(start.y);
            clearStartArea(start);
            const int left = secondsLeft();
            if (left < 0 || left >= values_.get(vid::kHurrySeconds)) p.lives = 1;
        }
        if (p.alive || (p.dying && p.dyingFrames < values_.get(kOutsurviveFrames))) {
            ++contenders;
            teamAlive[static_cast<std::size_t>(p.team & 1)] = true;
        }
    }
    // In team play the contenders are the teams that still have someone standing.
    contenders_ = teamPlay_ ? (teamAlive[0] ? 1 : 0) + (teamAlive[1] ? 1 : 0) : contenders;
    if (campaign_) contenders_ = 2;  // the original reports a constant 2 in campaign mode

    // Carried bombs follow their holder. A holder who died takes the bomb with them.
    for (Bomb& b : bombs_) {
        if (!b.active || b.mode != BombMode::Held) continue;
        Player& h = players_[static_cast<std::size_t>(b.holder)];
        if (!h.alive) {
            b.active = false;
            h.holding = -1;
            continue;
        }
        b.x = h.x;
        b.y = h.y;
    }
}

bool World::checkFlameDeath(int i) {
    const Player& p = players_[static_cast<std::size_t>(i)];
    const Cell c = pixelToCell(p.x, p.y);
    if (!inGrid(c) || !flames_[index(c)].active) return false;
    killPlayer(i, flames_[index(c)].owner);
    return !p.alive;  // a player in the air or inside a warp survives
}

void World::killPlayer(int i, int killer) {
    Player& p = players_[static_cast<std::size_t>(i)];
    if (!p.alive) return;
    if (p.special != Special::None) return;  // in the air or inside a warp
    p.alive = false;
    emit(EventKind::PlayerDied, i);
    p.deathAnim = 1 + rng_.below(std::max(1, values_.get(vid::kDeathAnimations)));
    p.dying = true;
    p.killedBy = killer;
    if (killer >= 0 && killer < kMaxPlayers) {
        if (killer == i) {
            if (!winByKills_) --p.kills;  // original 0x41DD23
        } else {
            ++players_[static_cast<std::size_t>(killer)].kills;
            // Campaign: points for burning a computer player (original 0x41DD4A).
            if (campaign_ && !p.human) players_[static_cast<std::size_t>(killer)].score += values_.get(vid::kPointsAi);
        }
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
    if (contenders_ > 1) return -1;  // time ran out
    if (teamPlay_) return -1;        // use winningTeam()
    for (int i = 0; i < kMaxPlayers; ++i)
        if (players_[static_cast<std::size_t>(i)].present && players_[static_cast<std::size_t>(i)].alive) return i;
    return -1;
}

void World::closeCell(Cell c) {
    setTile(c, Tile::Solid);
    ++wallsClosed_;
    emit(EventKind::WallBlock);
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
        // Warp holes and trampolines go away when the walls start (original 0x405D0C).
        extras_.erase(std::remove_if(extras_.begin(), extras_.end(),
                                     [](const Extra& e) { return e.type == ExtraType::Warp || e.type == ExtraType::Trampoline; }),
                      extras_.end());
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

// Brick regeneration (original 0x426704, levels with value 340 + level > 0): every that
// many seconds, while the round is undecided, up to 100 random cells are tried; the first
// blank one with no powerup, no bomb and no player within value 695 cells becomes a brick.
void World::regenerateTile(int dt) {
    if (regenerationSeconds_ <= 0 || contenders_ <= 1) return;
    regenerationMs_ += dt;
    if (regenerationMs_ <= regenerationSeconds_ * 1000) return;
    regenerationMs_ = 0;
    const int radius = values_.get(vid::kRegenerationClearRadius);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const Cell c{rng_.below(kGridW), rng_.below(kGridH)};
        if (tile(c) != Tile::Blank || powerups_[index(c)].state != PowerupState::None || bombAt(c) != nullptr) continue;
        bool clear = true;
        for (const Player& p : players_) {
            if (!p.present) continue;
            const Cell pc = pixelToCell(p.x, p.y);
            if (std::abs(pc.x - c.x) + std::abs(pc.y - c.y) <= radius) clear = false;
        }
        if (!clear) continue;
        setTile(c, Tile::Brick);
        break;
    }
}

// The same walk as updateEnclosement, without closing anything (original 0x42690D).
std::vector<Cell> World::upcomingWallCells(int count) const {
    std::vector<Cell> cells;
    const int rings = enclosementDepth_ * 2;
    if (!walls_.armed || walls_.ring >= rings) return cells;
    Cell cursor = walls_.cursor;
    Dir dir = walls_.dir;
    int ring = walls_.ring;
    for (int n = 0; n < count; ++n) {
        Cell next = step(cursor, dir);
        if (next.x >= kGridW - ring || next.y >= kGridH - ring || next.x < ring || next.y < ring) {
            dir = turnRight(dir);
            if (dir == 1) {
                if (rings <= ring) break;
                ++ring;
                cursor = {cursor.x + 1, cursor.y + 1};
            }
            next = cursor;
        }
        cells.push_back(next);
        cursor = next;
    }
    return cells;
}

void World::checkPickup(int i) {
    const Player& p = players_[static_cast<std::size_t>(i)];
    const Cell c = pixelToCell(p.x, p.y);
    if (!inGrid(c) || powerups_[index(c)].state != PowerupState::Revealed) return;
    const int type = powerups_[index(c)].type;
    powerups_[index(c)] = {};
    pickUp(i, type);
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
    int speed = p.baseSpeed + p.inventory[kPowSkate] * values_.get(vid::kSkateSpeed) -
                p.inventory[kPowClog] * values_.get(vid::kClogSpeed);
    if (p.disease[kDisSlow]) speed /= 3;
    if (p.disease[kDisFast] || p.disease[kDisFastDrop]) speed = speed * 3 / 2;
    return speed;
}

void World::kickBomb(Bomb& b, Dir d) {
    if (b.mode == BombMode::Sliding && b.dir != d) {
        const Cell c = pixelToCell(b.x, b.y);
        b.x = cellToPixelX(c.x);
        b.y = cellToPixelY(c.y);
        b.mode = BombMode::Resting;
        b.moveAcc = 0;
    }
    if (b.mode != BombMode::Sliding || b.dir != d) emit(EventKind::BombKicked, b.owner);
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

        if (fwd == -1) {
            // One pixel before the centre of a cell holding a warp or a trampoline.
            for (Extra& e : extras_) {
                if (!(e.cell == here) || tile(here) != Tile::Blank) continue;
                if (e.type == ExtraType::Warp) {
                    Cell dest = e.cell;
                    for (const Extra& o : extras_)
                        if (&o != &e && o.type == ExtraType::Warp && o.id == e.linkTo) dest = o.cell;
                    p.special = Special::WarpOut;
                    emit(EventKind::Warped, i);
                    p.warpX = cellToPixelX(dest.x);
                    p.warpY = cellToPixelY(dest.y);
                } else if (e.type == ExtraType::Trampoline) {
                    e.animFrame = 1;
                    p.special = Special::Trampoline;
                    emit(EventKind::TrampolineJump, i);
                    p.warpX = 0;
                }
                if (p.special != Special::None) {
                    p.specialFrames = 0;
                    p.specialAcc = 0;
                    p.moveAcc = 0;
                    return false;
                }
            }
        }

        if (fwd == 0 && p.inventory[kPowKicker] > 0) {
            if (Bomb* b = findBomb(next); b != nullptr && playerPassable(step(next, requested))) {
                kickBomb(*b, requested);
                if (p.action != 1) {
                    p.action = 1;
                    p.actionFrames = 0;
                    p.actionAcc = 0;
                }
            }
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

void World::dropBomb(int i, Cell cell, int delayFrames) {
    Player& p = players_[static_cast<std::size_t>(i)];
    BombType type = p.inventory[kPowJelly] > 0 ? BombType::Jelly : BombType::Regular;
    if (p.inventory[kPowTrigger] > 0 && p.triggerBombsLaid < p.inventory[kPowBomb]) {
        type = BombType::Trigger;
        ++p.triggerBombsLaid;
    }
    int range = p.inventory[kPowFlame];
    if (p.disease[kDisShortFlame]) range = 1;
    if (p.inventory[kPowGoldflame] > 0) range = std::max(kGridW, kGridH);
    const int fuse = p.disease[kDisShortFuse] ? p.fuseFrames / 3 : p.fuseFrames;
    int out = 0;
    for (const Bomb& b : bombs_) out += b.active && b.owner == i ? 1 : 0;
    if (createBomb(i, cell, type, range, fuse)) {
        const bool forcedDrop = p.disease[kDisDropBombs] || p.disease[kDisFastDrop];
        if (p.inventory[kPowBomb] >= values_.get(vid::kBombStringSize) && out == p.inventory[kPowBomb] - 1)
            emit(EventKind::BombString, i);
        emit(forcedDrop ? EventKind::BombPooped : EventKind::BombDropped, i);
        // The newest bomb of this owner in that cell gets the start delay.
        // Now and then (not before the dud timer has run out) a regular bomb is a dud.
        bool dud = false;
        if (type == BombType::Regular && totalMs_ >= nextDudMs_) {
            nextDudMs_ = totalMs_ + 1000LL * (values_.get(vid::kDudMinSeconds) + rng_.below(std::max(1, values_.get(vid::kDudRandomSeconds))));
            dud = rng_.below(std::max(1, values_.get(vid::kDudChance))) == 0;
        }
        for (Bomb& b : bombs_)
            if (b.active && b.owner == i && b.createdTick == tickCount_ && pixelToCell(b.x, b.y) == cell) {
                b.elapsedMs = -delayFrames * frameMs_;
                b.dud = dud;
            }
    }
}

void World::handleButtons(int i, const PlayerInput& raw) {
    Player& p = players_[static_cast<std::size_t>(i)];
    // "Drops bombs" diseases press button 1 afresh on every tick.
    const bool forced = p.disease[kDisDropBombs] || p.disease[kDisFastDrop];
    PlayerInput in = raw;
    if (forced) {
        in.button1 = true;
        p.prevButton1 = false;
    }
    const bool press1 = in.button1 && !p.prevButton1 && !p.disease[kDisNoBombs];
    const bool press2 = in.button2 && !p.prevButton2;
    p.prevButton1 = in.button1;
    p.prevButton2 = in.button2;

    if (press2) {
        if (p.inventory[kPowKicker] > 0)
            for (Bomb& b : bombs_)
                if (b.active && b.owner == i && b.type != BombType::Jelly && b.mode == BombMode::Sliding)
                    b.stopRequested = true;
        if (p.inventory[kPowPunch] > 0 && !in.button1) {
            const Cell ahead = step(pixelToCell(p.x, p.y), p.facing);
            if (Bomb* b = findBomb(ahead)) {
                launchBomb(*b, p.facing);
                emit(EventKind::BombPunched, i);
            }
            p.action = 2;  // the punch animation plays whether or not a bomb was there
            p.actionFrames = 0;
            p.actionAcc = 0;
        }
        if (p.inventory[kPowTrigger] > 0) {
            Bomb* oldest = nullptr;
            int oldestTick = tickCount_;
            for (Bomb& b : bombs_)
                if (b.active && b.owner == i && b.type == BombType::Trigger && b.mode != BombMode::Flying &&
                    b.mode != BombMode::Held && b.createdTick < oldestTick) {
                    oldest = &b;
                    oldestTick = b.createdTick;
                }
            if (oldest != nullptr) queueDetonation(*oldest, 0);
        }
    }

    // A carried bomb is thrown as soon as button 1 is no longer held.
    if (p.holding >= 0 && (!in.button1 || forced)) {
        Bomb& b = bombs_[static_cast<std::size_t>(p.holding)];
        b.x = p.x;
        b.y = p.y;
        b.elapsedMs = 0;
        launchBomb(b, p.facing);
        emit(EventKind::BombThrown, i);
    }

    if (press1) {
        const Cell here = pixelToCell(p.x, p.y);
        Bomb* underfoot = findBomb(here);
        const bool own = underfoot != nullptr && underfoot->owner == i;
        if (p.inventory[kPowGrab] > 0 && own) {
            underfoot->mode = BombMode::Held;
            underfoot->holder = i;
            underfoot->stopRequested = false;
            p.holding = static_cast<int>(underfoot - bombs_.data());
            p.action = 3;
            p.actionFrames = 0;
            p.actionAcc = 0;
            emit(EventKind::BombGrabbed, i);
        } else if (p.inventory[kPowSpooge] > 0 && own && !forced) {
            // A line of bombs ahead; each starts one frame further behind.
            Cell c = here;
            for (int n = 1;; ++n) {
                c = step(c, p.facing);
                if (playerAt(c) >= 0) break;
                if (inGrid(c) && powerups_[index(c)].state != PowerupState::None) break;
                if (!playerPassable(c) || bombsOwnedBy(i) >= p.inventory[kPowBomb]) break;
                const int before = activeBombs();
                dropBomb(i, c, n);
                if (activeBombs() == before) break;
            }
        } else if (bombsOwnedBy(i) < p.inventory[kPowBomb] && playerPassable(here)) {
            dropBomb(i, here, 0);
        }
    }
}

void World::updatePlayer(int i, int dt, const PlayerInput& in) {
    Player& p = players_[static_cast<std::size_t>(i)];
    if (checkFlameDeath(i)) return;
    checkPickup(i);

    bool stunned = false;
    if (p.stunTicks > 0) {
        --p.stunTicks;
        stunned = true;
    }
    const bool frozen = startFreezeMs_ > 0 || stunned;
    // Just after picking up a bomb the controls are not read for a moment (value 665):
    // the player keeps acting on the last input.
    const bool pickupPause = p.action == 3 && p.actionFrames <= values_.get(vid::kGrabPauseFrames);
    const PlayerInput effective = frozen ? PlayerInput{} : pickupPause ? p.lastInput : in;
    p.lastInput = effective;

    updateDisease(i, dt);
    // Trapped: with all four neighbouring cells blocked a random "cornerhead" animation
    // starts; it is dropped as soon as a neighbour opens.
    {
        const Cell here = pixelToCell(p.x, p.y);
        int blocked = 0;
        for (Dir d = 0; d < 4; ++d)
            if (!playerPassable(step(here, d))) ++blocked;
        if (blocked == 4) {
            if (p.action == 0) {
                p.action = kActionCornerhead + rng_.below(std::max(1, values_.get(vid::kCornerheadCount)));
                p.actionFrames = 0;
                p.actionAcc = 0;
            }
        } else if (p.action >= kActionCornerhead) {
            p.action = 0;
        }
    }
    if (p.action != 0) {
        p.actionAcc += dt;
        while (p.actionAcc > 0) {
            ++p.actionFrames;
            p.actionAcc -= frameMs_;
        }
        // Lengths of the kick / punch sequences; the pickup state lasts one frame past its 10.
        const int length = p.action == 1 ? 8 : p.action == 2 ? 10 : p.action == 3 ? 11 : kCornerheadFrames;
        if (p.actionFrames >= length) p.action = 0;
    }

    if (p.special != Special::None) {
        updateSpecial(i, dt);
        p.prevButton1 = p.prevButton2 = false;
        return;
    }

    Dir requested = chooseDirection(p, effective);
    if (requested != kNoDir && p.disease[kDisReverse]) requested = opposite(requested);
    if (p.human) {
        // Control delay (original 0x41FBB6): every entry ages, the new request goes in front,
        // and the direction used is the newest one at least the level's delay old (the
        // oldest of the 30 if none is). With no delay that is the new request itself.
        for (auto& entry : p.dirHistory) entry.first += dt;
        for (std::size_t k = p.dirHistory.size() - 1; k > 0; --k) p.dirHistory[k] = p.dirHistory[k - 1];
        p.dirHistory[0] = {0, requested};
        for (const auto& entry : p.dirHistory) {
            requested = entry.second;
            if (controlDelayMs_ <= entry.first) break;
        }
    }
    p.moving = false;
    const Extra* under = extraAt(pixelToCell(p.x, p.y));
    const bool onConveyor = under != nullptr && under->type == ExtraType::Conveyor;
    if (requested != kNoDir) {
        int speed = playerSpeed(p);
        if (onConveyor) {
            // Faster with the belt, slower against it.
            if (under->dir == requested) speed += conveyorSpeed_;
            if (under->dir == opposite(requested)) speed -= conveyorSpeed_;
        }
        p.moveAcc += speed * dt / frameMs_;
        if (movePlayer(i, requested)) return;
    } else if (onConveyor) {
        // Standing still: carried along, facing unchanged.
        const Dir facing = p.facing;
        p.moveAcc += conveyorSpeed_ * dt / frameMs_;
        const bool died = movePlayer(i, under->dir);
        p.facing = facing;
        p.moving = false;
        if (died) return;
    }
    handleButtons(i, effective);
}

}  // namespace ab
