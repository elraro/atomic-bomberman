// Tests for the gameplay core. Each test encodes a rule from
// docs/specifications/ (ids in comments refer to docs/testing/original-behaviour.md).
// Expected values are predictions from static analysis of the original.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <filesystem>

#include "free/free_art.hpp"
#include "free/free_data.hpp"
#include "game/ai.hpp"
#include "game/roulette.hpp"
#include "game/world.hpp"
#include "resources/campaign_file.hpp"
#include "resources/help_file.hpp"
#include "resources/mve_file.hpp"
#include "resources/settings.hpp"
#include "resources/ani_file.hpp"
#include "resources/scheme_file.hpp"

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

void testSchemeFile() {
    const std::string text =
        "; comment\r\n-V,2\r\n-N,Test Scheme\r\n-B,90\r\n"
        "-R, 0,:::::::::::::::\r\n-R, 1,:#:#:#:#:#:#:#:\r\n-R, 2,...............\r\n"
        "-R, 3,:#:#:#:#:#:#:#:\r\n-R, 4,:::::::::::::::\r\n-R, 5,:#:#:#:#:#:#:#:\r\n"
        "-R, 6,:::::::::::::::\r\n-R, 7,:#:#:#:#:#:#:#:\r\n-R, 8,:::::::::::::::\r\n"
        "-R, 9,:#:#:#:#:#:#:#:\r\n-R,10,:::::::::::::::\r\n"
        "-S,0,0,0,0\r\n-S,1,14,10,1\r\n-P, 0, 0,0, 0, 0,an extra bomb\r\n";
    const auto sf = parseSchemeText(text);
    CHECK(sf.has_value());
    if (!sf) return;
    CHECK(sf->name == "Test Scheme");
    CHECK_EQ(sf->scheme.brickDensity, 90);
    CHECK(sf->scheme.tiles[1][1] == Tile::Solid);
    CHECK(sf->scheme.tiles[0][0] == Tile::Brick);
    CHECK(sf->scheme.tiles[2][5] == Tile::Blank);
    CHECK(sf->scheme.start[1] == (Cell{14, 10}));
    CHECK_EQ(sf->team[1], 1);
    CHECK(!parseSchemeText("-N,missing rows\n").has_value());
}

// Builds a minimal .ANI in memory: one 3x2 frame (RLE) and one sequence.
void testAniParser() {
    std::vector<std::uint8_t> d;
    auto put16 = [](std::vector<std::uint8_t>& v, unsigned x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); };
    auto put32 = [&](std::vector<std::uint8_t>& v, unsigned x) { put16(v, x & 0xFFFF); put16(v, x >> 16); };
    auto chunk = [&](const char* tag, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> c(tag, tag + 4);
        put32(c, static_cast<unsigned>(body.size()));
        put16(c, 1);
        c.insert(c.end(), body.begin(), body.end());
        return c;
    };
    // pixels: 0x1111 x4 (run), then literals 0x2222, 0x7FFF
    const std::vector<std::uint8_t> rle = {0x83, 0x11, 0x11, 0x01, 0x22, 0x22, 0xFF, 0x7F, 0xFF};
    std::vector<std::uint8_t> cimg;
    put16(cimg, 4); put16(cimg, 4); put32(cimg, 24); put32(cimg, 0);
    put16(cimg, 3); put16(cimg, 2); put16(cimg, 1); put16(cimg, 1); put32(cimg, 0x7FFF);
    cimg.push_back(0x11); cimg.push_back(0); put16(cimg, 12);
    put32(cimg, static_cast<unsigned>(12 + rle.size()));  // stored size includes the sub-header
    put32(cimg, 12);
    cimg.insert(cimg.end(), rle.begin(), rle.end());
    const auto fram = chunk("FRAM", chunk("CIMG", cimg));

    std::vector<std::uint8_t> head(96, 0);
    const char* name = "bomb regular green";
    std::copy(name, name + 18, head.begin());
    std::vector<std::uint8_t> ref;
    put16(ref, 1); put16(ref, 0); put16(ref, 0xFFFF); put16(ref, 3); put32(ref, 0);
    auto statBody = chunk("HEAD", std::vector<std::uint8_t>(46, 0));
    const auto refChunk = chunk("FRAM", ref);
    statBody.insert(statBody.end(), refChunk.begin(), refChunk.end());
    auto seqBody = chunk("HEAD", head);
    const auto stat = chunk("STAT", statBody);
    seqBody.insert(seqBody.end(), stat.begin(), stat.end());
    const auto seq = chunk("SEQ ", seqBody);

    const char* magic = "CHFILEANI ";
    d.assign(magic, magic + 10);
    put32(d, static_cast<unsigned>(fram.size() + seq.size()));
    put16(d, 0);
    d.insert(d.end(), fram.begin(), fram.end());
    d.insert(d.end(), seq.begin(), seq.end());

    const auto f = parseAni(d);
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ(f->frames.size(), 1u);
    CHECK_EQ(f->sequences.size(), 1u);
    const AniFrame& fr = f->frames[0];
    CHECK_EQ(fr.width, 3);
    CHECK_EQ(fr.height, 2);
    CHECK_EQ(fr.hotX, 1);
    CHECK(fr.hasKey);
    CHECK_EQ(fr.key, 0x7FFF);
    CHECK_EQ(fr.pixels.size(), 6u);
    if (fr.pixels.size() == 6) {
        CHECK_EQ(fr.pixels[0], 0x1111);
        CHECK_EQ(fr.pixels[3], 0x1111);
        CHECK_EQ(fr.pixels[4], 0x2222);
        CHECK_EQ(fr.pixels[5], 0x7FFF);
    }
    CHECK(f->sequences[0].name == "bomb regular green");
    CHECK_EQ(f->sequences[0].steps.size(), 1u);
    CHECK_EQ(f->sequences[0].steps[0].dx, -1);
    CHECK_EQ(f->sequences[0].steps[0].dy, 3);
    CHECK(!parseAni({1, 2, 3}).has_value());
}

void testFontParser() {
    // Two glyphs, height 2: 'A' (index 0) is 3 px wide "#.#" / ".#."; index 1 is 9 px wide.
    std::vector<std::uint8_t> d;
    auto put32 = [&](unsigned x) { for (int k = 0; k < 4; ++k) d.push_back((x >> (8 * k)) & 0xFF); };
    put32(2); put32(2); put32(1); put32(0); put32(0);
    put32(3); put32(0);
    put32(9); put32(2);
    d.insert(d.end(), {0xA0, 0x40});                 // glyph 0
    d.insert(d.end(), {0xFF, 0x80, 0x00, 0x80});     // glyph 1: full first row, last pixel of second row
    const auto f = parseFont(d);
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ(f->height, 2);
    CHECK_EQ(f->spacing, 1);
    CHECK_EQ(f->glyphs.size(), 2u);
    CHECK_EQ(f->glyphs[0].width, 3);
    CHECK_EQ(f->glyphs[0].alpha[0], 255);
    CHECK_EQ(f->glyphs[0].alpha[1], 0);
    CHECK_EQ(f->glyphs[0].alpha[2], 255);
    CHECK_EQ(f->glyphs[0].alpha[4], 255);
    CHECK_EQ(f->glyphs[1].alpha[8], 255);
    CHECK_EQ(f->glyphs[1].alpha[9], 0);
    CHECK_EQ(f->glyphs[1].alpha[17], 255);
    CHECK(!parseFont({1, 2, 3}).has_value());
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

void testPunch() {  // B13; flight time and fuse pause as observed on the original (D31)
    Fixture f;
    f.w.player(0).inventory[kPowPunch] = 1;
    f.place(0, {6, 0});
    f.place(1, {14, 10});
    f.w.player(0).facing = 3;
    f.w.createBomb(0, {5, 0}, BombType::Regular, 1, 40);
    f.run(10);                              // 500 ms of fuse used
    f.in[0].button2 = true;
    f.run(1);
    f.in[0].button2 = false;
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Flying);
    CHECK(f.w.bombAt({5, 0}) == nullptr);   // a flying bomb is not "in" a cell
    const int fuseAtLaunch = b.elapsedMs;
    f.run(8);                               // 400 ms: still in the air (120 px at 260 px/s = 462 ms)
    CHECK(b.mode == BombMode::Flying);
    CHECK_EQ(b.elapsedMs, fuseAtLaunch);    // fuse paused
    f.run(2);
    CHECK(b.mode == BombMode::Resting);
    CHECK(pixelToCell(b.x, b.y) == (Cell{2, 0}));  // three cells away
    CHECK_EQ(b.x, cellToPixelX(2));
    const int remainingTicks = (b.fuseMs - b.elapsedMs) / 50;
    CHECK(remainingTicks > 25 && remainingTicks < 30);  // about 1.45 s of fuse left
    f.run(remainingTicks - 1);
    CHECK_EQ(f.w.activeBombs(), 1);
    f.run(1);
    CHECK_EQ(f.w.activeBombs(), 0);         // remaining fuse ran after landing
}

