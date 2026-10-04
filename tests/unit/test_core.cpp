// Tests for the gameplay core. Each test encodes a rule from
// docs/specifications/ (ids in comments refer to docs/testing/original-behaviour.md).
// Expected values are predictions from static analysis of the original.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "game/world.hpp"

using namespace ab;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        const auto va = (a);                                                     \
        const auto vb = (b);                                                     \
        if (!(va == vb)) {                                                       \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                        static_cast<long long>(va), static_cast<long long>(vb)); \
        }                                                                        \
    } while (0)

using Inputs = std::array<PlayerInput, kMaxPlayers>;

struct Fixture {
    World w{Values::defaults(), 1234};
    Inputs in{};

    explicit Fixture(const Scheme& s = Scheme::pillars(), int players = 2) {
        w.startRound(s, false);
        for (int i = 0; i < players; ++i) w.addPlayer(i);
        w.endStartFreeze();
    }
    void run(int ticks, int dt = 50) {
        for (int i = 0; i < ticks; ++i) w.tick(dt, in);
    }
    void hold(int player, Dir d) {
        in[static_cast<std::size_t>(player)].dir = {};
        if (d != kNoDir) in[static_cast<std::size_t>(player)].dir[static_cast<std::size_t>(d)] = true;
    }
    void press1(int player) {  // one-tick press of the bomb button
        in[static_cast<std::size_t>(player)].button1 = true;
        run(1);
        in[static_cast<std::size_t>(player)].button1 = false;
    }
    void place(int player, Cell c) {
        w.player(player).x = cellToPixelX(c.x);
        w.player(player).y = cellToPixelY(c.y);
    }
    Cell cellOf(int player) { return pixelToCell(w.player(player).x, w.player(player).y); }
};

void testGeometry() {
    for (int cy = 0; cy < kGridH; ++cy)
        for (int cx = 0; cx < kGridW; ++cx) {
            const int px = cellToPixelX(cx);
            const int py = cellToPixelY(cy);
            CHECK(pixelToCell(px, py) == (Cell{cx, cy}));
            CHECK_EQ(offsetInCellX(px), 0);
            CHECK_EQ(offsetInCellY(py), 0);
        }
    CHECK_EQ(cellToPixelX(0), 40);
    CHECK_EQ(cellToPixelY(0), 103);
    CHECK_EQ(offsetInCellX(20), -20);
    CHECK_EQ(offsetInCellX(59), 19);
    CHECK_EQ(offsetInCellY(85), -18);
    CHECK_EQ(offsetInCellY(120), 17);
    CHECK_EQ(pixelToCellX(59), 0);
    CHECK_EQ(pixelToCellX(60), 1);
    CHECK_EQ(pixelToCellY(120), 0);
    CHECK_EQ(pixelToCellY(121), 1);
}

void testValuesParser() {
    Values v;
    v.parse("; comment\r\n41,40\t\t; PGT\r\n600,0,0\r\n602,-1,-1\r\n\r\n");
    CHECK_EQ(v.get(41), 40);
    CHECK_EQ(v.get(600), 0);
    CHECK_EQ(v.get(601), 0);
    CHECK_EQ(v.get(602), -1);
    CHECK_EQ(v.get(603), -1);
    CHECK(!v.has(604));
}

void testStartFreeze() {  // T1
    World w{Values::defaults(), 1};
    w.startRound(Scheme::pillars(), false);
    w.addPlayer(0);
    w.addPlayer(1);
    Inputs in{};
    in[0].dir[1] = true;
    const int x0 = w.player(0).x;
    for (int i = 0; i < 19; ++i) w.tick(50, in);
    CHECK_EQ(w.player(0).x, x0);  // 950 ms: still frozen
    w.tick(50, in);                // freeze ends at 1000 ms and input is accepted this tick
    CHECK(w.player(0).x > x0);
}

void testSpeed() {  // M1, M2
    Fixture f;
    f.hold(0, 1);
    f.run(20);
    CHECK_EQ(f.w.player(0).x - cellToPixelX(0), 185);  // 923/100 px per 50 ms for 1 s

    Fixture g;
    g.w.player(0).inventory[kPowSkate] = 2;
    g.hold(0, 1);
    g.run(20);
    CHECK_EQ(g.w.player(0).x - cellToPixelX(0), 245);  // speed 1223
}

