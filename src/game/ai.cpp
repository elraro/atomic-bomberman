#include "game/ai.hpp"

#include <algorithm>
#include <cstdlib>

namespace ab {

namespace {
constexpr int kFlameDanger = 1000;  // original value for flame cells
constexpr int kPowerupReach = 4;    // original value 920
constexpr int kBrickBlastChance = 5;  // original value 915
std::size_t idx(Cell c) { return static_cast<std::size_t>(c.y * kGridW + c.x); }
}  // namespace

void AiPlayer::buildDanger(const World& w) {
    danger_.fill(0);
    auto mark = [this](Cell c, int level) {
        if (inGrid(c) && danger_[idx(c)] < level) danger_[idx(c)] = level;
    };
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            if (w.flame({x, y}).active) mark({x, y}, kFlameDanger);
    // Every cell a bomb's blast would reach, at level 100 + elapsed fuse in
    // milliseconds (original: 0x42429B), so bombs closer to exploding rank higher.
    for (const Bomb& b : w.bombs()) {
        if (!b.active) continue;
        const int level = 100 + std::max(0, b.elapsedMs);
        const Cell origin = pixelToCell(b.x, b.y);
        mark(origin, level);
        for (Dir d = 0; d < 4; ++d) {
            Cell c = origin;
            for (int n = 0; n < b.range; ++n) {
                c = step(c, d);
                if (w.tile(c) != Tile::Blank || w.bombAt(c) != nullptr) break;
                mark(c, level);
                if (w.powerup(c).state != PowerupState::None) break;
            }
        }
    }
    markWalls(w);
}

// (continued) The closing walls: the next value-910 (15) cells on their path, the nearest
// at 100 + 10 per cell of warning, falling by 10 with each cell further ahead (0x42692D).
void AiPlayer::markWalls(const World& w) {
    constexpr int kAhead = 15;
    int level = 100 + kAhead * 10;
    for (const Cell c : w.upcomingWallCells(kAhead)) {
        if (inGrid(c) && danger_[idx(c)] < level) danger_[idx(c)] = level;
        level -= 10;
    }
}

int AiPlayer::danger(Cell c) const { return inGrid(c) ? danger_[idx(c)] : 0; }

bool AiPlayer::blocked(const World& w, Cell c) const { return w.tile(c) != Tile::Blank || w.bombAt(c) != nullptr; }

bool AiPlayer::safeWalkable(const World& w, Cell c) const {
    return !blocked(w, c) && !w.flame(c).active && danger(c) == 0;
}

// The original's search, shared by its three uses (0x4092A1, 0x40970B, 0x409C1F), written
// out as it works there rather than as a textbook search, because its quirks decide
// which of several equally good routes is taken.
//
// Up to 100 "walkers" move over a copy of the passability grid (blank tile, no bomb). One
// starts in each open neighbour of the start cell (north, east, south, west) and remembers
// that first step. On every pass the slots are visited in order; a walker marks its cell
// as used, looks left and right (which first is decided by a coin flip per search), puts a
// new walker into each open side cell (marking that cell at once), then steps forward. It
// dies if the cell ahead is blocked or used, or if its own cell had been taken by another
// walker before its turn. A walker created in a slot after its parent's sits out the rest
// of the pass. `own` is asked about a walker's cell and `look` about the cells it looks
// into; either ends the search by returning true. Passes 0..maxPasses are made.
template <class Own, class Look>
static int walkerFlood(const std::array<bool, kGridW * kGridH>& passable, Cell from, int maxPasses, int coin, Own own, Look look) {
    struct Walker {
        bool active = false;
        Cell cell{};
        int first = 0;   // direction of the first step + 1
        Dir dir = 0;
        int state = 0;   // 1 new, 2 new and to be skipped once, 0 moving
    };
    std::array<bool, kGridW * kGridH> blockedGrid;
    for (std::size_t i = 0; i < blockedGrid.size(); ++i) blockedGrid[i] = !passable[i];
    auto blockedAt = [&](Cell c) { return !inGrid(c) || blockedGrid[static_cast<std::size_t>(c.y * kGridW + c.x)]; };
    auto mark = [&](Cell c) {
        if (inGrid(c)) blockedGrid[static_cast<std::size_t>(c.y * kGridW + c.x)] = true;
    };
    std::array<Walker, 100> walkers{};
    auto freeSlot = [&]() -> int {
        for (int i = 0; i < 100; ++i)
            if (!walkers[static_cast<std::size_t>(i)].active) return i;
        return -1;
    };
    for (Dir d = 0; d < 4; ++d) {
        const Cell c = step(from, d);
        if (blockedAt(c)) continue;
        Walker& w = walkers[static_cast<std::size_t>(freeSlot())];
        w = {true, c, d + 1, d, 1};
    }
    for (int pass = 0; pass <= maxPasses; ++pass) {
        int alive = 0;
        for (int i = 0; i < 100; ++i) {
            Walker& w = walkers[static_cast<std::size_t>(i)];
            if (!w.active) continue;
            ++alive;
            if (w.state == 2) {
                w.state = 1;
                continue;
            }
            const bool taken = w.state == 0 && blockedAt(w.cell);
            w.state = 0;
            if (!blockedAt(w.cell) && own(w.cell, w.first)) return w.first;
            if (!taken) mark(w.cell);
            for (int side = -1; side <= 1; side += 2) {
                const Dir nd = (side * coin + w.dir) & 3;
                const Cell c = step(w.cell, nd);
                if (blockedAt(c)) continue;
                if (look(c, w.first)) return w.first;
                const int slot = freeSlot();
                if (slot < 0) continue;  // the original stops with an error here; 100 are never needed on this grid
                ++alive;
                walkers[static_cast<std::size_t>(slot)] = {true, c, w.first, nd, slot > i ? 2 : 1};
                mark(c);
            }
            const Cell ahead = step(w.cell, w.dir);
            if (!blockedAt(ahead) && look(ahead, w.first)) return w.first;
            if (taken || blockedAt(ahead)) w.active = false;
            w.cell = ahead;
        }
        if (alive == 0) break;
    }
    return 0;
}

std::array<bool, kGridW * kGridH> AiPlayer::passability(const World& w) const {
    std::array<bool, kGridW * kGridH> grid;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) grid[idx({x, y})] = !blocked(w, {x, y});
    return grid;
}