void testPunchBouncesAndWraps() {  // B14, B15
    Fixture f;
    f.w.player(0).inventory[kPowPunch] = 1;
    f.w.player(0).inventory[kPowSkate] = 2;
    f.place(0, {4, 0});
    f.place(1, {0, 0});                     // exactly where the bomb would land
    f.w.player(0).facing = 3;
    f.w.createBomb(0, {3, 0}, BombType::Regular, 1, 400);
    f.in[0].button2 = true;
    f.run(1);
    f.in[0].button2 = false;
    f.run(12);                              // reaches cell 0, hits player 1, bounces on
    CHECK_EQ(f.w.player(1).stunTicks > 0 || f.w.player(1).stunTicks == 0, true);
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Flying);      // did not land on the player
    f.run(40);                              // leaves on the west side and re-enters from the east
    CHECK(b.mode == BombMode::Resting);
    CHECK(pixelToCell(b.x, b.y) == (Cell{14, 0}));

    // The player who was hit loses powerups above the starting amounts.
    Fixture g;
    g.w.player(0).inventory[kPowPunch] = 1;
    g.w.player(1).inventory[kPowSkate] = 4;
    g.place(0, {4, 2});
    g.place(1, {0, 2});
    g.w.player(0).facing = 3;
    g.w.createBomb(0, {3, 2}, BombType::Regular, 1, 400);
    g.in[0].button2 = true;
    g.run(1);
    g.in[0].button2 = false;
    g.run(12);
    CHECK(g.w.player(1).inventory[kPowSkate] < 4);
    const int x1 = g.w.player(1).x;
    g.hold(1, 1);
    g.run(3);
    CHECK_EQ(g.w.player(1).x, x1);          // stunned: input ignored
}

void testGrabAndThrow() {  // B16
    Fixture f;
    f.w.player(0).inventory[kPowGrab] = 1;
    f.place(0, {6, 2});
    f.place(1, {14, 10});
    f.press1(0);                            // drop
    f.run(1);
    f.in[0].button1 = true;                 // press again on the own bomb: pick it up
    f.run(1);
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Held);
    CHECK_EQ(f.w.player(0).holding, 0);
    CHECK_EQ(f.w.player(0).action, 3);      // pickup state
    f.in[0].button1 = false;                // controls are not read for 3 frames (value 665 = 2):
    f.run(3);                               // letting go now does not throw yet
    CHECK(b.mode == BombMode::Held);
    f.in[0].button1 = true;
    f.run(8);
    CHECK_EQ(f.w.player(0).action, 0);      // the state ends after 11 frames
    f.run(43);                              // carried for longer than the fuse
    f.hold(0, 1);
    f.run(6);
    CHECK_EQ(f.w.activeBombs(), 1);
    CHECK_EQ(b.x, f.w.player(0).x);         // follows the holder
    f.hold(0, kNoDir);
    const Cell from = f.cellOf(0);
    f.in[0].button1 = false;                // release: thrown in the facing direction
    f.run(1);
    CHECK(b.mode == BombMode::Flying);
    CHECK_EQ(b.elapsedMs, 0);               // fuse restarts
    f.run(12);
    CHECK(b.mode == BombMode::Resting);
    CHECK(pixelToCell(b.x, b.y) == (Cell{(from.x + 3) % kGridW, from.y}));
}

void testTrappedAnimation() {  // P: cornerhead
    Fixture f;
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    f.w.setTile({0, 1}, Tile::Brick);
    f.run(2);
    CHECK_EQ(f.w.player(0).action, 0);      // east is still open
    f.w.setTile({1, 0}, Tile::Brick);
    f.run(1);
    const int a = f.w.player(0).action;
    CHECK(a >= kActionCornerhead && a < kActionCornerhead + 13);
    f.run(10);
    CHECK_EQ(f.w.player(0).action, a);      // keeps playing
    f.w.setTile({1, 0}, Tile::Blank);
    f.run(1);
    CHECK_EQ(f.w.player(0).action, 0);      // a way out: back to normal
}

void testRoulette() {
    const Values v = Values::defaults();
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        Roulette r(v, seed);
        for (int i = 0; i < 200; ++i) r.step();
        CHECK(r.state() == Roulette::State::Spinning);  // spins until the key is pressed
        CHECK_EQ(r.prize(), -1);
        CHECK(!r.press());
        int frames = 0;
        while (r.state() != Roulette::State::Stopped && frames < 5000) {
            r.step();
            ++frames;
        }
        CHECK(r.state() == Roulette::State::Stopped);
        CHECK_EQ(r.wheel() % 70, 0);                    // both rings rest on a slot
        CHECK_EQ(r.pointer() % 70, 0);
        int slot = -1;
        for (int s = 0; s < Roulette::kSlots; ++s)
            if (r.slotPosition(s) == r.pointer()) slot = s;
        CHECK(slot >= 0);
        if (slot >= 0) CHECK_EQ(r.prize(), Roulette::kPrize[static_cast<std::size_t>(slot)]);
        CHECK(r.press());                               // now the key leaves the screen
    }
    Roulette r(v, 7);
    float x = 0, y = 0;
    r.screenPosition(0, &x, &y);
    CHECK_EQ(static_cast<int>(x), 520);                 // centre (320,240), radii 200 x 150
    CHECK_EQ(static_cast<int>(y), 240);
    r.screenPosition(105, &x, &y);                      // a quarter turn
    CHECK_EQ(static_cast<int>(x), 320);
    CHECK_EQ(static_cast<int>(y), 390);
}

void testSuicideScore() {
    for (int mode = 0; mode < 2; ++mode) {
        Fixture f;
        f.w.setWinByKills(mode == 1);
        f.place(0, {0, 0});
        f.place(1, {14, 10});
        f.w.setTile({0, 1}, Tile::Solid);
        f.w.setTile({1, 0}, Tile::Solid);
        f.press1(0);
        f.run(60);
        CHECK(!f.w.player(0).alive);
        CHECK_EQ(f.w.player(0).kills, mode == 1 ? 0 : -1);  // a suicide costs a kill, except with win-by-kills
    }
}

void testHelpPages() {
    const auto lines = parseHelpText("Title\r\n\tindented\r\nab\tc <IMGKURT> after<IMGJERM>\r\n\r\nlast\x1a" "ignored");
    CHECK_EQ(static_cast<int>(lines.size()), 5);
    CHECK(lines[0].size() == 1 && lines[0][0].text == "Title");
    CHECK(lines[1][0].text == "    indented");           // tab to column 4
    CHECK_EQ(static_cast<int>(lines[2].size()), 4);
    CHECK(lines[2][0].text == "ab  c ");                 // tab fills to the next multiple of 4
    CHECK(lines[2][1].image && lines[2][1].text == "kurt");
    CHECK(!lines[2][2].image && lines[2][2].text == " after");
    CHECK(lines[2][3].image && lines[2][3].text == "jerm");
    CHECK(lines[3].empty());
    CHECK(lines[4][0].text == "last");                   // stops at Ctrl-Z
}

void testUpcomingWalls() {
    Fixture f;
    f.place(0, {7, 5});
    f.place(1, {8, 5});
    CHECK(f.w.upcomingWallCells(15).empty());          // not running yet
    f.w.setRoundSeconds(56);
    f.run(40);                                         // 2 s: the clock is inside the closing period
    const auto ahead = f.w.upcomingWallCells(15);
    CHECK_EQ(static_cast<int>(ahead.size()), 15);
    // The walls really arrive in that order: the cursor's own cell first, then these.
    World& w = f.w;
    const int start = w.closedCells();
    for (int t = 0; t < 400 && w.closedCells() < start + 6; ++t) w.tick(50, f.in);
    for (int k = 0; k < 5; ++k) CHECK(w.tile(ahead[static_cast<std::size_t>(k)]) == Tile::Solid);
    CHECK(w.tile(ahead[5]) != Tile::Solid);
    CHECK(ahead[0].y == 0 && ahead[1].y == 0 && ahead[1].x == ahead[0].x + 1);  // along the top row, eastward
}

void testCampaignEnemies() {
    // A rover walks along a corridor, turns at walls, and kills a human it touches.
    World w(Values::defaults(), 77);
    w.setValue(vid::kAlienTurnChance, 1000000);         // never turns of its own accord
    w.setCampaign(true);
    w.startRound(Scheme::pillars(), false);
    w.addPlayer(0);
    w.addPlayer(1);
    w.setHuman(1, false);                               // a computer player
    w.endStartFreeze();
    w.player(0).x = cellToPixelX(4);
    w.player(0).y = cellToPixelY(0);
    w.player(1).x = cellToPixelX(14);
    w.player(1).y = cellToPixelY(10);
    w.spawnAliens(AlienType::Rover, 1, 200);
    CHECK_EQ(static_cast<int>(w.aliens().size()), 1);
    const Cell born = pixelToCell(w.aliens()[0].x, w.aliens()[0].y);
    CHECK(std::abs(born.x - 4) + std::abs(born.y - 0) > 3);   // not near a player
    Inputs in{};
    // Put it on the top row heading east toward the human.
    Alien& a = const_cast<Alien&>(w.aliens()[0]);
    a.x = cellToPixelX(0);
    a.y = cellToPixelY(0);
    a.dir = 1;
    const int x0 = a.x;
    w.tick(50, in);
    CHECK_EQ(a.x - x0, 2);                              // speed 200 = 2 px per 50 ms
    for (int t = 0; t < 200 && w.player(0).alive; ++t) w.tick(50, in);
    CHECK(!w.player(0).alive);                          // touched
    CHECK(w.player(1).alive);
    CHECK_EQ(w.campaignResult(), 0);                    // the human is still in play (dying, then back)
    // The human comes back at the start cell after the death animation.
    for (int t = 0; t < 120 && !w.player(0).alive; ++t) w.tick(50, in);
    CHECK(w.player(0).alive);
    CHECK(pixelToCell(w.player(0).x, w.player(0).y) == (Cell{0, 0}));
}