void testFrameRateIndependenceIsApproximate() {
    // Same second of movement at 100 Hz: each tick truncates speed*dt/50.
    Fixture f;
    f.hold(0, 1);
    f.run(100, 10);
    const int moved = f.w.player(0).x - cellToPixelX(0);
    CHECK(moved >= 183 && moved <= 185);
}

void testClampLongTick() {  // T5
    Fixture f;
    f.hold(0, 1);
    f.w.tick(1000, f.in);  // treated as 150 ms
    CHECK_EQ(f.w.player(0).x - cellToPixelX(0), 28);  // 923*150/50 = 2769 -> 28 steps
    CHECK_EQ(f.w.roundMs(), 150);
}

void testWallStop() {  // M3
    Fixture f;
    f.place(0, {0, 1});
    f.hold(0, 1);  // east: (1,1) is a pillar
    f.run(20);
    CHECK_EQ(f.w.player(0).x, cellToPixelX(0));
    CHECK_EQ(f.w.player(0).y, cellToPixelY(1));

    // Past the centre toward a blocked cell: pulled back to the centre.
    f.w.player(0).x = cellToPixelX(0) + 5;
    f.run(1);
    CHECK_EQ(f.w.player(0).x, cellToPixelX(0));

    // Field edge behaves as solid.
    Fixture g;
    g.hold(0, 3);
    g.run(20);
    CHECK_EQ(g.w.player(0).x, cellToPixelX(0));
}

void testLaneAlignment() {  // M4
    Fixture f;
    f.place(0, {0, 0});
    f.w.player(0).y -= 6;  // above the row's centre line
    f.hold(0, 1);
    f.w.player(0).moveAcc = 0;
    f.w.tick(50, f.in);  // 10 steps: 6 diagonal, 4 straight
    CHECK_EQ(f.w.player(0).y, cellToPixelY(0));
    CHECK_EQ(f.w.player(0).x, cellToPixelX(0) + 10);
}

void testCornerSlide() {  // M5
    Fixture f;
    // At the centre column of cell (0,1), 8 px north of the row line, heading
    // east into the pillar at (1,1). Lane to the north (0,0) and (1,0) are open.
    f.place(0, {0, 1});
    f.w.player(0).y -= 8;
    f.hold(0, 1);
    f.run(3);
    CHECK(f.w.player(0).y < cellToPixelY(1) - 8);  // slid north, away from the blocked row
    f.run(20);
    CHECK_EQ(f.w.player(0).y, cellToPixelY(0));     // ended up in the row above
    CHECK(f.w.player(0).x > cellToPixelX(1));       // and went east along it
}

void testDirectionPriority() {  // M6, M7
    Fixture f;
    f.place(0, {0, 1});  // east is a pillar, south is open
    f.in[0].dir = {false, true, true, false};
    f.run(4);
    CHECK(f.w.player(0).y > cellToPixelY(1));
    CHECK_EQ(f.w.player(0).x, cellToPixelX(0));

    Fixture g;
    g.place(0, {2, 2});  // all four neighbours open
    g.in[0].dir = {true, true, false, false};  // north + east -> east
    g.run(2);
    CHECK(g.w.player(0).x > cellToPixelX(2));
    g.place(0, {2, 2});
    g.in[0].dir = {false, true, true, true};  // east + south + west -> west
    g.run(2);
    CHECK(g.w.player(0).x < cellToPixelX(2));
}

void testFuse() {  // T2
    Fixture f;
    f.press1(0);
    CHECK_EQ(f.w.activeBombs(), 1);
    f.run(39);
    CHECK_EQ(f.w.activeBombs(), 1);  // 1950 ms
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 0);  // 2000 ms
    CHECK(f.w.flame({0, 0}).active);
}

void testFlameLifetime() {  // T3
    Fixture f;
    f.place(0, {4, 4});
    f.place(1, {14, 10});
    f.w.createBomb(1, {2, 2}, BombType::Regular, 2, 1);
    f.run(1);  // detonates: flame age 50 ms at the end of this tick
    CHECK(f.w.flame({2, 2}).active);
    f.run(9);  // age 500 ms
    CHECK(f.w.flame({2, 2}).active);
    f.run(1);  // age 550 ms > 500
    CHECK(!f.w.flame({2, 2}).active);
}

