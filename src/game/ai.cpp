#include "game/ai.hpp"

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
}

int AiPlayer::danger(Cell c) const { return inGrid(c) ? danger_[idx(c)] : 0; }

bool AiPlayer::blocked(const World& w, Cell c) const { return w.tile(c) != Tile::Blank || w.bombAt(c) != nullptr; }

bool AiPlayer::safeWalkable(const World& w, Cell c) const {
    return !blocked(w, c) && !w.flame(c).active && danger(c) == 0;
}

Dir AiPlayer::pathStep(const World& w, Cell from, Cell to, int maxDepth) const {
    if (from == to || !inGrid(to)) return kNoDir;
    std::array<int, kGridW * kGridH> first;
    std::array<int, kGridW * kGridH> depth;
    first.fill(-2);
    std::array<Cell, kGridW * kGridH> queue;
    std::size_t head = 0, tail = 0;
    first[idx(from)] = kNoDir;
    depth[idx(from)] = 0;
    queue[tail++] = from;
    while (head < tail) {
        const Cell c = queue[head++];
        if (depth[idx(c)] >= maxDepth) continue;
        for (Dir d = 0; d < 4; ++d) {
            const Cell n = step(c, d);
            if (!inGrid(n) || first[idx(n)] != -2) continue;
            if (blocked(w, n) && !(n == to)) continue;
            first[idx(n)] = (c == from) ? d : first[idx(c)];
            depth[idx(n)] = depth[idx(c)] + 1;
            if (n == to) return first[idx(n)];
            queue[tail++] = n;
        }
    }
    return kNoDir;
}

Dir AiPlayer::stepToSafety(const World& w, Cell from, Cell* target) const {
    std::array<int, kGridW * kGridH> first;
    first.fill(-2);
    std::array<Cell, kGridW * kGridH> queue;
    std::size_t head = 0, tail = 0;
    first[idx(from)] = kNoDir;
    queue[tail++] = from;
    const int here = danger(from);
    while (head < tail) {
        const Cell c = queue[head++];
        for (Dir d = 0; d < 4; ++d) {
            const Cell n = step(c, d);
            if (!inGrid(n) || first[idx(n)] != -2 || blocked(w, n) || w.flame(n).active) continue;
            first[idx(n)] = (c == from) ? d : first[idx(c)];
            if (danger(n) == 0 || danger(n) < here / 2) {
                if (target != nullptr) *target = n;
                return first[idx(n)];
            }
            queue[tail++] = n;
        }
    }
    return kNoDir;
}

bool AiPlayer::nearestPowerup(const World& w, Cell from, int maxDepth, Cell* found) const {
    for (int r = 1; r <= maxDepth; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) + std::abs(dy) != r) continue;
                const Cell c{from.x + dx, from.y + dy};
                if (inGrid(c) && w.powerup(c).state == PowerupState::Revealed && w.tile(c) == Tile::Blank &&
                    pathStep(w, from, c, maxDepth + 1) != kNoDir) {
                    *found = c;
                    return true;
                }
            }
    return false;
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

    // 4. An enemy in this cell or next to it: bomb, one tick in five.
    if (spare && canDropHere) {
        bool enemyNear = false;
        for (int i = 0; i < kMaxPlayers; ++i) {
            const Player& o = w.player(i);
            if (i == self || !o.present || !o.alive) continue;
            if (w.teamPlay() && o.team == me.team) continue;
            const Cell oc = pixelToCell(o.x, o.y);
            if (std::abs(oc.x - here.x) + std::abs(oc.y - here.y) <= 1) enemyNear = true;
        }
        if (enemyNear && rng_.below(5) == 0) {
            press(in.button1, lastButton1_);
            return finish();
        }
    }

    // 5. Bricks next to this cell: bomb, one tick in V(915).
    if (spare && canDropHere && !me.disease[kDisNoBombs]) {
        int bricks = 0;
        for (Dir d = 0; d < 4; ++d) bricks += w.tile(step(here, d)) == Tile::Brick ? 1 : 0;
        if (bricks > 0 && rng_.below(kBrickBlastChance) == 0) {
            press(in.button1, lastButton1_);
            return finish();
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