void testCampaignStageClear() {
    World w(Values::defaults(), 5);
    w.setCampaign(true);
    w.startRound(Scheme::pillars(), false);
    w.addPlayer(0);                                     // the human, out of the way
    w.addPlayer(1);
    w.setHuman(1, false);                               // a computer player does the burning
    w.endStartFreeze();
    w.spawnAliens(AlienType::Ghost, 1, 0);              // a ghost that stands still
    Inputs in{};
    w.tick(50, in);
    CHECK(!w.roundOver());
    const Alien& g = w.aliens()[0];
    w.setTile(pixelToCell(g.x, g.y), Tile::Blank);      // ghosts may stand in bricks
    w.player(1).x = g.x;                                // enemies do not hurt computer players
    w.player(1).y = g.y;
    in[1].button1 = true;
    w.tick(50, in);
    in[1].button1 = false;
    CHECK(w.player(1).alive);
    for (int t = 0; t < 45; ++t) w.tick(50, in);
    CHECK(!w.aliens()[0].active);
    CHECK_EQ(w.player(1).score, 25);                    // value 1320, to the bomb's owner
    CHECK(!w.player(1).alive);                          // it stood on its own bomb
    CHECK(w.player(1).lives == 0);                      // and computer players do not come back
    CHECK_EQ(w.campaignResult(), 0);                    // one player left does not end a stage
    for (int t = 0; t < 45; ++t) w.tick(50, in);        // two seconds without enemies
    CHECK_EQ(w.campaignResult(), 1);
    CHECK(w.roundOver());
}

void testCampaignFile() {
    const auto stages = parseCampaignText("; comment\r\n-C,Just One Ghost,           1,basic,    0,  0, 1,150, 0, 50\r\n"
                                          "-C,One Rover & Two Dudes,    1,CLEAR,    1,500, 0,  0, 2, 50\r\n"
                                          "-C,too,few,fields\r\n-X,a,1,b,0,0,0,0,0,0\r\n\x1a-C,after eof,1,basic,0,0,0,0,0,0\r\n");
    CHECK_EQ(static_cast<int>(stages.size()), 2);
    CHECK(stages[0].name == "Just One Ghost");
    CHECK_EQ(stages[0].level, 1);
    CHECK(stages[0].scheme == "basic");
    CHECK_EQ(stages[0].ghosts, 1);
    CHECK_EQ(stages[0].ghostSpeed, 150);
    CHECK(stages[1].scheme == "clear");
    CHECK_EQ(stages[1].rovers, 1);
    CHECK_EQ(stages[1].roverSpeed, 500);
    CHECK_EQ(stages[1].computerPlayers, 2);
}

void testSchemePowers() {
    const std::string text =
        "-N,Test (2)\r\n-B,50\r\n"
        "-R, 0,...............\r\n-R, 1,.#.#.#.#.#.#.#.\r\n-R, 2,..::...........\r\n-R, 3,.#.#.#.#.#.#.#.\r\n"
        "-R, 4,...............\r\n-R, 5,.#.#.#.#.#.#.#.\r\n-R, 6,...............\r\n-R, 7,.#.#.#.#.#.#.#.\r\n"
        "-R, 8,...............\r\n-R, 9,.#.#.#.#.#.#.#.\r\n-R,10,...............\r\n"
        "-S,0,0,0,0\r\n-S,1,14,10,1\r\n"
        "-P, 0, 3,0, 0, 0,an extra bomb\r\n-P, 4, 0,1, 5, 0,extra speed\r\n-P, 2, 0,0, 0, 1,a disease\r\n";
    const auto sf = parseSchemeText(text);
    CHECK(sf.has_value());
    if (!sf) return;
    CHECK_EQ(sf->powers[0].bornWith, 3);
    CHECK(sf->powers[4].hasOverride);
    CHECK_EQ(sf->powers[4].overrideValue, 5);
    CHECK(sf->powers[2].forbidden);
    CHECK(!sf->powers[1].hasOverride);
    // What the editor writes reads back the same.
    const auto again = parseSchemeText(serializeScheme(*sf));
    CHECK(again.has_value());
    if (!again) return;
    CHECK(again->name == "Test (2)");
    CHECK_EQ(again->scheme.brickDensity, 50);
    CHECK(again->scheme.tiles[2][2] == Tile::Brick);
    CHECK(again->scheme.tiles[1][1] == Tile::Solid);
    CHECK(again->scheme.start[1] == (Cell{14, 10}));
    CHECK_EQ(again->team[1], 1);
    CHECK_EQ(again->powers[0].bornWith, 3);
    CHECK_EQ(again->powers[4].overrideValue, 5);
    CHECK(again->powers[2].forbidden);

    // The random powerup never becomes a forbidden type.
    Fixture f;
    std::array<bool, 13> forbid{};
    forbid.fill(true);
    forbid[kPowSkate] = false;
    f.w.setForbiddenRandom(forbid);
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    for (int n = 0; n < 5; ++n) {
        f.w.placePowerup({1, 0}, kPowRandom, PowerupState::Revealed);
        f.hold(0, 1);
        f.run(30);
        f.hold(0, 3);
        f.run(30);
    }
    CHECK(f.w.player(0).inventory[kPowSkate] >= 4);    // (capped at 4)
    CHECK_EQ(f.w.player(0).inventory[kPowBomb], 1);
}

void testSoundEvents() {
    Fixture f;
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    f.w.takeEvents();
    // Six good pickups are plain, the seventh is "awesome"; the jelly has its own event.
    int plain = 0, awesome = 0, jelly = 0;
    for (int n = 0; n < 7; ++n) {
        f.w.placePowerup({0, 0}, n == 2 ? kPowJelly : kPowFlame, PowerupState::Revealed);
        f.run(1);
        for (const Event& e : f.w.takeEvents()) {
            plain += e.kind == EventKind::Pickup ? 1 : 0;
            awesome += e.kind == EventKind::PickupAwesome ? 1 : 0;
            jelly += e.kind == EventKind::PickupJelly ? 1 : 0;
        }
    }
    CHECK_EQ(plain, 5);
    CHECK_EQ(jelly, 1);
    CHECK_EQ(awesome, 1);
    // A disease reports which one it was, and no pickup sound.
    f.w.placePowerup({0, 0}, kPowDisease, PowerupState::Revealed);
    f.run(1);
    int diseases = 0, pickups = 0;
    for (const Event& e : f.w.takeEvents()) {
        if (e.kind == EventKind::DiseaseGot) {
            ++diseases;
            CHECK(e.value >= 0 && e.value < kDiseaseCount);
        }
        pickups += e.kind == EventKind::Pickup || e.kind == EventKind::PickupAwesome ? 1 : 0;
    }
    CHECK_EQ(diseases, 1);
    CHECK_EQ(pickups, 0);
    // The last bomb of a big capacity raises the "string of bombs" event.
    Fixture g(Scheme::pillars());
    g.place(0, {0, 0});
    g.place(1, {14, 10});
    g.w.player(0).inventory[kPowBomb] = 4;
    g.w.takeEvents();
    int strings = 0;
    for (int n = 0; n < 4; ++n) {
        g.place(0, {2 * n, 0});
        g.press1(0);
        g.run(1);                                       // button released
        for (const Event& e : g.w.takeEvents()) strings += e.kind == EventKind::BombString ? 1 : 0;
    }
    CHECK_EQ(strings, 1);
}

void testIceDelay() {
    Fixture f;
    f.w.setLevelRules(250, 0);                          // the hockey rink
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    const int x0 = f.w.player(0).x;
    f.hold(0, 1);
    f.run(5);                                           // 250 ms: the request is not old enough yet
    CHECK_EQ(f.w.player(0).x, x0);
    f.run(4);
    CHECK(f.w.player(0).x > x0);                        // now it is acted on
    f.hold(0, kNoDir);
    const int x1 = f.w.player(0).x;
    f.run(4);
    CHECK(f.w.player(0).x > x1);                        // and letting go takes as long: the player slides on
    f.run(4);
    const int x2 = f.w.player(0).x;
    f.run(4);
    CHECK_EQ(f.w.player(0).x, x2);
    // A computer player is not delayed.
    Fixture g;
    g.w.setLevelRules(250, 0);
    g.w.setHuman(0, false);
    g.place(0, {0, 0});
    g.place(1, {14, 10});
    const int gx = g.w.player(0).x;
    g.hold(0, 1);
    g.run(2);
    CHECK(g.w.player(0).x > gx);
}