void testPropagation() {  // E1, E2, E3, E4
    const Scheme s = Scheme::fromRows({
        "...............",
        ".#.#.#.#.#.#.#.",
        "....::.........",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
    });
    Fixture f(s);
    f.place(0, {14, 10});
    f.place(1, {12, 10});
    f.w.createBomb(0, {2, 2}, BombType::Regular, 3, 1);
    f.run(1);
    CHECK(f.w.flame({2, 2}).active);
    CHECK(f.w.flame({1, 2}).active);
    CHECK(f.w.flame({0, 2}).active);        // west to the field edge
    CHECK(f.w.flame({3, 2}).active);
    CHECK(f.w.flame({4, 2}).active);        // first brick burns
    CHECK(f.w.flame({4, 2}).burningBrick);
    CHECK(!f.w.flame({5, 2}).active);       // second brick untouched
    CHECK(f.w.tile({5, 2}) == Tile::Brick);
    CHECK(f.w.flame({2, 1}).active);
    CHECK(f.w.flame({2, 0}).active);
    CHECK(f.w.flame({2, 5}).active);        // range 3 south
    CHECK(!f.w.flame({2, 6}).active);
    CHECK(!f.w.flame({1, 1}).active);       // pillar

    CHECK(f.w.tile({4, 2}) == Tile::Brick); // still a brick while burning
    f.run(10);
    CHECK(f.w.tile({4, 2}) == Tile::Blank); // T4: gone 500 ms later
}

void testChainReaction() {  // B5, B6, B7
    Fixture f(Scheme::pillars(), 3);
    f.place(0, {14, 10});
    f.place(1, {12, 10});
    f.place(2, {10, 10});
    f.w.createBomb(0, {2, 2}, BombType::Regular, 1, 1);    // goes off first
    f.w.createBomb(1, {3, 2}, BombType::Regular, 1, 40);
    f.w.createBomb(1, {4, 2}, BombType::Regular, 1, 40);
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 2);         // neighbour is only queued
    CHECK(!f.w.flame({3, 2}).active);       // no flame on a cell holding a bomb
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 1);         // one link per tick
    CHECK(f.w.flame({3, 2}).active);
    CHECK_EQ(f.w.flame({3, 2}).owner, 0);   // credit transferred to the first bomb's owner
    CHECK_EQ(f.w.flame({2, 2}).ageMs, 100); // no new flame sent back west onto (2,2)
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 0);
    CHECK_EQ(f.w.flame({4, 2}).owner, 0);
}

void testFusePausesWhenDecided() {  // B8
    Fixture f(Scheme::pillars(), 1);
    f.press1(0);
    f.run(100);
    CHECK_EQ(f.w.activeBombs(), 1);  // a single contender: bombs never go off
}

void testDeathIsCellBased() {  // E6, E7
    Fixture f;
    f.place(1, {14, 10});
    // Player 0 one pixel left of the boundary between cells (2,2) and (3,2).
    f.w.player(0).x = kOriginX + 3 * kCellW - 1;
    f.w.player(0).y = cellToPixelY(2);
    f.w.createBomb(1, {4, 2}, BombType::Regular, 1, 1);  // flames on (3,2), (4,2), (5,2)
    f.run(1);
    CHECK(f.w.flame({3, 2}).active);
    CHECK(f.w.player(0).alive);      // reference point still in (2,2)
    f.w.player(0).x += 1;            // now in (3,2)
    f.run(1);
    CHECK(!f.w.player(0).alive);
    CHECK_EQ(f.w.player(0).killedBy, 1);
    CHECK_EQ(f.w.player(1).kills, 1);
}

void testOutsurviveWindow() {
    Fixture f;
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.w.createBomb(1, {2, 2}, BombType::Regular, 1, 1);
    f.run(1);
    CHECK(!f.w.player(0).alive);
    CHECK_EQ(f.w.contenders(), 2);   // still counted right after dying
    f.run(19);
    CHECK_EQ(f.w.contenders(), 1);   // 20 frames later the round is decided
}