// First step of a route to `to` (original 0x4092A1). The target itself must be passable.
Dir AiPlayer::pathStep(const World& w, Cell from, Cell to, int maxDepth) {
    const int coin = rng_.below(2) * 2 - 1;
    if (from == to) return kNoDir;
    auto reached = [&](Cell c, int) { return c == to; };
    return walkerFlood(passability(w), from, maxDepth, coin, reached, reached) - 1;
}

// Original 0x40970B: every cell a walker stands on or looks into is compared with the least
// dangerous cell so far (starting at 10000). A cell with no danger ends the search; otherwise
// the least dangerous one seen within 20 passes is the target. The own cell is not a
// candidate, so a player in danger always moves if it can.
Dir AiPlayer::stepToSafety(const World& w, Cell from, Cell* target) {
    const int coin = rng_.below(2) * 2 - 1;
    int best = 10000, bestFirst = 0;
    auto consider = [&](Cell c, int first) {
        if (danger(c) >= best) return false;
        best = danger(c);
        bestFirst = first;
        if (target != nullptr) *target = c;
        return best == 0;
    };
    const int found = walkerFlood(passability(w), from, 20, coin, consider, consider);
    return (found != 0 ? found : bestFirst) - 1;
}

// Original 0x409C1F: ends at the first walker standing on a powerup. Only a walker's own
// cell is tested: where the path and safety searches test the cells a walker looks into,
// this one tests the walker's own cell again (it passes the walker's coordinates, not the
// looked-at cell's), which can find nothing new. A cell first entered by a side step is
// already marked used when its walker is asked, so a powerup there is seen only if another
// walker walks onto it. Kept as it is.
bool AiPlayer::nearestPowerup(const World& w, Cell from, int maxDepth, Cell* found) {
    const int coin = rng_.below(2) * 2 - 1;
    auto own = [&](Cell c, int) {
        if (w.powerup(c).state != PowerupState::Revealed) return false;
        *found = c;
        return true;
    };
    auto look = [](Cell, int) { return false; };
    return walkerFlood(passability(w), from, maxDepth, coin, own, look) != 0;
}