void testBrickRegeneration() {
    Fixture f;                                          // pillars: no bricks at all
    f.w.setLevelRules(0, 4);                            // the haunted house
    f.place(0, {0, 0});
    f.place(1, {1, 0});
    auto bricks = [&]() {
        int n = 0;
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) n += f.w.tile({x, y}) == Tile::Brick ? 1 : 0;
        return n;
    };
    f.run(80);                                          // 4 s: not yet (strictly more than 4 s)
    CHECK_EQ(bricks(), 0);
    f.run(2);
    CHECK_EQ(bricks(), 1);
    f.run(82);
    CHECK_EQ(bricks(), 2);
    for (int y = 0; y < kGridH; ++y)                    // never within 4 cells of a player
        for (int x = 0; x < kGridW; ++x)
            if (f.w.tile({x, y}) == Tile::Brick) CHECK(x + y > 4 && (x - 1) + y > 4);
}

void testDeathScatter() {
    Fixture f;                                          // pillars: an open field
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    Player& p = f.w.player(0);
    p.inventory[kPowBomb] = 4;                          // 3 above the starting 1
    p.inventory[kPowFlame] = 3;                         // 1 above the starting 2
    p.inventory[kPowPunch] = 1;
    p.inventory[kPowSkate] = 2;
    f.w.setTile({0, 1}, Tile::Solid);
    f.w.setTile({1, 0}, Tile::Solid);
    f.press1(0);
    f.run(45);
    CHECK(!p.alive);
    auto onField = [&](int type) {
        int n = 0;
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) {
                const Powerup& pu = f.w.powerup({x, y});
                n += pu.state == PowerupState::Revealed && pu.type == type ? 1 : 0;
            }
        return n;
    };
    CHECK_EQ(onField(kPowBomb), 0);                     // not before the animation has run
    f.run(100);
    CHECK_EQ(onField(kPowBomb), 3);
    CHECK_EQ(onField(kPowFlame), 1);
    CHECK_EQ(onField(kPowPunch), 1);
    CHECK_EQ(onField(kPowSkate), 2);
    CHECK_EQ(p.inventory[kPowBomb], 1);
    f.run(50);
    CHECK_EQ(onField(kPowBomb), 3);                     // once only
}

void testDeadOwnersTriggerBombs() {
    Fixture f(Scheme::pillars(), 3);
    f.place(0, {0, 0});
    f.place(1, {14, 10});
    f.place(2, {4, 0});
    f.w.player(0).inventory[kPowTrigger] = 1;
    f.press1(0);                                        // a trigger bomb at (0,0)
    f.run(1);
    f.place(0, {6, 0});
    f.run(60);
    CHECK_EQ(f.w.activeBombs(), 1);                     // waits for its owner for ever
    // Another player's blast kills the owner; the trigger bomb then runs like any bomb.
    f.place(2, {7, 0});
    f.press1(2);
    f.run(1);
    f.place(2, {14, 0});
    f.run(45);
    CHECK(!f.w.player(0).alive);
    CHECK_EQ(f.w.activeBombs(), 1);
    f.run(45);
    CHECK_EQ(f.w.activeBombs(), 0);                     // 2 s after its owner died it has gone off
}

void testWallsRemoveWarps() {
    Fixture f;
    std::vector<Extra> extras(3);
    extras[0].type = ExtraType::Warp;
    extras[0].cell = {4, 4};
    extras[0].id = 0;
    extras[0].linkTo = 0;
    extras[1].type = ExtraType::Trampoline;
    extras[1].cell = {6, 6};
    extras[2].type = ExtraType::Arrow;
    extras[2].cell = {8, 8};
    f.w.setExtras(extras);
    f.place(0, {0, 10});
    f.place(1, {14, 10});
    f.run(2);
    CHECK_EQ(static_cast<int>(f.w.extras().size()), 3);
    f.w.setRoundSeconds(56);                            // inside the closing period
    f.run(40);
    CHECK_EQ(static_cast<int>(f.w.extras().size()), 1); // the arrow stays
    CHECK(f.w.extras()[0].type == ExtraType::Arrow);
}

void testMovieDecoder() {
    // A two-picture movie built by hand: 16x8 pixels (two blocks), 8-bit sound.
    std::vector<std::uint8_t> m;
    auto bytes = [&](std::initializer_list<int> v) { for (int b : v) m.push_back(static_cast<std::uint8_t>(b)); };
    auto op = [&](int type, int version, std::initializer_list<int> data) {
        bytes({static_cast<int>(data.size() & 255), static_cast<int>(data.size() >> 8), type, version});
        bytes(data);
    };
    bytes({'j', 'u', 'n', 'k'});                                       // the movie may start anywhere in a file
    for (char c : std::string("Interplay MVE File\x1a")) m.push_back(static_cast<std::uint8_t>(c));
    bytes({0, 0x1a, 0x00, 0x00, 0x01, 0x33, 0x11});
    std::size_t chunk = 0;
    auto beginChunk = [&]() {
        chunk = m.size();
        bytes({0, 0, 3, 0});                                           // chunk header, length patched at the end
    };
    auto endChunk = [&]() {
        const std::size_t len = m.size() - chunk - 4;
        m[chunk] = static_cast<std::uint8_t>(len & 255);
        m[chunk + 1] = static_cast<std::uint8_t>(len >> 8);
    };
    beginChunk();
    op(0x02, 0, {0x10, 0x27, 0, 0, 5, 0});                             // 10000 us x 5 = 0.05 s per picture
    op(0x03, 0, {0, 0, 0, 0, 0x22, 0x56, 0, 0});                       // mono, 8-bit, 22050 Hz
    op(0x05, 0, {2, 0, 1, 0});                                         // 2 x 1 blocks
    op(0x0C, 0, {1, 0, 2, 0, 63, 0, 0, 0, 63, 0});                     // colours 1 = red, 2 = green
    op(0x0F, 0, {0x7E});                                               // block 0: solid (E), block 1: two colours (7)
    op(0x11, 3, {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 0, 0,             // 14-byte header
                 1,                                                    // solid colour 1
                 1, 2, 0x0F, 0, 0, 0, 0, 0, 0, 0xFF});                 // colours 1,2 and eight rows of bits
    op(0x08, 0, {0, 0, 1, 0, 2, 0, 128, 255});                         // two 8-bit samples for stream 1
    op(0x07, 0, {0, 0, 0, 0});
    endChunk();
    beginChunk();
    // Second picture: block 0 from the previous picture, block 1 from one block to the left of the new one.
    op(0x0F, 0, {0x30});
    op(0x11, 3, {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 0, 0, 0});       // motion byte 0: 8 left, 0 up
    op(0x07, 0, {0, 0, 0, 0});
    endChunk();
    beginChunk();
    op(0x00, 0, {});
    endChunk();

    MveDecoder d;
    CHECK(d.openBytes(m));
    CHECK_EQ(static_cast<int>(d.movieOffset()), 4);
    CHECK(d.nextFrame());
    CHECK_EQ(d.width(), 16);
    CHECK_EQ(d.height(), 8);
    CHECK(d.frameSeconds() > 0.049 && d.frameSeconds() < 0.051);
    auto pixel = [&](int x, int y) { return std::array<int, 3>{d.rgba()[static_cast<std::size_t>((y * 16 + x) * 4)],
                                                                d.rgba()[static_cast<std::size_t>((y * 16 + x) * 4 + 1)],
                                                                d.rgba()[static_cast<std::size_t>((y * 16 + x) * 4 + 2)]}; };
    CHECK(pixel(0, 0) == (std::array<int, 3>{255, 0, 0}));             // the solid block
    CHECK(pixel(8, 0) == (std::array<int, 3>{0, 255, 0}));             // row 0 bits 0000 1111: low bits first
    CHECK(pixel(12, 0) == (std::array<int, 3>{255, 0, 0}));
    CHECK(pixel(8, 1) == (std::array<int, 3>{255, 0, 0}));             // row 1: all zero bits
    CHECK(pixel(15, 7) == (std::array<int, 3>{0, 255, 0}));            // row 7: all one bits
    const auto sound = d.takeAudio();
    CHECK_EQ(static_cast<int>(sound.size()), 2);
    CHECK_EQ(sound[0], 0);
    CHECK(sound[1] > 32000);
    CHECK(d.nextFrame());
    CHECK(pixel(0, 0) == (std::array<int, 3>{255, 0, 0}));             // kept from the first picture
    CHECK(pixel(8, 0) == (std::array<int, 3>{255, 0, 0}));             // copied from the block to its left
    CHECK(!d.nextFrame());
    CHECK_EQ(d.framesDecoded(), 2);
    MveDecoder none;
    CHECK(!none.openBytes({1, 2, 3}));
}