void testDropRules() {  // B1, B2, B3
    Fixture f;
    f.w.player(0).x += 12;           // off-centre inside cell (0,0)
    f.press1(0);
    const Bomb* b = f.w.bombAt({0, 0});
    CHECK(b != nullptr);
    if (b != nullptr) CHECK_EQ(b->x, cellToPixelX(0));  // snapped to the cell centre
    f.press1(0);
    CHECK_EQ(f.w.activeBombs(), 1);  // capacity 1, and the cell is occupied

    f.w.player(0).inventory[kPowBomb] = 2;
    f.press1(0);
    CHECK_EQ(f.w.activeBombs(), 1);  // still standing on a bomb: not passable

    // M8: walking off the bomb is allowed, coming back is not.
    f.hold(0, 1);
    f.run(4);
    CHECK(f.cellOf(0) == (Cell{1, 0}));
    f.hold(0, 3);
    f.run(8);
    CHECK(f.cellOf(0) == (Cell{1, 0}));
    CHECK_EQ(f.w.player(0).x, cellToPixelX(1));
}

void testButtonIsEdgeTriggered() {
    Fixture f;
    f.w.player(0).inventory[kPowBomb] = 3;
    f.in[0].button1 = true;
    f.hold(0, 1);
    f.run(12);                        // held down while walking across cells
    CHECK_EQ(f.w.activeBombs(), 1);
}

void testKick() {  // B9, B12
    Fixture f;
    f.w.player(0).inventory[kPowKicker] = 1;
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.w.createBomb(0, {3, 2}, BombType::Regular, 1, 400);
    f.hold(0, 1);
    f.run(1);                         // at the cell centre facing the bomb: kick
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Sliding);
    f.hold(0, kNoDir);
    const int x0 = b.x;
    f.run(1);
    CHECK_EQ(b.x - x0, 10);           // 1000/100 px per 50 ms
    f.run(60);
    CHECK(b.mode == BombMode::Resting);
    CHECK_EQ(b.x, cellToPixelX(14));  // slid to the last cell of the row
    CHECK_EQ(b.y, cellToPixelY(2));

    Fixture g;
    g.w.player(0).inventory[kPowKicker] = 1;
    g.place(0, {2, 2});
    g.place(1, {14, 10});
    g.w.createBomb(0, {3, 2}, BombType::Regular, 1, 400);
    g.hold(0, 1);
    g.run(1);
    g.hold(0, kNoDir);
    g.run(3);
    g.in[0].button2 = true;           // stop request
    g.run(8);
    const Bomb& gb = g.w.bombs()[0];
    CHECK(gb.mode == BombMode::Resting);
    CHECK_EQ(offsetInCellX(gb.x), 0);
    CHECK(gb.x < cellToPixelX(14));
}

void testJellyBounces() {  // B11
    Fixture f;
    f.w.player(0).inventory[kPowKicker] = 1;
    f.place(0, {11, 2});
    f.place(1, {0, 10});
    f.w.createBomb(0, {12, 2}, BombType::Jelly, 1, 400);
    f.hold(0, 1);
    f.run(1);
    f.hold(0, kNoDir);
    f.place(0, {0, 8});               // out of the way
    f.run(12);                        // reaches the east edge and turns round
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Sliding);
    CHECK_EQ(b.dir, 3);
}

void testSlidingBombMeetsFlame() {  // B10
    Fixture f(Scheme::pillars(), 3);
    f.w.player(0).inventory[kPowKicker] = 1;
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.place(2, {12, 10});
    f.w.createBomb(0, {3, 2}, BombType::Regular, 1, 400);
    f.w.createBomb(1, {8, 2}, BombType::Regular, 1, 4);   // flames at (7..9, 2) from tick 4
    f.hold(0, 1);
    f.run(1);
    f.hold(0, kNoDir);
    f.run(14);
    CHECK_EQ(f.w.activeBombs(), 0);   // stopped next to the flame and exploded a tick later
    CHECK(f.w.flame({6, 2}).active);
}

void testTriggerBombs() {  // B17
    Fixture f;
    f.w.player(0).inventory[kPowBomb] = 2;
    f.w.player(0).inventory[kPowTrigger] = 1;
    f.press1(0);
    f.run(100);
    CHECK_EQ(f.w.activeBombs(), 1);   // no fuse
    CHECK(f.w.bombs()[0].type == BombType::Trigger);
    f.place(0, {4, 4});               // out of the blast
    f.in[0].button2 = true;
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 1);   // queued
    f.in[0].button2 = false;
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 0);

    // Only "capacity" trigger bombs per pickup: one was laid above.
    f.run(20);
    f.place(0, {4, 4});
    f.press1(0);                      // second trigger bomb (capacity is 2)
    f.run(1);                         // release the button
    f.place(0, {6, 4});
    f.press1(0);                      // supply used up: a regular bomb

    int triggers = 0;
    int regular = 0;
    for (const Bomb& b : f.w.bombs())
        if (b.active) (b.type == BombType::Trigger ? triggers : regular)++;
    CHECK_EQ(triggers, 1);
    CHECK_EQ(regular, 1);
}