// A random living enemy, human players first (as the original does).
int AiPlayer::pickTarget(const World& w, int self) {
    const int start = rng_.below(kMaxPlayers);
    for (int n = 0; n < kMaxPlayers; ++n) {
        const int i = (start + n) % kMaxPlayers;
        if (i == self || !w.player(i).present || !w.player(i).alive) continue;
        if (w.teamPlay() && w.player(i).team == w.player(self).team) continue;
        return i;
    }
    return -1;
}

void AiPlayer::press(bool& button, bool& last) {
    // A press must be a fresh edge: never hold the button two ticks in a row.
    button = !last;
}

PlayerInput AiPlayer::decide(const World& w, int self, int dtMs) {
    PlayerInput in;
    const Player& me = w.player(self);
    auto finish = [&]() {
        lastButton1_ = in.button1;
        lastButton2_ = in.button2;
        return in;
    };
    if (!me.present || !me.alive) return finish();
    buildDanger(w);
    const Cell here = pixelToCell(me.x, me.y);
    // The start cell of this round (the original keeps the start position in the player
    // object and rewrites it when the player goes through a warp hole).
    if (w.tickCount() < lastTick_ || lastTick_ < 0) spawn_ = here;
    lastTick_ = w.tickCount();
    if ((me.special == Special::WarpOut || me.special == Special::WarpIn) && me.warpX != 0) spawn_ = pixelToCell(me.warpX, me.warpY);
    auto go = [&](Dir d) {
        // Never step into a flame.
        if (d != kNoDir && !w.flame(step(here, d)).active) in.dir[static_cast<std::size_t>(d)] = true;
    };

    // 1. Carrying a bomb: let go (throw). Standing on an own bomb with a grab: maybe pick it up.
    if (me.inventory[kPowGrab] > 0) {
        if (me.holding >= 0) return finish();  // button 1 released
        const Bomb* b = w.bombAt(here);
        if (b != nullptr && b->owner == self && rng_.below(2) != 0) {
            in.button1 = true;
            return finish();
        }
    }

    // 2. Punch a neighbouring bomb, one tick in four.
    if (me.inventory[kPowPunch] > 0 && rng_.below(4) == 0) {
        for (Dir d = 0; d < 4; ++d)
            if (w.bombAt(step(here, d)) != nullptr) {
                in.dir[static_cast<std::size_t>(d)] = true;
                press(in.button2, lastButton2_);
                return finish();
            }
    }

    // 3. Danger. In a safe cell with nowhere safe to go: stand still.
    if (danger(here) == 0) {
        if (me.inventory[kPowTrigger] > 0 && me.inventory[kPowPunch] == 0 && rng_.below(10) == 0)
            press(in.button2, lastButton2_);
        fleeing_ = false;
        bool anySafe = false;
        for (Dir d = 0; d < 4; ++d) anySafe = anySafe || safeWalkable(w, step(here, d));
        if (!anySafe) return finish();
    } else {
        if (fleeing_ && danger(fleeTarget_) != 0) fleeing_ = false;
        Dir d = kNoDir;
        if (!fleeing_) {
            Cell target{};
            d = stepToSafety(w, here, &target);
            if (d != kNoDir) {
                fleeing_ = true;
                fleeTarget_ = target;
            }
        } else {
            d = pathStep(w, here, fleeTarget_, 20);
            if (d == kNoDir) fleeing_ = false;
        }
        go(d);
        return finish();
    }

    const bool spare = w.bombsOwnedBy(self) < me.inventory[kPowBomb];
    const bool canDropHere = w.bombAt(here) == nullptr && w.tile(here) == Tile::Blank;

    // 4. Bricks next to this cell: bomb, one tick in V(915).
    if (spare && canDropHere && !me.disease[kDisNoBombs]) {
        int bricks = 0;
        for (Dir d = 0; d < 4; ++d) bricks += w.tile(step(here, d)) == Tile::Brick ? 1 : 0;
        if (bricks > 0 && rng_.below(kBrickBlastChance) == 0) {
            press(in.button1, lastButton1_);
            return finish();
        }
    }

    // 5. Attack (original 0x40ABED). Not within three cells of the own start cell (the test
    // compares the present cell with the start position, which a warp also rewrites). Then the
    // five cells north, west, here, east, south are looked at in that order; the first that
    // holds another player decides: a team-mate ends it, an enemy means a bomb one time in five.
    if (spare && std::abs(here.x - spawn_.x) + std::abs(here.y - spawn_.y) >= 3) {
        static constexpr int kLookX[5] = {0, -1, 0, 1, 0}, kLookY[5] = {-1, 0, 0, 0, 1};
        for (int n = 0; n < 5; ++n) {
            const Cell c{here.x + kLookX[n], here.y + kLookY[n]};
            int other = -1;
            for (int i = 0; i < kMaxPlayers && other < 0; ++i) {
                const Player& o = w.player(i);
                if (i != self && o.present && o.alive && pixelToCell(o.x, o.y) == c) other = i;
            }
            if (other < 0) continue;
            if (w.teamPlay() && w.player(other).team == me.team) break;
            if (canDropHere && rng_.below(5) == 0) {
                press(in.button1, lastButton1_);
                return finish();
            }
            break;
        }
    }

    // 6. A powerup within reach.
    if (!seekingPowerup_ && rng_.below(50) == 0 && nearestPowerup(w, here, kPowerupReach, &powerupCell_)) {
        seekingPowerup_ = true;
        powerupMs_ = 0;
    }
    if (seekingPowerup_) {
        powerupMs_ += dtMs;
        if (powerupMs_ >= 500 || w.powerup(powerupCell_).state != PowerupState::Revealed) {
            seekingPowerup_ = false;
        } else {
            const Dir d = pathStep(w, here, powerupCell_, kPowerupReach + 1);
            if (d == kNoDir) {
                seekingPowerup_ = false;
            } else {
                if (safeWalkable(w, step(here, d))) go(d);
                return finish();
            }
        }
    }

    // 7. Walk toward an enemy.
    if (!hunting_ && rng_.below(50) == 0) {
        huntTarget_ = pickTarget(w, self);
        hunting_ = huntTarget_ >= 0;
        huntMs_ = 0;
    }
    if (hunting_) {
        huntMs_ += dtMs;
        const Player& t = w.player(huntTarget_);
        if ((huntMs_ >= 500 && rng_.below(50) == 0) || !t.alive) {
            hunting_ = false;
        } else {
            const Dir d = pathStep(w, here, pixelToCell(t.x, t.y), 20);
            if (d == kNoDir) {
                if (rng_.below(2) != 0) hunting_ = false;
            } else {
                if (safeWalkable(w, step(here, d))) go(d);
                return finish();
            }
        }
    }

    // 8. Wander: keep going, sometimes turn at a junction, pick a new way when blocked.
    if (rng_.below(25) == 0 && safeWalkable(w, step(here, wanderDir_)))
        wanderDir_ = (wanderDir_ + rng_.below(2) * 2 - 1) & 3;
    if (safeWalkable(w, step(here, wanderDir_)))
        go(wanderDir_);
    else
        wanderDir_ = rng_.below(4);
    return finish();
}

}  // namespace ab