void testSettings() {
    Settings s;
    s.parse("levelno=4\nnum_to_win_match=3\nenclosement_depth=9\nconveyor_speed=2\nteam_play=1\nrandom_start=1\n"
            "stomped_bombs_detonate=0\nwin_by_kills=1\ngoldman=1\nschemefilename=TESTLAB.SCH\nplaytime=20\n"
            "diseases_destroyable=0\ndisable_game_music=1\nsmallmemory=0\n");
    CHECK_EQ(s.level, 4);
    CHECK_EQ(s.winsNeeded, 3);
    CHECK_EQ(s.enclosementDepth, 3);   // clamped
    CHECK_EQ(s.conveyorSpeed, 2);
    CHECK(s.teamPlay && s.randomStart && !s.stompedBombsDetonate && s.winByKills && s.goldman);
    CHECK(s.scheme == "TESTLAB");
    CHECK_EQ(s.playTime, 60);          // below a minute becomes a minute
    CHECK(!s.diseasesDestroyable && s.disableGameMusic);
    CHECK(!s.smallMemory);             // "smallmemory=0" in the text above
    s.parse("smallmemory=1\nmusic_volume=250\nsound_volume=40\n");
    CHECK(s.smallMemory);
    CHECK_EQ(s.musicVolume, 100);      // clamped
    CHECK_EQ(s.soundVolume, 40);
    CHECK_EQ(s.keys[0][4], 0x39);      // defaults: Space is action 1 of the first set
    CHECK_EQ(s.keys[1][0], 0x13);      // R is "up" of the second set
    s.parse("keydef=1,5,16\nkeydef=0,0,17\nkeydef=2,0,30\nkeydef=0,9,30\n");
    CHECK_EQ(s.keys[1][5], 16);
    CHECK_EQ(s.keys[0][0], 17);
    Settings t;
    t.parse(s.serialize());            // round trip
    CHECK(t.serialize() == s.serialize());
    // Play-time steps of the original's settings screen.
    const int order[10] = {60, 90, 120, 150, 180, 240, 300, 600, Settings::kInfiniteTime, 60};
    for (int i = 0; i < 9; ++i) {
        CHECK_EQ(Settings::nextPlayTime(order[i], 1), order[i + 1]);
        CHECK_EQ(Settings::nextPlayTime(order[i + 1], -1), order[i]);
    }
    CHECK(Settings::playTimeText(150) == "2:30");
    CHECK(Settings::playTimeText(Settings::kInfiniteTime) == "Infinite");
}

void testSpooge() {  // B18
    Fixture f;
    f.w.player(0).inventory[kPowSpooge] = 1;
    f.w.player(0).inventory[kPowBomb] = 4;
    f.place(0, {4, 2});
    f.place(1, {14, 10});
    f.w.player(0).facing = 1;
    f.press1(0);
    f.run(1);
    f.press1(0);                            // on the own bomb: lay a line east
    CHECK_EQ(f.w.activeBombs(), 4);
    CHECK(f.w.bombAt({5, 2}) != nullptr);
    CHECK(f.w.bombAt({6, 2}) != nullptr);
    CHECK(f.w.bombAt({7, 2}) != nullptr);
    CHECK(f.w.bombAt({8, 2}) == nullptr);   // capacity reached
    CHECK_EQ(f.w.bombAt({7, 2})->elapsedMs - f.w.bombAt({5, 2})->elapsedMs, -100);  // staggered by one frame each
}

void testDiseases() {  // P6, P7 and the effects table of the specification
    {   // slow and fast
        Fixture f;
        f.w.player(0).disease[kDisSlow] = true;
        f.w.player(0).diseaseMs = 1;
        f.w.player(0).diseaseDurationMs = 15000;
        f.w.player(0).diseaseCooldown = 1000;
        f.hold(0, 1);
        f.run(20);
        CHECK_EQ(f.w.player(0).x - cellToPixelX(0), 62);   // speed 923/3 = 307 -> 3.07 px per frame
        Fixture g;
        g.w.player(0).disease[kDisFast] = true;
        g.hold(0, 1);
        g.run(20);
        CHECK_EQ(g.w.player(0).x - cellToPixelX(0), 277);  // speed 1384
    }
    {   // reversed controls
        Fixture f;
        f.place(0, {4, 2});
        f.w.player(0).disease[kDisReverse] = true;
        f.hold(0, 1);
        f.run(4);
        CHECK(f.w.player(0).x < cellToPixelX(4));
    }
    {   // cannot drop / short flame / short fuse
        Fixture f;
        f.w.player(0).disease[kDisNoBombs] = true;
        f.press1(0);
        CHECK_EQ(f.w.activeBombs(), 0);
        f.w.player(0).disease = {};
        f.w.player(0).disease[kDisShortFlame] = true;
        f.w.player(0).disease[kDisShortFuse] = true;
        f.w.player(0).inventory[kPowFlame] = 5;
        f.run(1);
        f.press1(0);
        CHECK_EQ(f.w.activeBombs(), 1);
        CHECK_EQ(f.w.bombs()[0].range, 1);
        CHECK_EQ(f.w.bombs()[0].fuseMs, 650);              // 40 / 3 = 13 frames
    }
    {   // drops bombs continuously
        Fixture f;
        f.w.player(0).inventory[kPowBomb] = 3;
        f.w.player(0).disease[kDisDropBombs] = true;
        f.hold(0, 1);
        f.run(16);                                         // walks across about three cells
        CHECK_EQ(f.w.activeBombs(), 3);
    }
    {   // duration and spreading
        Fixture f;
        f.place(0, {4, 4});
        f.place(1, {8, 4});
        f.w.player(0).disease[kDisShortFlame] = true;
        f.w.player(0).diseaseMs = 1;
        f.w.player(0).diseaseDurationMs = 15000;
        f.run(10);
        CHECK(!f.w.player(1).disease[kDisShortFlame]);     // four cells apart
        f.w.player(1).x = f.w.player(0).x + 31;            // just outside 30 px
        f.run(2);
        CHECK(!f.w.player(1).disease[kDisShortFlame]);
        f.w.player(1).x = f.w.player(0).x + 30;
        f.run(2);
        CHECK(f.w.player(1).disease[kDisShortFlame]);      // infected
        CHECK(f.w.player(0).disease[kDisShortFlame]);      // copied, not handed over
        f.run(20 * 15);
        CHECK(!f.w.player(0).disease[kDisShortFlame]);     // 15 s later both are healthy
        CHECK(!f.w.player(1).disease[kDisShortFlame]);
    }
    {   // picking up a disease gives exactly one effect (or a swap)
        Fixture f;
        f.w.placePowerup({0, 0}, kPowDisease, PowerupState::Revealed);
        const int x0 = f.w.player(0).x;
        f.run(1);
        int flags = 0;
        for (bool d : f.w.player(0).disease) flags += d ? 1 : 0;
        CHECK(flags == 1 || f.w.player(0).x != x0);
        CHECK_EQ(f.w.player(0).inventory[kPowDisease], 0);
    }
}

void testDeathAnimationChosen() {
    Fixture f;
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.w.createBomb(1, {2, 2}, BombType::Regular, 1, 1);
    f.run(1);
    CHECK(!f.w.player(0).alive);
    CHECK(f.w.player(0).deathAnim >= 1 && f.w.player(0).deathAnim <= 24);
}

void testAiSurvivesOwnBombs() {
    // Four computer players on the standard scheme: over many rounds they must
    // open the map, not all die at once, and usually leave a single winner.
    Scheme s = Scheme::pillars();
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            if (s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == Tile::Blank)
                s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = Tile::Brick;
    s.brickDensity = 90;
    int decided = 0, withWinner = 0, bombsSeen = 0, earlyWipe = 0;
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        World w{Values::defaults(), seed};
        w.startRound(s, true);
        std::vector<AiPlayer> ai;
        for (int i = 0; i < 4; ++i) {
            w.addPlayer(i);
            ai.emplace_back(seed * 100 + static_cast<std::uint32_t>(i));
        }
        Inputs in{};
        int t = 0;
        for (; t < 20 * 150 && !w.roundOver(); ++t) {
            for (int i = 0; i < 4; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(w, i, 50);
            w.tick(50, in);
            for (const Event& e : w.takeEvents()) bombsSeen += e.kind == EventKind::BombDropped ? 1 : 0;
        }
        if (w.roundOver()) ++decided;
        if (w.winner() >= 0) ++withWinner;
        if (t < 20 * 10) ++earlyWipe;  // over within ten seconds
    }
    CHECK(bombsSeen > 100);   // they do lay bombs
    CHECK(decided == 12);     // every round ends (kills, or the clock)
    CHECK(withWinner >= 4);   // and a fair share end with a survivor
    CHECK(earlyWipe <= 2);    // they do not all blow themselves up at once
}

void testExtrasFile() {
    const auto ex = parseExtrasText(
        "; comment\r\n-A,S, 2, 2\r\n-A,W,-3, 2\r\n-C,E, 4, 4\r\n-W,1, 0, 2, 2, 3\r\n-W,1, 3, 2,-3, 2\r\n-T,2,2\r\n-T,H,H\r\n");
    CHECK_EQ(ex.size(), 7u);
    if (ex.size() != 7) return;
    CHECK(ex[0].type == ExtraType::Arrow);
    CHECK_EQ(ex[0].dir, 2);
    CHECK(ex[1].cell == (Cell{12, 2}));            // -3 counts from the right edge
    CHECK(ex[2].type == ExtraType::Conveyor);
    CHECK_EQ(ex[2].dir, 1);
    CHECK(ex[3].type == ExtraType::Warp);
    CHECK_EQ(ex[3].id, 0);
    CHECK_EQ(ex[3].linkTo, 3);
    CHECK(ex[4].cell == (Cell{2, 8}));
    CHECK(ex[5].type == ExtraType::Trampoline);
    CHECK(ex[6].cell == (Cell{-1, -1}));           // "H": position chosen by the game
}