void testGoldflame() {  // E9
    Fixture f;
    f.w.player(0).inventory[kPowGoldflame] = 1;
    f.place(0, {4, 2});
    f.press1(0);
    f.place(0, {0, 8});
    f.run(40);
    CHECK(f.w.flame({0, 2}).active);
    CHECK(f.w.flame({14, 2}).active);
    CHECK(f.w.flame({4, 0}).active);
    CHECK(f.w.flame({4, 10}).active);
}

void testPowerupGeneration() {  // P1, P2
    Scheme s = Scheme::pillars();
    for (auto& row : s.tiles)
        for (Tile& t : row)
            if (t == Tile::Blank) t = Tile::Brick;
    World w{Values::defaults(), 99};
    w.startRound(s, true);
    std::array<int, 13> count{};
    int bricks = 0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            if (w.tile({x, y}) == Tile::Brick) ++bricks;
            const Powerup& p = w.powerup({x, y});
            if (p.state == PowerupState::Hidden) ++count[static_cast<std::size_t>(p.type)];
        }
    CHECK_EQ(bricks, 15 * 11 - 35);   // density 100 keeps every brick
    CHECK_EQ(count[kPowBomb], 10);
    CHECK_EQ(count[kPowFlame], 10);
    CHECK_EQ(count[kPowDisease], 3);
    CHECK_EQ(count[kPowKicker], 4);
    CHECK_EQ(count[kPowSkate], 8);
    CHECK_EQ(count[kPowPunch], 2);
    CHECK_EQ(count[kPowGrab], 2);
    CHECK_EQ(count[kPowSpooge], 1);
    CHECK_EQ(count[kPowJelly], 1);
    CHECK(count[kPowGoldflame] <= 2);
    CHECK(count[kPowTrigger] <= 4);

    s.brickDensity = 50;
    World h{Values::defaults(), 7};
    h.startRound(s, false);
    int half = 0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            if (h.tile({x, y}) == Tile::Brick) ++half;
    CHECK(half > 40 && half < 90);    // about half of 130
}

void testStartAreaCleared() {  // R5
    Scheme s = Scheme::pillars();
    for (auto& row : s.tiles)
        for (Tile& t : row)
            if (t == Tile::Blank) t = Tile::Brick;
    World w{Values::defaults(), 3};
    w.startRound(s, true);
    w.addPlayer(4);                   // start cell (6,4)
    CHECK(w.tile({6, 4}) == Tile::Blank);
    CHECK(w.tile({5, 4}) == Tile::Blank);
    CHECK(w.tile({7, 4}) == Tile::Blank);
    CHECK(w.tile({6, 3}) == Tile::Blank);
    CHECK(w.tile({6, 5}) == Tile::Blank);
    CHECK(w.tile({4, 4}) == Tile::Brick);
    w.addPlayer(0);                   // corner: only two neighbours exist
    CHECK(w.tile({1, 0}) == Tile::Blank);
    CHECK(w.tile({0, 1}) == Tile::Blank);
    CHECK(w.tile({1, 1}) == Tile::Solid);
}

void testPickupAndCaps() {  // P4, P5
    Fixture f;
    f.w.placePowerup({1, 0}, kPowFlame, PowerupState::Revealed);
    f.hold(0, 1);
    f.run(6);
    CHECK_EQ(f.w.player(0).inventory[kPowFlame], 3);
    CHECK(f.w.powerup({1, 0}).state == PowerupState::None);

    Fixture g;
    g.w.player(0).inventory[kPowBomb] = 8;
    g.w.placePowerup({0, 0}, kPowBomb, PowerupState::Revealed);
    g.run(1);
    CHECK_EQ(g.w.player(0).inventory[kPowBomb], 8);  // capped

    Fixture h;
    h.w.player(0).inventory[kPowTrigger] = 1;
    h.w.placePowerup({0, 0}, kPowPunch, PowerupState::Revealed);
    h.run(1);
    CHECK_EQ(h.w.player(0).inventory[kPowPunch], 1);
    CHECK_EQ(h.w.player(0).inventory[kPowTrigger], 0);  // exclusive pair
    int scattered = 0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            if (h.w.powerup({x, y}).state == PowerupState::Revealed && h.w.powerup({x, y}).type == kPowTrigger)
                ++scattered;
    CHECK_EQ(scattered, 1);  // the lost trigger is back on the field
}