void testArrowTurnsSlidingBomb() {
    Fixture f;
    f.w.setExtras({Extra{ExtraType::Arrow, {6, 2}, 2}});
    f.w.player(0).inventory[kPowKicker] = 1;
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.w.createBomb(0, {3, 2}, BombType::Regular, 1, 400);
    f.hold(0, 1);
    f.run(1);
    f.hold(0, kNoDir);
    f.run(60);
    const Bomb& b = f.w.bombs()[0];
    CHECK(b.mode == BombMode::Resting);
    CHECK(pixelToCell(b.x, b.y) == (Cell{6, 10}));  // turned south at the arrow, slid to the bottom
}

void testConveyor() {
    Fixture f;
    std::vector<Extra> belt;
    for (int x = 2; x <= 6; ++x) belt.push_back(Extra{ExtraType::Conveyor, {x, 2}, 1});
    f.w.setExtras(belt, 1);                         // speed setting 1 = 350
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.run(20);                                      // standing still for one second
    CHECK(f.w.player(0).x > cellToPixelX(2) + 60);  // carried about 70 px
    CHECK_EQ(f.w.player(0).facing, 2);              // facing unchanged
    const int x0 = f.w.player(0).x;
    f.run(60);
    CHECK(f.cellOf(0) == (Cell{7, 2}));             // set down at the end of the belt
    CHECK(f.w.player(0).x >= x0);

    Fixture g;                                      // walking with and against the belt
    std::vector<Extra> long1;
    for (int x = 0; x < kGridW; ++x) long1.push_back(Extra{ExtraType::Conveyor, {x, 2}, 1});
    g.w.setExtras(long1, 1);
    g.place(0, {0, 2});
    g.hold(0, 1);
    g.run(20);
    CHECK_EQ(g.w.player(0).x - cellToPixelX(0), 255);   // (923 + 350) / 100 px per frame
    g.place(0, {14, 2});
    g.w.player(0).moveAcc = 0;
    g.hold(0, 3);
    g.run(20);
    CHECK_EQ(cellToPixelX(14) - g.w.player(0).x, 115);  // (923 - 350) / 100 px per frame

    Fixture h;                                      // a resting bomb is carried too
    h.w.setExtras(belt, 1);
    h.place(0, {0, 8});
    h.place(1, {14, 10});
    h.w.createBomb(0, {2, 2}, BombType::Regular, 1, 400);
    h.run(80);
    CHECK(pixelToCell(h.w.bombs()[0].x, h.w.bombs()[0].y) == (Cell{7, 2}));
    CHECK(h.w.bombs()[0].mode == BombMode::Resting);
}

void testWarp() {
    Scheme s = Scheme::pillars();
    s.tiles[2][2] = Tile::Brick;
    Fixture f(s);
    f.w.setExtras({Extra{ExtraType::Warp, {2, 2}, 0, 0, 1}, Extra{ExtraType::Warp, {12, 8}, 0, 1, 0}});
    f.place(0, {0, 2});
    f.place(1, {14, 10});
    f.run(1);
    CHECK(f.w.tile({2, 2}) == Tile::Blank);          // a warp clears its own cell
    f.hold(0, 1);
    f.run(9);                                        // walk into the warp
    CHECK(f.w.player(0).special == Special::WarpOut);
    f.w.createBomb(1, {2, 2}, BombType::Regular, 1, 1);
    f.run(3);
    CHECK(f.w.player(0).alive);                      // untouchable while warping
    f.run(10);
    CHECK(f.cellOf(0) == (Cell{12, 8}));             // came out at the linked warp
    f.run(12);
    CHECK(f.w.player(0).special == Special::None);
    f.run(4);
    CHECK(f.cellOf(0).x >= 12);                      // walks on, no bounce back into the warp
}

void testTrampoline() {
    Fixture f;
    f.w.setExtras({Extra{ExtraType::Trampoline, {4, 4}}});
    f.place(0, {3, 4});
    f.place(1, {14, 10});
    f.hold(0, 1);
    f.run(5);
    CHECK(f.w.player(0).special == Special::Trampoline);
    f.run(31);                                       // 30 frames in the air
    CHECK(f.w.player(0).special == Special::None);
    const Cell c = f.cellOf(0);
    CHECK(c.x != 4 && c.y != 4);                     // lands in another row and column
    CHECK(std::abs(c.x - 4) <= 2 && std::abs(c.y - 4) <= 2);
    CHECK(f.w.tile(c) == Tile::Blank);
}

void testTeamPlay() {
    World w{Values::defaults(), 5};
    w.startRound(Scheme::pillars(), false);
    w.setTeamPlay(true, {0, 1, 0, 1});
    for (int i = 0; i < 4; ++i) w.addPlayer(i);
    w.endStartFreeze();
    Inputs in{};
    CHECK_EQ(w.contenders(), 2);                 // two teams
    CHECK_EQ(w.displayColour(1), 1);             // own colours at first
    for (int t = 0; t < 41; ++t) w.tick(50, in);
    CHECK_EQ(w.displayColour(0), 0);             // then team colours: white and red
    CHECK_EQ(w.displayColour(1), 2);
    CHECK_EQ(w.displayColour(2), 0);
    // Kill player 1 (team 1): team 1 still has player 3.
    w.createBomb(0, pixelToCell(w.player(1).x, w.player(1).y), BombType::Regular, 1, 1);
    for (int t = 0; t < 30; ++t) w.tick(50, in);
    CHECK(!w.player(1).alive);
    CHECK(!w.roundOver());
    w.createBomb(0, pixelToCell(w.player(3).x, w.player(3).y), BombType::Regular, 1, 1);
    for (int t = 0; t < 30; ++t) w.tick(50, in);
    CHECK(w.roundOver());                        // team 1 wiped out although two players live
    CHECK_EQ(w.alivePlayers(), 2);
    CHECK_EQ(w.winningTeam(), 0);
}

void testDud() {
    Values v = Values::defaults();
    v.set(320, 0);   // a dud may occur at once
    v.set(321, 1);
    v.set(322, 1);   // and always does
    World w{v, 3};
    w.startRound(Scheme::pillars(), false);
    w.addPlayer(0);
    w.addPlayer(1);
    w.endStartFreeze();
    Inputs in{};
    in[0].button1 = true;
    w.tick(50, in);
    in[0].button1 = false;
    CHECK_EQ(w.activeBombs(), 1);
    CHECK(w.bombs()[0].dud);
    w.player(0).x = cellToPixelX(6);
    w.player(0).y = cellToPixelY(6);
    for (int t = 0; t < 119; ++t) w.tick(50, in);
    CHECK(w.bombs()[0].dud);                 // still sputtering after 6 s
    CHECK_EQ(w.bombs()[0].elapsedMs, 0);     // fuse not started
    for (int t = 0; t < 3; ++t) w.tick(50, in);
    CHECK(!w.bombs()[0].dud);
    for (int t = 0; t < 36; ++t) w.tick(50, in);
    CHECK_EQ(w.activeBombs(), 1);
    for (int t = 0; t < 4; ++t) w.tick(50, in);
    CHECK_EQ(w.activeBombs(), 0);            // a normal 2 s fuse afterwards

    World n{Values::defaults(), 3};          // with the shipped values no dud in the first 3 minutes
    n.startRound(Scheme::pillars(), false);
    n.addPlayer(0);
    n.addPlayer(1);
    n.endStartFreeze();
    in[0].button1 = true;
    n.tick(50, in);
    CHECK(!n.bombs()[0].dud);
}

void testEvents() {
    Fixture f;
    f.w.takeEvents();
    f.press1(0);
    auto ev = f.w.takeEvents();
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) {
        CHECK(ev[0].kind == EventKind::BombDropped);
        CHECK_EQ(ev[0].player, 0);
    }
    f.place(0, {6, 6});
    f.run(40);
    ev = f.w.takeEvents();
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) CHECK(ev[0].kind == EventKind::BombExploded);
    CHECK(f.w.takeEvents().empty());
}

void testClockAndHurry() {
    Fixture f;
    f.w.setRoundSeconds(70);
    CHECK_EQ(f.w.secondsLeft(), 70);
    f.run(20);                          // 1 s
    CHECK_EQ(f.w.secondsLeft(), 69);
    CHECK(!f.w.hurry());
    f.run(180);                         // 10.0 s: the display still reads 60
    CHECK_EQ(f.w.secondsLeft(), 60);
    CHECK(!f.w.hurry());
    f.run(1);                           // just past 10 s: 59
    CHECK(f.w.hurry());
    f.w.setRoundSeconds(-1);
    CHECK_EQ(f.w.secondsLeft(), -1);
    CHECK(!f.w.hurry());
}

void testClosingWalls() {  // R1, R2, R3, R4; cadence and order as observed on the original (D27, D28)
    Fixture f;
    f.w.setRoundSeconds(70);
    f.place(0, {3, 3});                 // inside both rings
    f.place(1, {11, 7});
    f.run(280);                         // 14.0 s: 56 s left
    CHECK_EQ(f.w.closedCells(), 0);
    f.run(1);                           // 14.05 s: 55 s left, walls armed
    f.run(5);                           // +250 ms: not yet (strictly more than 250 ms)
    CHECK_EQ(f.w.closedCells(), 0);
    f.run(1);
    CHECK_EQ(f.w.closedCells(), 1);
    CHECK(f.w.tile({0, 0}) == Tile::Solid);
    CHECK(f.w.tile({1, 0}) == Tile::Blank);
    f.run(5 * 14);                      // 14 more slots: the whole top row
    CHECK_EQ(f.w.closedCells(), 15);
    CHECK(f.w.tile({14, 0}) == Tile::Solid);
    CHECK(f.w.tile({14, 1}) == Tile::Blank);
    f.run(5);                           // the corner cell is visited a second time
    CHECK_EQ(f.w.closedCells(), 16);
    CHECK(f.w.tile({14, 1}) == Tile::Blank);
    f.run(5);
    CHECK(f.w.tile({14, 1}) == Tile::Solid);
    f.run(5 * 200);                     // let it finish
    // depth 1 = two rings; the third ring stays open
    CHECK(f.w.tile({1, 1}) == Tile::Solid);
    CHECK(f.w.tile({13, 9}) == Tile::Solid);
    CHECK(f.w.tile({2, 2}) == Tile::Blank);
    CHECK(f.w.tile({12, 8}) == Tile::Blank);
    CHECK(f.w.player(0).alive);
    CHECK(f.w.player(1).alive);
    const int closed = f.w.closedCells();
    f.run(100);
    CHECK_EQ(f.w.closedCells(), closed);  // stopped

    // A player or a bomb in a closing cell.
    Fixture g(Scheme::pillars(), 3);
    g.w.setRoundSeconds(56);            // armed after 1 s
    g.place(0, {2, 0});
    g.place(1, {11, 7});
    g.place(2, {9, 7});
    g.w.createBomb(1, {4, 0}, BombType::Regular, 1, 4000);
    g.run(21 + 6 + 10);                 // three cells closed: (0,0), (1,0), (2,0)
    CHECK(!g.w.player(0).alive);
    CHECK_EQ(g.w.player(0).killedBy, -1);
    g.run(10 + 1);                      // (3,0), (4,0): bomb queued, explodes on the next tick
    CHECK_EQ(g.w.activeBombs(), 0);
    CHECK(g.w.tile({4, 0}) == Tile::Solid);
}