void testBlastAndPowerups() {  // E5, P3
    const Scheme s = Scheme::fromRows({
        "...............",
        ".#.#.#.#.#.#.#.",
        "....:.....:....",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
        ".#.#.#.#.#.#.#.",
        "...............",
    });
    Fixture f(s);
    f.place(0, {14, 10});
    f.place(1, {12, 10});
    f.w.placePowerup({3, 2}, kPowSkate, PowerupState::Revealed);
    f.w.createBomb(0, {2, 2}, BombType::Regular, 3, 1);
    f.run(1);
    CHECK(f.w.powerup({3, 2}).state == PowerupState::None);  // destroyed
    CHECK(!f.w.flame({3, 2}).active);                        // and it stopped the blast
    CHECK(!f.w.flame({4, 2}).active);
    CHECK(f.w.tile({4, 2}) == Tile::Brick);

    // Early-round protection: a punch under the burnt brick is swapped with
    // the skate hidden under the other brick.
    Fixture g(s);
    g.place(0, {14, 10});
    g.place(1, {12, 10});
    g.w.placePowerup({4, 2}, kPowPunch, PowerupState::Hidden);
    g.w.placePowerup({10, 2}, kPowSkate, PowerupState::Hidden);
    g.w.createBomb(0, {3, 2}, BombType::Regular, 1, 1);
    g.run(1);
    CHECK(g.w.powerup({4, 2}).state == PowerupState::Revealed);
    CHECK_EQ(g.w.powerup({4, 2}).type, kPowSkate);
    CHECK_EQ(g.w.powerup({10, 2}).type, kPowPunch);
    CHECK(g.w.powerup({10, 2}).state == PowerupState::Hidden);
}

void testDeterminism() {
    auto runOnce = [] {
        Scheme s = Scheme::pillars();
        World w{Values::defaults(), 42};
        w.startRound(s, false);
        w.addPlayer(0);
        w.addPlayer(1);
        Inputs in{};
        long long sum = 0;
        for (int t = 0; t < 400; ++t) {
            in[0].dir = {};
            in[0].dir[static_cast<std::size_t>((t / 17) % 4)] = true;
            in[0].button1 = (t % 23) == 0;
            in[1].dir = {};
            in[1].dir[static_cast<std::size_t>((t / 11) % 4)] = true;
            w.tick(20 + (t % 5) * 10, in);
            sum = sum * 31 + w.player(0).x + w.player(0).y * 7 + w.player(1).x * 13 + w.activeBombs();
        }
        return sum;
    };
    CHECK_EQ(runOnce(), runOnce());
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"geometry", testGeometry},
        {"values parser", testValuesParser},
        {"start freeze", testStartFreeze},
        {"speed", testSpeed},
        {"tick length sensitivity", testFrameRateIndependenceIsApproximate},
        {"long tick clamp", testClampLongTick},
        {"wall stop", testWallStop},
        {"lane alignment", testLaneAlignment},
        {"corner slide", testCornerSlide},
        {"direction priority", testDirectionPriority},
        {"fuse", testFuse},
        {"flame lifetime", testFlameLifetime},
        {"blast propagation", testPropagation},
        {"chain reaction", testChainReaction},
        {"fuse pauses when decided", testFusePausesWhenDecided},
        {"death is cell based", testDeathIsCellBased},
        {"outsurvive window", testOutsurviveWindow},
        {"drop rules", testDropRules},
        {"button edge", testButtonIsEdgeTriggered},
        {"kick", testKick},
        {"jelly bounce", testJellyBounces},
        {"sliding bomb meets flame", testSlidingBombMeetsFlame},
        {"trigger bombs", testTriggerBombs},
        {"goldflame", testGoldflame},
        {"powerup generation", testPowerupGeneration},
        {"start area", testStartAreaCleared},
        {"pickup and caps", testPickupAndCaps},
        {"blast and powerups", testBlastAndPowerups},
        {"determinism", testDeterminism},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