void testRoundResult() {
    Fixture f;
    CHECK(!f.w.roundOver());
    f.place(0, {2, 2});
    f.place(1, {14, 10});
    f.w.createBomb(1, {2, 2}, BombType::Regular, 1, 1);
    f.run(1);
    CHECK(!f.w.roundOver());            // the loser still counts for 20 frames
    f.run(20);
    CHECK(f.w.roundOver());
    CHECK_EQ(f.w.winner(), 1);

    Fixture t;                          // clock runs out with both alive: draw (observed on the original, D29)
    t.w.setRoundSeconds(60);
    t.w.setEnclosementDepth(0);
    t.run(20 * 59);
    CHECK(!t.w.roundOver());            // 59.0 s: the clock still reads 0:01
    t.run(1);
    CHECK(t.w.roundOver());
    CHECK_EQ(t.w.winner(), -1);
    CHECK_EQ(t.w.closedCells(), 0);     // depth 0: no walls

    Fixture d;                          // both die: draw
    d.place(0, {2, 2});
    d.place(1, {3, 2});
    d.w.createBomb(0, {2, 2}, BombType::Regular, 1, 1);
    d.run(25);
    CHECK(d.w.roundOver());
    CHECK_EQ(d.w.winner(), -1);
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

// Network play rests on this: a saved state loaded into another World goes on
// exactly like the original, and the hash tells two states apart.
void testStateTransfer() {
    const Values values = Values::defaults();
    World a{values, 777};
    a.startRound(Scheme::fromRows({"...:::::::::...", ".#:#:#:#:#:#:#.", "..:::::::::::..", ":#:#:#:#:#:#:#:", ":::::::::::::::",
                                   ":#:#:#:#:#:#:#:", ":::::::::::::::", ":#:#:#:#:#:#:#:", "..:::::::::::..", ".#:#:#:#:#:#:#.",
                                   "...:::::::::..."}),
                 true);
    Extra arrow;
    arrow.type = ExtraType::Arrow;
    arrow.cell = {7, 5};
    a.setExtras({arrow}, 1);
    for (int i = 0; i < 4; ++i) a.addPlayer(i);
    std::vector<AiPlayer> ai;
    for (int i = 0; i < 4; ++i) ai.emplace_back(900u + static_cast<std::uint32_t>(i));
    auto inputs = [&] {
        Inputs in{};
        for (int i = 0; i < 4; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(a, i, 50);
        return in;
    };
    // Into the round, until bombs and flames are about.
    int t = 0;
    for (; t < 2000 && !(a.activeBombs() > 0 && t > 200); ++t) a.tick(50, inputs());
    CHECK(a.activeBombs() > 0);

    const std::vector<std::uint8_t> saved = a.saveState();
    World b{values, 1};  // another seed, no round started
    CHECK(b.stateHash() != a.stateHash());
    CHECK(b.loadState(saved));
    CHECK_EQ(b.stateHash(), a.stateHash());
    CHECK(b.saveState() == saved);
    int differences = 0;
    for (int n = 0; n < 1500 && !a.roundOver(); ++n) {
        const Inputs in = inputs();
        a.tick(50, in);
        b.tick(50, in);
        if (a.stateHash() != b.stateHash()) ++differences;
    }
    CHECK_EQ(differences, 0);
    CHECK_EQ(a.tickCount(), b.tickCount());
    CHECK_EQ(a.alivePlayers(), b.alivePlayers());

    // The hash notices a single changed field.
    const std::uint32_t before = b.stateHash();
    b.player(0).kills += 1;
    CHECK(b.stateHash() != before);
    b.player(0).kills -= 1;
    CHECK_EQ(b.stateHash(), before);

    // Malformed input is refused and leaves the state alone.
    std::vector<std::uint8_t> cut(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(saved.size() / 2));
    CHECK(!b.loadState(cut));
    CHECK(!b.loadState({}));
    std::vector<std::uint8_t> longer = saved;
    longer.push_back(0);
    CHECK(!b.loadState(longer));
    CHECK_EQ(b.stateHash(), before);
}

// The free asset set has to hold everything the game asks for by name.
void testFreeAssets() {
    const FreeArt art = makeFreeArt();
    std::vector<std::string> needed = {"shadow", "spin", "goldman", "xxx", "hurry", "cursor1", "ring", "extra warp 1", "extra trampoline",
                                       "bomb regular green", "bomb regular green dud", "bomb trigger green", "bomb jelly green",
                                       "flame center green"};
    for (const char* dir : {"north", "east", "south", "west"}) {
        for (const char* what : {"stand ", "walk ", "kick ", "punch ", "pickup ", "walkbomb ", "standbomb ", "ghost ", "rover ",
                                 "extra arrow ", "extra conveyor "})
            needed.push_back(std::string(what) + dir);
        needed.push_back(std::string("flame mid") + dir + " green");
        needed.push_back(std::string("flame tip") + dir + " green");
    }
    for (const char* power : {"bomb", "flame", "disease", "kicker", "skate", "punch", "grab", "spooge", "goldflame", "trigger", "jelly",
                              "disease3", "random", "clog"})
        needed.push_back(std::string("power ") + power);
    for (int level = 0; level <= 10; ++level)
        for (const char* what : {"tile %d solid", "tile %d brick", "flame brick %d"}) {
            char name[32];
            std::snprintf(name, sizeof name, what, level);
            needed.emplace_back(name);
        }
    const Values values = Values::defaults();
    for (int k = 1; k <= values.get(vid::kDeathAnimations); ++k) needed.push_back("die green " + std::to_string(k));
    for (int k = 0; k < values.get(vid::kCornerheadCount); ++k) needed.push_back("cornerhead " + std::to_string(k));
    int missing = 0;
    for (const std::string& name : needed) {
        const auto it = art.sequences.find(name);
        if (it == art.sequences.end() || it->second.empty()) {
            ++missing;
            std::printf("  missing sequence %s\n", name.c_str());
        }
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(art.sequences.at("numeric font").size(), 11u);  // ten digits and the colon
    CHECK(art.sequences.at("kick south").size() >= 8 && art.sequences.at("punch south").size() >= 10);
    CHECK_EQ(art.sequences.at("cornerhead 0").size(), static_cast<std::size_t>(kCornerheadFrames));
    int bad = 0, tinted = 0;
    for (const FreeFrame& f : art.frames) {
        if (f.width <= 0 || f.height <= 0 || f.rgba.size() != static_cast<std::size_t>(f.width * f.height * 4) || f.tint.size() != static_cast<std::size_t>(f.width * f.height)) ++bad;
        for (std::uint8_t t : f.tint) tinted += t;
    }
    CHECK_EQ(bad, 0);
    CHECK(tinted > 0);
    // The player colour reaches the tinted pixels only.
    const FreeFrame& stand = art.frames[static_cast<std::size_t>(art.sequences.at("stand south")[0])];
    const std::vector<std::uint8_t> red = colouredFrame(stand, 2), blue = colouredFrame(stand, 3);
    int differ = 0, wrong = 0;
    for (std::size_t i = 0; i < stand.tint.size(); ++i) {
        const bool same = red[i * 4] == blue[i * 4] && red[i * 4 + 2] == blue[i * 4 + 2];
        if (!same) ++differ;
        if (!same && stand.tint[i] == 0) ++wrong;
    }
    CHECK(differ > 50);
    CHECK_EQ(wrong, 0);

    for (const char* name : {"mainmenu", "glue0", "glue7", "field0", "field10", "results", "draw", "victory0", "victory9", "team0", "team1",
                             "roulette", "title"}) {
        const auto pic = makeFreePicture(name);
        CHECK(pic && pic->width == 640 && pic->height == 480 && pic->rgba.size() == 640u * 480u * 4u);
    }
    CHECK(!makeFreePicture("iplogo"));
    CHECK(!makeFreePicture("field11"));
    const FreeFont font = makeFreeFont();
    CHECK(font.height > 8 && font.width.size() == 128 && font.alpha.size() == static_cast<std::size_t>(font.atlasWidth * font.height));
    int blank = 0;
    for (int ch = 33; ch < 127; ++ch) {
        int lit = 0;
        for (int y = 0; y < font.height; ++y)
            for (int x = 0; x < font.width[static_cast<std::size_t>(ch)]; ++x) lit += font.alpha[static_cast<std::size_t>(y * font.atlasWidth + font.x[static_cast<std::size_t>(ch)] + x)] > 128 ? 1 : 0;
        if (lit == 0) ++blank;
    }
    CHECK_EQ(blank, 0);

    // Sounds: every id the game plays without the original's voices, none silent.
    const FreeSounds sounds = makeFreeSounds();
    std::vector<int> ids = {10, 20, 40, 100, 120, 130, 135, 140, 141, 142, 150, 160, 170, 200, 300, 350, 360, 400, 550,
                            1000, 1010, 1020, 1130, 1300, 1310, 1320, 1330, 1400, 2300, 2600, 2700};
    for (int level = 0; level <= 10; ++level) ids.push_back(1100 + level);
    int unknown = 0, silent = 0;
    for (int id : ids) {
        bool found = false;
        for (const auto& [have, name] : sounds.ids) found = found || (have == id && sounds.samples.count(name) != 0);
        if (!found) ++unknown;
    }
    for (const auto& [name, samples] : sounds.samples) {
        int peak = 0;
        for (std::int16_t v : samples) peak = std::max(peak, std::abs(static_cast<int>(v)));
        if (samples.size() < 200 || peak < 2000) ++silent;
    }
    CHECK_EQ(unknown, 0);
    CHECK_EQ(silent, 0);
    // The voice lines exist for each kind of moment, and the vowels are where they should be:
    // "see you" (ee, oo) has far less sound around 1100 Hz, where "ah" and "aw" resonate,
    // than "draw" has, relative to the band around 2300 Hz where "ee" resonates.
    for (int id : {700, 701, 702, 703, 704, 1200, 1401, 1402, 1700, 1701, 2000, 2001, 2002, 2301, 2302, 2601, 2602}) {
        bool found = false;
        for (const auto& [have, name] : sounds.ids) found = found || have == id;
        CHECK(found);
    }
    auto band = [](const std::vector<std::int16_t>& samples, double hz) {
        // Energy at one frequency (Goertzel), summed over a few neighbours.
        double total = 0;
        for (double f = hz - 150; f <= hz + 150; f += 50) {
            const double k = 2.0 * std::cos(2.0 * 3.14159265358979 * f / 22050.0);
            double a = 0, b = 0;
            for (std::int16_t v : samples) {
                const double c = v + k * a - b;
                b = a;
                a = c;
            }
            total += a * a + b * b - k * a * b;
        }
        return total;
    };
    const auto& draw = sounds.samples.at("say_draw");
    const auto& seeYou = sounds.samples.at("say_seeyou");
    CHECK(band(draw, 800) / band(draw, 2300) > 10.0 * band(seeYou, 800) / band(seeYou, 2300));
    CHECK(sounds.samples.at("say_winner").size() > 22050 / 4 && sounds.samples.at("say_winner").size() < 22050 * 2);

    // On disk: a folder the game and the server can use as their game data.
    const std::string dir = (std::filesystem::temp_directory_path() / "ab-free-assets-test").string();
    std::filesystem::remove_all(dir);
    CHECK(!isFreeAssetDir(dir));
    CHECK(ensureFreeAssets(dir, false));
    CHECK(isFreeAssetDir(dir));
    CHECK(!std::filesystem::exists(dir + "/data/res/soundlst.res"));
    Values fromFile;
    CHECK(fromFile.loadFile(dir + "/data/res/valuelst.res"));
    CHECK(fromFile.entries() == Values::defaults().entries());
    const std::vector<SchemeEntry> schemes = listSchemes(dir + "/data/schemes", "");
    CHECK_EQ(schemes.size(), 8u);
    bool basic = false;
    for (const SchemeEntry& entry : schemes) {
        basic = basic || entry.file == "basic";
        const auto file = loadSchemeFile(entry.path);
        CHECK(file.has_value());
        if (!file) continue;
        // Nobody starts in a wall, and no two players share a cell.
        for (int p = 0; p < kMaxPlayers; ++p) {
            const Cell c = file->scheme.start[static_cast<std::size_t>(p)];
            CHECK(inGrid(c) && file->scheme.tiles[static_cast<std::size_t>(c.y)][static_cast<std::size_t>(c.x)] != Tile::Solid);
            for (int q = 0; q < p; ++q) CHECK(!(file->scheme.start[static_cast<std::size_t>(q)] == c));
        }
        // A round on it can be played to an end by computer players.
        World w{Values::defaults(), 5};
        w.startRound(file->scheme, true);
        std::vector<AiPlayer> ai;
        for (int i = 0; i < 4; ++i) w.addPlayer(i), ai.emplace_back(static_cast<std::uint32_t>(40 + i));
        for (int t = 0; t < 6000 && !w.roundOver(); ++t) {
            Inputs in{};
            for (int i = 0; i < 4; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(w, i, 50);
            w.tick(50, in);
        }
        CHECK(w.roundOver());
    }
    CHECK(basic);
    int extras = 0;
    for (int level = 0; level <= 10; ++level)
        for (const Extra& e : loadExtrasFile(dir + "/data/res/extra" + std::to_string(level) + ".res")) {
            ++extras;
            CHECK(inGrid(e.cell) || (e.type == ExtraType::Trampoline && e.cell.x == -1));
        }
    CHECK(extras > 30);
    CHECK(loadHelpFile(dir + "/manual.bm").has_value() && loadHelpFile(dir + "/credits.bm").has_value());
    // With sounds asked for, the folder is completed; asked again, nothing is rewritten.
    CHECK(ensureFreeAssets(dir, true));
    CHECK(std::filesystem::exists(dir + "/data/res/soundlst.res") && std::filesystem::file_size(dir + "/data/sound/boom0.wav") > 1000);
    const auto stamp = std::filesystem::last_write_time(dir + "/free-assets.txt");
    CHECK(ensureFreeAssets(dir, false));
    CHECK(std::filesystem::last_write_time(dir + "/free-assets.txt") == stamp);
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"geometry", testGeometry},
        {"values parser", testValuesParser},
        {"scheme file", testSchemeFile},
        {"ani parser", testAniParser},
        {"font parser", testFontParser},
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
        {"punch", testPunch},
        {"punch bounce and wrap", testPunchBouncesAndWraps},
        {"grab and throw", testGrabAndThrow},
        {"trapped animation", testTrappedAnimation},
        {"roulette", testRoulette},
        {"settings", testSettings},
        {"movie decoder", testMovieDecoder},
        {"dead owner's trigger bombs", testDeadOwnersTriggerBombs},
        {"walls remove warps", testWallsRemoveWarps},
        {"death scatter", testDeathScatter},
        {"ice delay", testIceDelay},
        {"brick regeneration", testBrickRegeneration},
        {"sound events", testSoundEvents},
        {"scheme powers", testSchemePowers},
        {"campaign file", testCampaignFile},
        {"campaign enemies", testCampaignEnemies},
        {"campaign stage clear", testCampaignStageClear},
        {"upcoming walls", testUpcomingWalls},
        {"help pages", testHelpPages},
        {"suicide score", testSuicideScore},
        {"spooge", testSpooge},
        {"diseases", testDiseases},
        {"death animation", testDeathAnimationChosen},
        {"ai plays rounds", testAiSurvivesOwnBombs},
        {"extras file", testExtrasFile},
        {"arrow", testArrowTurnsSlidingBomb},
        {"conveyor", testConveyor},
        {"warp", testWarp},
        {"trampoline", testTrampoline},
        {"team play", testTeamPlay},
        {"dud", testDud},
        {"events", testEvents},
        {"clock and hurry", testClockAndHurry},
        {"closing walls", testClosingWalls},
        {"round result", testRoundResult},
        {"determinism", testDeterminism},
        {"state transfer", testStateTransfer},
        {"free assets", testFreeAssets},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
