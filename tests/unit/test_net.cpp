// Tests for the network mode (docs/specifications/networking.md): the message
// encoding, and a server with clients in one process over 127.0.0.1. Time is
// simulated (the server and clients take the clock as an argument), so a whole
// round runs in well under a second.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include "free/free_data.hpp"
#include "game/roulette.hpp"
#include "net/client.hpp"
#include "net/crypto.hpp"
#include "net/link.hpp"
#include "net/protocol.hpp"
#include "net/server.hpp"
#include "net/snapshot.hpp"
#include "game/ai.hpp"
#include "net/upnp.hpp"
#include "resources/settings.hpp"

using namespace ab;
using namespace ab::net;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        ++g_checks;                                                                                   \
        const auto va = (a);                                                                          \
        const auto vb = (b);                                                                          \
        if (!(va == vb)) {                                                                            \
            ++g_failures;                                                                             \
            std::printf("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,       \
                        static_cast<long long>(va), static_cast<long long>(vb));                      \
        }                                                                                             \
    } while (0)

void testCodec() {
    PlayerInput in;
    in.dir = {true, false, false, true};
    in.button2 = true;
    const PlayerInput back = unpackInput(packInput(in));
    CHECK(back.dir == in.dir && back.button1 == in.button1 && back.button2 == in.button2);
    CHECK_EQ(packInput(in), 1 | 8 | 32);

    RoundStartMsg start;
    start.roundId = 7;
    start.seed = 0xDEADBEEFu;
    start.values = Values::defaults().entries();
    start.winsNeeded = 3;
    start.score.wins[2] = 1;
    start.score.kills[4] = -2;
    RoundSetup& s = start.setup;
    s.level = 7;
    s.scheme = Scheme::pillars();
    s.scheme.brickDensity = 80;
    s.scheme.start[3] = {14, 10};
    s.powers[1].bornWith = 4;
    s.powers[9].hasOverride = true;
    s.powers[9].overrideValue = -5;
    s.powers[2].forbidden = true;
    Extra warp;
    warp.type = ExtraType::Warp;
    warp.cell = {3, 4};
    warp.id = 2;
    warp.linkTo = 1;
    Extra tramp;
    tramp.type = ExtraType::Trampoline;
    tramp.cell = {-1, -1};
    s.extras = {warp, tramp};
    s.teamPlay = true;
    s.teams = {0, 1, 0, 1, 1, 0, 0, 0, 1, 1};
    s.playTime = kInfinitePlayTime;
    s.enclosementDepth = 3;
    s.conveyorSpeed = 2;
    s.campaign = true;
    s.ghosts = 3, s.ghostSpeed = 150, s.rovers = 2, s.roverSpeed = 400;
    s.prize[3] = kPowSkate;
    s.present = {true, true, false, true};
    s.human = {true, false, false, true};
    const std::vector<std::uint8_t> bytes = encoded(start);
    RoundStartMsg got;
    {
        ByteReader r(bytes);
        CHECK(decode(r, got) && r.atEnd());
    }
    CHECK(encoded(got) == bytes);
    CHECK_EQ(got.setup.level, 7);
    CHECK_EQ(got.setup.extras.size(), 2u);
    CHECK_EQ(got.setup.extras[1].cell.x, -1);
    CHECK_EQ(got.setup.powers[9].overrideValue, -5);
    CHECK_EQ(got.score.kills[4], -2);
    CHECK(got.setup.present[3] && !got.setup.present[2] && !got.setup.human[1]);
    CHECK(got.setup.campaign && got.setup.ghosts == 3 && got.setup.roverSpeed == 400 && got.setup.prize[3] == kPowSkate && got.setup.prize[0] == -1);
    // Two worlds built from the two copies are the same world.
    {
        Values values = Values::defaults();
        World a{values, start.seed}, b{values, got.seed};
        applyRoundSetup(a, values, start.setup);
        applyRoundSetup(b, values, got.setup);
        CHECK_EQ(a.stateHash(), b.stateHash());
    }
    // Every truncation is refused, none crashes.
    int accepted = 0;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        ByteReader r(cut);
        RoundStartMsg m;
        if (decode(r, m)) ++accepted;
    }
    CHECK_EQ(accepted, 0);
    // Out-of-range content is refused.
    {
        std::vector<std::uint8_t> bad = bytes;
        bad[4 + 4 + 2 + start.values.size() * 8 + 1] = 9;  // a tile value
        ByteReader r(bad);
        RoundStartMsg m;
        CHECK(!decode(r, m));
    }

    LobbyState lobby;
    lobby.phase = Phase::Result;
    lobby.admin = 3;
    lobby.settings.level = -1;
    lobby.settings.schemeTitle = "X marks the spot";
    lobby.settings.playTime = kInfinitePlayTime;
    lobby.seats[0] = {SeatKind::Human, 3, 0, 1};
    lobby.seats[1] = {SeatKind::Computer, 0, 0, 0};
    lobby.seats[4] = {SeatKind::Human, 3, 1, 0};  // the second player at Ann's computer
    lobby.settings.goldman = true;
    lobby.settings.campaign = 2;
    lobby.settings.campaignTitle = "night-shift";
    lobby.clients.push_back({3, "Ann", 0, 2, 42});
    lobby.clients.push_back({5, "Watcher", -1, 1, 7});
    LobbyState lobby2;
    {
        const std::vector<std::uint8_t> lb = encoded(lobby);
        ByteReader r(lb);
        CHECK(decode(r, lobby2) && r.atEnd());
        CHECK(encoded(lobby2) == lb);
    }
    CHECK_EQ(lobby2.settings.level, -1);
    CHECK(lobby2.seatName(0) == "Ann");
    CHECK(lobby2.seatName(1) == "Computer");
    CHECK(lobby2.seatName(2).empty());
    CHECK(lobby2.seatName(4) == "Ann (2)");
    CHECK(lobby2.settings.goldman && lobby2.settings.campaign == 2 && lobby2.settings.campaignTitle == "night-shift");
    CHECK_EQ(lobby2.clients[0].players, 2);
    CHECK_EQ(lobby2.clients[1].seat, -1);

    DeltaMsg delta;
    delta.roundId = 9;
    delta.step = 100;
    delta.baseStep = 97;
    delta.events.push_back({98, {EventKind::BombDropped, 3, 0}});
    delta.events.push_back({100, {EventKind::DiseaseGot, -1, 70000}});
    delta.delta = {1, 2, 3, 250};
    DeltaMsg delta2;
    {
        const std::vector<std::uint8_t> sb = encoded(delta);
        ByteReader r(sb);
        CHECK(decode(r, delta2) && r.atEnd());
        // Cut short anywhere, it is refused.
        for (std::size_t cut = 0; cut < sb.size(); ++cut) {
            const std::vector<std::uint8_t> part(sb.begin(), sb.begin() + static_cast<std::ptrdiff_t>(cut));
            ByteReader pr(part);
            DeltaMsg none;
            CHECK(!decode(pr, none));
        }
    }
    CHECK(delta2.step == 100 && delta2.baseStep == 97 && delta2.delta == delta.delta);
    CHECK_EQ(delta2.events.size(), 2u);
    CHECK(delta2.events[0].step == 98 && delta2.events[0].event.kind == EventKind::BombDropped && delta2.events[0].event.player == 3);
    CHECK(delta2.events[1].step == 100 && delta2.events[1].event.player == -1 && delta2.events[1].event.value == 70000);
    {
        // A difference from a later state, or an event of a kind or player that does not exist: refused.
        DeltaMsg bad = delta;
        bad.baseStep = 100;
        std::vector<std::uint8_t> sb = encoded(bad);
        ByteReader r(sb);
        DeltaMsg none;
        CHECK(!decode(r, none));
        sb = encoded(delta);
        sb[14] = 200;  // the first event's kind
        ByteReader r2(sb);
        CHECK(!decode(r2, none));
    }
    InputMsg input;
    input.roundId = 4;
    input.number = 77;
    input.haveState = true;
    input.haveStep = 55;
    input.step = 60;
    input.inputs = {{1, 2, 3, 4}, {0xff, 0, 0, 0}};
    InputMsg input2;
    {
        const std::vector<std::uint8_t> sb = encoded(input);
        ByteReader r(sb);
        CHECK(decode(r, input2) && r.atEnd());
    }
    CHECK(input2.number == 77 && input2.haveState && input2.haveStep == 55 && input2.step == 60 && input2.inputs.size() == 2);
    CHECK(input2.inputs[0][3] == 4 && input2.inputs[1][0] == 0x3f);  // only the six keys get through

    CHECK(cleanText("  hi\x01 there\n ", 100) == "hi there");
    CHECK(cleanText("abcdef", 3) == "abc");
    CHECK(cleanText("a\xC3\xA9z", 2) == "a");  // not cut inside a character
    CHECK(resolveHostPort("127.0.0.1:99999", 1) == std::nullopt);
    CHECK(resolveHostPort("127.0.0.1", 5)->port == 5);
    CHECK(resolveHostPort("127.0.0.1:1234", 5)->port == 1234);
    CHECK(resolveHostPort("127.0.0.1", 5)->ip == kLoopback);
    // IPv6 literals, with and without brackets and port.
    const auto six = resolveHostPort("[::1]:99", 5);
    CHECK(six && six->v6 && six->port == 99 && six->loopback() && six->text() == "[::1]:99");
    CHECK(resolveHostPort("::1", 5) && resolveHostPort("::1", 5)->v6 && resolveHostPort("::1", 5)->port == 5);
    CHECK(resolveHostPort("[::1]", 7) && resolveHostPort("[::1]", 7)->port == 7);
    CHECK(resolveHostPort("[::1", 7) == std::nullopt);
    CHECK(resolveHostPort("[::1]x", 7) == std::nullopt);
    CHECK(!resolveHostPort("127.0.0.1", 5)->v6 && resolveHostPort("127.0.0.1", 5)->loopback());
    // A link-local address keeps its interface; "lo" exists everywhere the tests run on Linux, 1 everywhere.
    if (const auto local = resolveHostPort("[fe80::1%1]:9", 5)) {
        CHECK(local->v6 && local->zone == 1 && local->port == 9);
        CHECK(local->text() == "[fe80::1%1]:9");
    }
}

// Two ends of a link joined by a line that loses, repeats and delays packets.
struct Line {
    Link a, b;
    Rng rng{99};
    int lossPercent = 0, repeatPercent = 0, delayPercent = 0;
    std::uint64_t now = 1000;
    struct Flying {
        std::uint64_t at;
        Bytes packet;
        bool toB;
    };
    std::vector<Flying> flying;
    int sentPackets = 0;
    std::size_t sentBytes = 0;

    Line() {
        Key ab{}, ba{};
        randomBytes(ab.data(), 32);
        randomBytes(ba.data(), 32);
        a.start(ab, ba, 77, now);
        b.start(ba, ab, 77, now);
    }
    void carry(const Bytes& packet, bool toB) {
        ++sentPackets;
        sentBytes += packet.size();
        if (rng.below(100) < lossPercent) return;
        const int copies = rng.below(100) < repeatPercent ? 2 : 1;
        for (int i = 0; i < copies; ++i) flying.push_back({now + 10 + (rng.below(100) < delayPercent ? static_cast<std::uint64_t>(rng.below(80)) : 0), packet, toB});
    }
    void turn() {
        a.update(now, [&](const Bytes& p) { carry(p, true); });
        b.update(now, [&](const Bytes& p) { carry(p, false); });
        now += 5;
        for (std::size_t i = 0; i < flying.size();) {
            if (flying[i].at > now) {
                ++i;
                continue;
            }
            (flying[i].toB ? b : a).receive(flying[i].packet, now);
            flying.erase(flying.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
};

// The link: messages complete and in order whatever the line does; datagrams as they come.
void testLink() {
    auto message = [](int n) {
        // Sizes from nothing to several packets, contents that show a mix-up.
        Bytes m(static_cast<std::size_t>((n * 677) % 4200));
        for (std::size_t i = 0; i < m.size(); ++i) m[i] = static_cast<std::uint8_t>(n * 31 + static_cast<int>(i) * 7);
        return m;
    };
    for (int kind = 0; kind < 3; ++kind) {
        Line line;
        if (kind >= 1) line.lossPercent = 40;
        if (kind == 2) line.repeatPercent = 30, line.delayPercent = 50;
        const int count = 300;
        int sentA = 0, gotB = 0, sentB = 0, gotA = 0;
        bool order = true;
        for (int turns = 0; turns < 40000 && (gotB < count || gotA < count); ++turns) {
            // A few at a time, both ways, while the line is at work.
            if (sentA < count && turns % 3 == 0) line.a.sendMessage(static_cast<std::uint8_t>(sentA), message(sentA)), ++sentA;
            if (sentB < count && turns % 5 == 0) line.b.sendMessage(static_cast<std::uint8_t>(sentB), message(sentB + 1000)), ++sentB;
            line.turn();
            std::uint8_t type = 0;
            Bytes payload;
            while (line.b.nextMessage(type, payload)) order = order && type == static_cast<std::uint8_t>(gotB) && payload == message(gotB), ++gotB;
            while (line.a.nextMessage(type, payload)) order = order && type == static_cast<std::uint8_t>(gotA) && payload == message(gotA + 1000), ++gotA;
        }
        CHECK_EQ(gotB, count);
        CHECK_EQ(gotA, count);
        CHECK(order);
        CHECK(!line.a.broken() && !line.b.broken());
        // Everything is acknowledged in the end.
        for (int i = 0; i < 2000 && (!line.a.idle() || !line.b.idle()); ++i) line.turn();
        CHECK(line.a.idle() && line.b.idle());
        if (kind == 0) CHECK(line.a.rttMs() >= 5 && line.a.rttMs() <= 40);  // the line takes 10 ms each way
    }
    {
        // Datagrams: none is held back by a lost one or by a message being repeated; a lost
        // one stays lost; none arrives twice.
        Line line;
        line.lossPercent = 40;
        line.repeatPercent = 30;
        int got = 0, last = -1;
        bool twice = false;
        std::vector<bool> seen(400, false);
        std::uint64_t worst = 0;
        std::vector<std::uint64_t> sentAt(400, 0);
        for (int n = 0; n < 400; ++n) {
            ByteWriter w;
            w.u32(static_cast<std::uint32_t>(n));
            CHECK(line.a.sendDatagram(9, w.data()));
            sentAt[static_cast<std::size_t>(n)] = line.now;
            if (n % 10 == 0) line.a.sendMessage(1, Bytes(3000, 1));
            line.turn();
            std::uint8_t type = 0;
            Bytes payload;
            while (line.b.nextDatagram(type, payload)) {
                ByteReader r(payload);
                const auto id = static_cast<int>(r.u32());
                CHECK(type == 9 && r.ok() && id >= 0 && id < 400);
                twice = twice || seen[static_cast<std::size_t>(id)];
                seen[static_cast<std::size_t>(id)] = true;
                worst = std::max(worst, line.now - sentAt[static_cast<std::size_t>(id)]);
                last = id;
                ++got;
            }
        }
        CHECK(!twice);
        CHECK(got > 180 && got < 300);  // about 60 % of 400
        CHECK(last > 390);
        CHECK(worst <= 20);             // the line's 10 ms and a turn: never the wait for a repeat
        CHECK(!line.a.sendDatagram(9, Bytes(Link::kMaxDatagram + 1)));
        CHECK(line.a.sendDatagram(9, Bytes(Link::kMaxDatagram)));
    }
    {
        // A packet altered, from another connection, or played again is not taken.
        Line line;
        std::vector<Bytes> packets;
        line.a.sendMessage(5, {1, 2, 3});
        line.a.update(line.now, [&](const Bytes& p) { packets.push_back(p); });
        CHECK(!packets.empty());
        CHECK(packets[0].size() <= Link::kMaxPacket);
        Bytes bad = packets[0];
        bad.back() ^= 1;
        CHECK(!line.b.receive(bad, line.now));
        Bytes other = packets[0];
        other[5] ^= 1;  // the connection's number
        CHECK(!line.b.receive(other, line.now));
        CHECK(line.b.receive(packets[0], line.now));
        CHECK(!line.b.receive(packets[0], line.now));
        std::uint8_t type = 0;
        Bytes payload;
        CHECK(line.b.nextMessage(type, payload) && type == 5 && payload.size() == 3);
        CHECK(!line.b.nextMessage(type, payload));
        // Going away is told to the other side, after what was still to be delivered.
        line.a.sendMessage(6, Bytes(2500, 7));
        line.a.close();
        for (int i = 0; i < 400 && !line.b.closedByPeer(); ++i) line.turn();
        CHECK(line.b.closedByPeer());
        CHECK(line.b.nextMessage(type, payload) && type == 6 && payload.size() == 2500);
        CHECK(line.a.idle());
        // A silent line: a packet a second all the same, so that silence means the other side is gone.
        Line quiet;
        for (int i = 0; i < 1000; ++i) quiet.turn();  // five seconds
        CHECK(quiet.sentPackets >= 8 && quiet.sentPackets <= 14);
        CHECK(quiet.now - quiet.a.lastHeard() < 1100);
    }
}

// Snapshots: a world rebuilt from the row of numbers, or from differences, is the public
// state it was taken from; and how large the differences are in ten-player rounds.
// The same match, the same state and the same bytes on every machine: the values below were
// recorded on x86-64, and a build for another processor (32-bit, ARM) has to give them too, or
// its players could not play with the others. A deterministic round of four computer players,
// then the world's hash, its saved state, its snapshot row and an encoded message.
void testSameOnEveryMachine() {
    const auto hex = [](const Key& k) {
        std::string out;
        for (std::size_t i = 0; i < 8; ++i) {
            char two[3];
            std::snprintf(two, sizeof two, "%02x", k[i]);
            out += two;
        }
        return out;
    };
    Scheme scheme = Scheme::pillars();
    scheme.brickDensity = 85;
    World w{Values::defaults(), 4242};
    w.startRound(scheme, true);
    std::vector<AiPlayer> ai;
    for (int i = 0; i < 4; ++i) {
        w.addPlayer(i);
        ai.emplace_back(31u + static_cast<std::uint32_t>(i));
    }
    for (int t = 0; t < 900 && !w.roundOver(); ++t) {
        std::array<PlayerInput, kMaxPlayers> in{};
        for (int i = 0; i < 4; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(w, i, 50);
        w.tick(50, in);
    }
    const std::vector<std::uint8_t> state = w.saveState();
    const Bytes row = encodeDelta({}, w.snapshotFields());
    RoundStartMsg start;
    start.roundId = 9;
    start.seed = 0x01234567u;
    start.values = Values::defaults().entries();
    start.winsNeeded = 3;
    start.setup.scheme = scheme;
    start.setup.level = 4;
    start.setup.playTime = 150;
    const Bytes message = encoded(start);
    const std::string got = std::to_string(w.tickCount()) + " " + std::to_string(w.stateHash()) + " " + hex(sha256(state.data(), state.size())) + " " +
                            hex(sha256(row.data(), row.size())) + " " + hex(sha256(message.data(), message.size()));
    std::printf("  this machine: %s\n", got.c_str());
    CHECK(got == "900 1801455735 10bc37464c7587e2 2baa5e5076ab2f40 1a532e5ddff5d12e");
}

void testSnapshotFields() {
    // The coder by itself.
    {
        const Fields a = {5, -3, 0, 1000000, 7, 7, 7};
        const Fields b = {5, -4, 0, 1000050, 7, 7, 9, -1, 0};
        Fields out;
        CHECK(decodeDelta(a, encodeDelta(a, b), out) && out == b);
        CHECK(decodeDelta(b, encodeDelta(b, a), out) && out == a);  // shorter than the base
        CHECK(decodeDelta({}, encodeDelta({}, b), out) && out == b);
        CHECK(encodeDelta(a, a).size() == 1);
        const Fields wide = {INT64_MIN, INT64_MAX, -1};
        CHECK(decodeDelta(b, encodeDelta(b, wide), out) && out == wide);
        // Nonsense is refused: a place beyond the row, a row beyond the limit, a cut-off number.
        CHECK(!decodeDelta(a, Bytes{3, 5, 2}, out));
        CHECK(!decodeDelta(a, Bytes{0xff, 0xff, 0xff, 0x7f}, out));
        CHECK(!decodeDelta(a, Bytes{7, 0, 0x80}, out));
    }
    std::size_t steps = 0, full = 0, fullMost = 0;
    const int ages[4] = {1, 2, 5, 20};
    std::size_t sum[4] = {}, most[4] = {}, over[4] = {};
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        World w{Values::defaults(), seed};
        Scheme s = Scheme::pillars();
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x)
                if (s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == Tile::Blank && (x + y) % 3 != 0)
                    s.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = Tile::Brick;
        w.startRound(s, true);
        std::vector<AiPlayer> ai;
        for (int i = 0; i < kMaxPlayers; ++i) {
            w.addPlayer(i);
            ai.emplace_back(seed * 100 + static_cast<std::uint32_t>(i));
        }
        int hidden = 0;
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) hidden += w.powerup({x, y}).state == PowerupState::Hidden ? 1 : 0;
        CHECK(hidden > 5);  // there is something to keep to oneself
        World copy{Values::defaults(), 999};  // what a client holds: built anew from every snapshot
        std::deque<Fields> history;
        std::array<PlayerInput, kMaxPlayers> in{};
        for (int t = 0; t < 20 * 150 && !w.roundOver(); ++t) {
            for (int i = 0; i < kMaxPlayers; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(w, i, 50);
            w.tick(50, in);
            w.takeEvents();
            const Fields now = w.snapshotFields();
            const Bytes whole = encodeDelta({}, now);
            full += whole.size();
            fullMost = std::max(fullMost, whole.size());
            for (int k = 0; k < 4; ++k) {
                if (history.size() < static_cast<std::size_t>(ages[k])) continue;
                const Bytes d = encodeDelta(history[history.size() - static_cast<std::size_t>(ages[k])], now);
                sum[k] += d.size();
                most[k] = std::max(most[k], d.size());
                over[k] += d.size() > 1000 ? 1 : 0;
            }
            // Rebuilt from the difference to the step before, the copy is the public state.
            Fields rebuilt;
            const bool decoded = decodeDelta(history.empty() ? Fields{} : history.back(), encodeDelta(history.empty() ? Fields{} : history.back(), now), rebuilt);
            if (!decoded || rebuilt != now || !copy.loadSnapshotFields(rebuilt) || copy.stateHash(true) != w.stateHash(true)) {
                CHECK(decoded && rebuilt == now);
                CHECK(copy.stateHash(true) == w.stateHash(true));
                return;
            }
            history.push_back(now);
            if (history.size() > 32) history.pop_front();
            ++steps;
        }
        // Nothing hidden is in a snapshot: the copy has no powerup under any brick.
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) CHECK(copy.powerup({x, y}).state != PowerupState::Hidden);
    }
    CHECK(steps > 1000);
    std::printf("     snapshots over %zu steps of ten-player rounds: whole state %zu bytes on average (%zu at most)\n", steps, full / steps, fullMost);
    for (int k = 0; k < 4; ++k)
        std::printf("     difference from %2d step(s) before: %zu bytes on average, %zu at most, %zu over 1000 bytes\n", ages[k], sum[k] / steps, most[k], over[k]);
    CHECK(sum[0] / steps < 300);
    CHECK(sum[2] / steps < 600);
}

// A server and its clients on one simulated clock.
struct Harness {
    Server server;
    std::vector<std::unique_ptr<Client>> clients;
    std::uint64_t now = 100000;
    std::vector<std::string> log;
    std::string gameDir;  // game data for the server (empty: the built-in arena)
    bool alwaysReady = true;  // clients say "ready" by themselves (testReady switches it off)
    std::vector<Event> heard;  // every event the first client was given

    bool start(const std::string& password = "", int computers = 1) {
        ServerConfig config;
        config.port = 0;
        config.seed = 4242;
        config.password = password;
        config.discoverable = false;
        config.gameDir = gameDir;
        config.settings.computers = computers;
        config.settings.enclosementDepth = 3;
        config.settings.playTime = 60;
        config.settings.winsNeeded = 1;
        config.log = [this](const std::string& line) { log.push_back(line); };
        return server.start(config);
    }
    Client& join(const std::string& name, const std::string& password = "", bool udp = true) {
        // The tests count on seats and the administrator role going in the order of the
        // joins. Two connections opened in the same instant may reach the server in either
        // order (seen on macOS), so the ones before are let in first.
        for (int waited = 0; waited < 1000 && connecting(); waited += 5) turn();
        clients.push_back(std::make_unique<Client>());
        clients.back()->setAlwaysReady(alwaysReady);
        if (!udp) clients.back()->disableUdp();
        clients.back()->connect("127.0.0.1:" + std::to_string(server.port()), name, password, now);
        return *clients.back();
    }
    bool connecting() const {
        for (const auto& c : clients)
            if (c->state() == Client::State::Connecting) return true;
        return false;
    }
    void turn() {
        server.update(now);
        for (auto& c : clients) {
            c->update(now);
            if (c == clients.front()) c->advance(now, nullptr, [this](const World&, const std::vector<Event>& events) { heard.insert(heard.end(), events.begin(), events.end()); });
            else c->advance(now, nullptr, nullptr);
        }
        now += 5;
        pause();
    }
    // The clock here is made up (5 ms a turn) but the sockets are real. On Linux and Windows
    // a datagram sent over the loopback interface can be read at once; on macOS it is handed
    // over a moment later, which at full speed is many made-up milliseconds: a connection
    // then looks as if it had seconds of delay. A short real pause per turn keeps the two
    // clocks close enough there.
    static void pause() {
#ifdef __APPLE__
        std::this_thread::sleep_for(std::chrono::microseconds(150));
#else
        std::this_thread::yield();
#endif
    }
    bool until(const std::function<bool()>& done, int maxMs = 20000) {
        for (int waited = 0; waited < maxMs; waited += 5) {
            if (done()) return true;
            turn();
        }
        return done();
    }
    void spin(int ms) {
        for (int waited = 0; waited < ms; waited += 5) turn();
    }
};

void testLobby() {
    Harness h;
    CHECK(h.start("secret"));
    CHECK(h.server.port() != 0);

    Client& ann = h.join("Ann", "secret");
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    CHECK(ann.isAdmin());
    CHECK_EQ(ann.seat(), 0);
    CHECK(ann.lobby().seats[0].kind == SeatKind::Human);
    CHECK(ann.lobby().seats[1].kind == SeatKind::Computer);
    CHECK(ann.lobby().seats[2].kind == SeatKind::Empty);

    Client& nopass = h.join("Mallory");
    CHECK(h.until([&] { return nopass.state() == Client::State::Failed; }));
    CHECK(nopass.error() == "This server needs a password");
    Client& wrong = h.join("Mallory", "guess");
    CHECK(h.until([&] { return wrong.state() == Client::State::Failed; }));
    CHECK(wrong.error() == "Wrong password");
    CHECK_EQ(h.server.players(), 1);

    // The second player takes the computer's seat number 1; the computer moves down.
    Client& bob = h.join("Bob", "secret");
    CHECK(h.until([&] { return bob.state() == Client::State::Lobby && bob.lobby().clients.size() == 2; }));
    CHECK(!bob.isAdmin());
    CHECK_EQ(bob.seat(), 1);
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2; }));
    CHECK(ann.lobby().seats[2].kind == SeatKind::Computer);
    CHECK(ann.lobby().seatName(1) == "Bob");

    // A second "Ann" gets another name.
    Client& ann2 = h.join("Ann", "secret");
    CHECK(h.until([&] { return ann2.state() == Client::State::Lobby && ann2.lobby().client(ann2.id()) != nullptr; }));
    CHECK(ann2.lobby().client(ann2.id())->name == "Ann 2");

    // Chat reaches everyone, with the sender's name as the server knows it.
    bob.sendChat("hello there");
    CHECK(h.until([&] {
        for (const ChatLine& line : ann.chat())
            if (!line.fromServer && line.name == "Bob" && line.text == "hello there") return true;
        return false;
    }));
    bool joinedNotice = false;
    for (const ChatLine& line : ann.chat()) joinedNotice = joinedNotice || (line.fromServer && line.text == "Bob has joined");
    CHECK(joinedNotice);

    // Only the administrator changes settings.
    bob.sendOption(Option::Wins, 1);
    h.spin(200);
    CHECK_EQ(ann.lobby().settings.winsNeeded, 1);
    ann.sendOption(Option::Wins, 1);
    ann.sendOption(Option::Level, -1);
    ann.sendOption(Option::TeamPlay, 1);
    CHECK(h.until([&] { return bob.lobby().settings.winsNeeded == 2 && bob.lobby().settings.level == -1 && bob.lobby().settings.teamPlay; }));
    // Anyone changes their own team.
    const int before = bob.lobby().seats[1].team;
    bob.sendTeam();
    CHECK(h.until([&] { return ann.lobby().seats[1].team == 1 - before; }));

    // More computer players fill the free seats; the administrator removes a player.
    for (int i = 0; i < 12; ++i) ann.sendOption(Option::Computers, 1);
    CHECK(h.until([&] { return ann.lobby().settings.computers == 9; }));
    int computers = 0;
    for (const Seat& s : ann.lobby().seats) computers += s.kind == SeatKind::Computer ? 1 : 0;
    CHECK_EQ(computers, 7);
    bob.sendKick(ann.id());  // not the administrator: nothing happens
    ann.sendKick(ann2.id());
    CHECK(h.until([&] { return ann2.state() == Client::State::Failed; }));
    CHECK(ann2.error() == "Removed by the administrator");
    CHECK(ann.state() == Client::State::Lobby);

    // The administrator hands the role over; the new one hands it back.
    bob.sendAdmin(ann.id());  // not the administrator: nothing happens
    h.spin(200);
    CHECK(ann.isAdmin());
    ann.sendAdmin(bob.id());
    CHECK(h.until([&] { return bob.isAdmin() && !ann.isAdmin(); }));
    ann.sendOption(Option::Wins, 1);
    h.spin(200);
    CHECK_EQ(bob.lobby().settings.winsNeeded, 2);  // Ann's settings no longer count
    bob.sendAdmin(ann.id());
    CHECK(h.until([&] { return ann.isAdmin(); }));
    // No administrator password is set on this server: logging in is refused.
    bob.sendLogin("letmein");
    h.spin(200);
    CHECK(ann.isAdmin());

    // When the administrator leaves, the next player takes over.
    ann.disconnect();
    CHECK(h.until([&] { return bob.isAdmin() && bob.lobby().clients.size() == 1; }));
    CHECK_EQ(bob.seat(), 1);
}

// Plays rounds until every client shows a result; returns false on a timeout.
bool playToResult(Harness& h, const std::vector<Client*>& clients) {
    return h.until(
        [&] {
            for (Client* c : clients)
                if (c->state() != Client::State::Result) return false;
            return true;
        },
        400000);
}

void testRound(bool udp) {
    Harness h;
    CHECK(h.start("", 2));
    Client& ann = h.join("Ann", "", udp);
    Client& bob = h.join("Bob", "", udp);
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && bob.state() == Client::State::Lobby && ann.lobby().clients.size() == 2; }));
    if (udp) CHECK(h.until([&] { return ann.udpActive() && bob.udpActive(); }));
    h.spin(300);

    // Too few players is refused with the reason, in the chat.
    bob.sendStart();  // not the administrator
    h.spin(200);
    CHECK(h.server.phase() == Phase::Lobby);
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    CHECK(h.server.phase() == Phase::Round);
    CHECK_EQ(ann.world()->alivePlayers(), 4);

    // Both walk about and drop bombs; the inputs arrive at the server.
    PlayerInput walk;
    walk.dir[1] = true;
    ann.setInput(walk);
    CHECK(h.until([&] { return ann.stepsApplied() > 60 && bob.stepsApplied() > 60; }));
    CHECK(h.server.world()->player(0).x > cellToPixelX(0) || h.server.world()->player(0).facing == 1);
    ann.setInput({});

    // Whatever a client does to the state it holds lasts until the server's next word:
    // the state is the server's, not worked out by the client.
    bob.world()->player(0).kills += 5;
    const std::uint32_t tampered = bob.stepsApplied();
    CHECK(h.until([&] { return bob.stepsApplied() > tampered; }));
    CHECK_EQ(bob.world()->player(0).kills, h.server.world()->player(0).kills);
    CHECK_EQ(ann.snapshotsLoaded() + bob.snapshotsLoaded(), 0);  // differences were enough all along

    // Someone who connects now watches the round from here.
    Client& cat = h.join("Cat", "", udp);
    CHECK(h.until([&] { return cat.state() == Client::State::Round && cat.stepsApplied() > 0; }));
    CHECK_EQ(cat.seat(), -1);
    CHECK_EQ(cat.snapshotsLoaded(), 0);  // one whole state to come in by, none after
    // Nothing a player may not know is on any client: the powerups under the bricks are the server's alone.
    int hiddenOnClients = 0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            for (Client* c : {&ann, &bob, &cat}) hiddenOnClients += c->world()->powerup({x, y}).state == PowerupState::Hidden ? 1 : 0;
    CHECK_EQ(hiddenOnClients, 0);

    // Rounds until the match is decided (one win is enough here); after each, every
    // client holds exactly the server's state and scores.
    bool decided = false;
    int rounds = 0;
    while (!decided && rounds < 20) {
        ++rounds;
        CHECK(playToResult(h, {&ann, &bob, &cat}));
        CHECK(h.server.phase() == Phase::Result);
        const std::uint32_t hash = h.server.world()->stateHash(true);
        CHECK_EQ(ann.world()->stateHash(true), hash);
        CHECK_EQ(bob.world()->stateHash(true), hash);
        CHECK_EQ(cat.world()->stateHash(true), hash);
        CHECK_EQ(ann.stepsApplied(), h.server.steps());
        CHECK_EQ(cat.stepsApplied(), h.server.steps());
        CHECK_EQ(ann.udpActive(), udp);
        CHECK(ann.score().wins == h.server.score().wins);
        CHECK(cat.score().kills == h.server.score().kills);
        CHECK_EQ(ann.score().matchWinner, h.server.score().matchWinner);
        decided = ann.score().matchWinner >= 0;
        if (decided) CHECK_EQ(ann.result().winner, ann.score().matchWinner);
        // Everyone presses a key: the next round, or the lobby once the match is decided.
        const std::uint32_t round = ann.roundId();
        ann.sendContinue();
        bob.sendContinue();
        if (!decided) CHECK(h.until([&] { return ann.state() == Client::State::Round && cat.state() == Client::State::Round && ann.roundId() == round + 1; }));
    }
    CHECK(decided);
    CHECK_EQ(ann.snapshotsLoaded() + bob.snapshotsLoaded() + cat.snapshotsLoaded(), 0);
    // The round's traffic was datagrams, each within one packet.
    CHECK(ann.deltasSeen() > 100);
    CHECK(ann.largestDelta() <= Link::kMaxDatagram);
    // Back in the lobby the watcher gets a seat.
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && cat.state() == Client::State::Lobby && cat.seat() >= 0; }));
    CHECK(ann.world() == nullptr);
    CHECK(h.server.phase() == Phase::Lobby);
    for (const std::string& line : h.log) CHECK(line.rfind("ERROR", 0) != 0);
}

// A player who leaves in the middle is played by the computer; the others go on.
// The administrator can start only when every other player has said "ready"; the
// question is asked again after each match.
void testReady() {
    Harness h;
    h.alwaysReady = false;
    CHECK(h.start("", 0));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    Client& cat = h.join("Cat");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 3 && bob.state() == Client::State::Lobby && cat.state() == Client::State::Lobby; }));
    CHECK(ann.isAdmin());
    CHECK(!bob.ready());

    ann.sendStart();
    CHECK(h.until([&] { return !ann.chat().empty() && ann.chat().back().text == "Not ready yet: Bob, Cat"; }));
    CHECK(h.server.phase() == Phase::Lobby);
    bob.sendReady(true);
    CHECK(h.until([&] { return bob.ready() && ann.lobby().client(bob.id())->ready; }));
    ann.sendStart();
    CHECK(h.until([&] { return ann.chat().back().text == "Not ready yet: Cat"; }));
    // Ready can be taken back.
    bob.sendReady(false);
    CHECK(h.until([&] { return !bob.ready(); }));
    bob.sendReady(true);
    cat.sendReady(true);
    CHECK(h.until([&] { return bob.ready() && cat.ready(); }));
    ann.sendStart();  // the administrator's own "ready" is not asked for
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    bob.sendReady(false);  // not in the lobby: ignored
    // Back in the lobby everybody is asked again.
    ann.leaveMatch();
    bob.leaveMatch();
    cat.leaveMatch();
    CHECK(h.until([&] { return h.server.phase() == Phase::Lobby && bob.state() == Client::State::Lobby && cat.state() == Client::State::Lobby; }));
    CHECK(h.until([&] { return !bob.ready() && !cat.ready(); }));
    // A client set to answer by itself (the bot) is ready without being asked.
    Client& dan = h.join("Dan");
    dan.setAlwaysReady(true);
    CHECK(h.until([&] { return dan.state() == Client::State::Lobby && dan.ready(); }));
}

void testLeaveDuringRound() {
    Harness h;
    CHECK(h.start("", 1));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
    ann.sendStart();
    CHECK(h.until([&] { return bob.stepsApplied() > 30; }));
    ann.disconnect();
    CHECK(h.until([&] { return bob.isAdmin(); }));
    CHECK(h.server.phase() == Phase::Round);
    CHECK(bob.lobby().seats[0].kind == SeatKind::Computer);
    CHECK(playToResult(h, {&bob}));
    CHECK_EQ(bob.world()->stateHash(true), h.server.world()->stateHash(true));
    // The last human leaves: the server goes back to the lobby and frees the seats.
    bob.disconnect();
    CHECK(h.until([&] { return h.server.phase() == Phase::Lobby && h.server.players() == 0; }));
    CHECK(h.server.world() == nullptr);
}

// Four in ten datagrams lost in both directions: every step still arrives, because
// each message repeats what has not been acknowledged. No snapshot is needed.
void testUdpLoss() {
    Harness h;
    CHECK(h.start("", 3));
    Client& ann = h.join("Ann");
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && ann.udpActive(); }));
    setTestUdpLoss(40);
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round; }));
    PlayerInput in;
    in.dir[2] = true;
    ann.setInput(in);
    const bool finished = playToResult(h, {&ann});
    setTestUdpLoss(0);
    CHECK(finished);
    CHECK(ann.udpActive());
    CHECK_EQ(ann.snapshotsLoaded(), 0);
    CHECK_EQ(ann.stepsApplied(), h.server.steps());
    CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK(h.server.steps() > 100);
}

// A line that loses four packets in ten from the first packet on: joining, the lobby, chat
// and a whole round still work, and nothing arrives twice.
void testLossFromTheStart() {
    Harness h;
    CHECK(h.start("pass", 2));
    setTestUdpLoss(40);
    Client& ann = h.join("Ann", "pass");
    Client& bob = h.join("Bob", "pass");
    const bool joined = h.until([&] { return ann.state() == Client::State::Lobby && bob.state() == Client::State::Lobby && ann.lobby().clients.size() == 2 && bob.ready(); }, 60000);
    CHECK(joined);
    // (Not checked: that both are on UDP. With four packets in ten lost a client may hear
    // nothing for two seconds and go on inside TCP, as it is meant to; seen on macOS.)
    // One after the other has arrived (the server takes one chat line per 300 ms from a
    // player, and a line held up by a lost packet could otherwise arrive on the heels of the next).
    bool chatted = true;
    for (int i = 0; i < 5; ++i) {
        bob.sendChat("line " + std::to_string(i));
        chatted = chatted && h.until([&] {
            int lines = 0;
            for (const ChatLine& line : ann.chat()) lines += !line.fromServer ? 1 : 0;
            return lines == i + 1;
        }, 60000);
        h.spin(400);
    }
    CHECK(chatted);
    int n = 0;
    bool order = true;
    for (const ChatLine& line : ann.chat())
        if (!line.fromServer) order = order && line.text == "line " + std::to_string(n++);
    CHECK(order);
    ann.sendStart();
    const bool started = h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }, 60000);
    const bool finished = started && playToResult(h, {&ann, &bob});
    setTestUdpLoss(0);
    CHECK(started);
    CHECK(finished);
    if (!started) {
        // What stood where, for a failure that only shows on another machine.
        std::printf("  FAIL (details) ann: state=%d udp=%d rtt=%d waiting=%zu  bob: state=%d udp=%d rtt=%d waiting=%zu  server: phase=%d players=%d\n",
                    static_cast<int>(ann.state()), ann.udpActive() ? 1 : 0, ann.linkRttMs(), ann.linkUnacknowledged(), static_cast<int>(bob.state()), bob.udpActive() ? 1 : 0,
                    bob.linkRttMs(), bob.linkUnacknowledged(), static_cast<int>(h.server.phase()), h.server.players());
        for (const Client* c : {&ann, &bob})
            if (!c->chat().empty()) std::printf("  FAIL (details) last line seen by %s: %s\n", c == &ann ? "ann" : "bob", c->chat().back().text.c_str());
        if (ann.state() == Client::State::Failed || bob.state() == Client::State::Failed)
            std::printf("  FAIL (details) errors: ann \"%s\" bob \"%s\"\n", ann.error().c_str(), bob.error().c_str());
    }
    if (!finished) return;
    CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(bob.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(ann.snapshotsLoaded() + bob.snapshotsLoaded(), 0);
    // What happened is told once each, however many datagrams were lost: every death was heard
    // of exactly once, and every bomb that went off had been heard dropping.
    int deaths = 0, dead = 0, dropped = 0, exploded = 0;
    for (const Event& e : h.heard) {
        deaths += e.kind == EventKind::PlayerDied ? 1 : 0;
        dropped += e.kind == EventKind::BombDropped || e.kind == EventKind::BombPooped ? 1 : 0;
        exploded += e.kind == EventKind::BombExploded ? 1 : 0;
    }
    for (int i = 0; i < kMaxPlayers; ++i) dead += ann.world()->player(i).present && !ann.world()->player(i).alive ? 1 : 0;
    CHECK(dead >= 3);
    CHECK_EQ(deaths, dead);
    CHECK(exploded > 0 && exploded <= dropped);
}

// A client cut off for five seconds in the middle of a round has fallen further behind
// than differences reach: it is given the whole state once and goes on from there.
void testLeftBehind() {
    Harness h;
    CHECK(h.start("", 3));
    Client& ann = h.join("Ann");
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && ann.stepsApplied() > 40; }));
    CHECK_EQ(ann.snapshotsLoaded(), 0);
    setTestUdpLoss(100);
    const std::uint32_t before = ann.stepsApplied();
    h.spin(5000);
    CHECK(ann.stepsApplied() <= before + 2);
    CHECK(h.server.steps() > before + 80);
    setTestUdpLoss(0);
    const bool caughtUp = h.until([&] { return ann.state() != Client::State::Round || ann.stepsApplied() + 3 > h.server.steps(); }, 5000);
    CHECK(caughtUp);
    // (The one sent while the line was dead arrives too, out of date by then: two at most.)
    CHECK(ann.snapshotsLoaded() >= 1 && ann.snapshotsLoaded() <= 2);
    const int wholeStates = ann.snapshotsLoaded();
    CHECK(ann.state() == Client::State::Round || ann.state() == Client::State::Result);
    const bool finished = playToResult(h, {&ann});
    CHECK(finished);
    if (finished) CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(ann.snapshotsLoaded(), wholeStates);  // differences again from then on
}

// A network that lets no UDP through at all: after two seconds without an answer the
// client tries the same inside a TCP connection, and plays there.
void testNoUdpPath() {
    Harness h;
    CHECK(h.start("", 2));
    setTestUdpLoss(100);
    Client& ann = h.join("Ann");
    const bool joined = h.until([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); });
    CHECK(joined);
    CHECK(!ann.udpActive());
    CHECK(ann.encrypted());
    ann.sendStart();
    const bool finished = h.until([&] { return ann.state() == Client::State::Round; }) && playToResult(h, {&ann});
    setTestUdpLoss(0);
    CHECK(finished);
    if (finished) CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
}

// The first packet of a stranger costs the server nothing: it answers with a cookie and
// keeps no memory of it. Only who repeats the cookie from that address gets further.
void testCookie() {
    Harness h;
    CHECK(h.start());
    UdpSocket raw;
    CHECK(raw.open(0));
    const Address server{kLoopback, h.server.port()};
    auto key = [&](const Bytes& cookie) {
        ByteWriter w;
        w.u32(kUdpMagic);
        w.u8(kClientKey);
        w.u16(kProtocolVersion);
        Key secret{};
        randomBytes(secret.data(), 32);
        for (std::uint8_t b : x25519Base(secret)) w.u8(b);
        for (std::uint8_t b : cookie) w.u8(b);
        while (w.data().size() < 96) w.u8(0);
        return w.data();
    };
    auto answer = [&]() {
        Address from;
        Bytes data;
        for (int i = 0; i < 400; ++i) {
            h.turn();
            if (raw.receiveFrom(from, data)) return data;
        }
        return Bytes{};
    };
    CHECK(raw.sendTo(server, key(Bytes(kCookieBytes, 0))));
    const Bytes first = answer();
    CHECK_EQ(first.size(), 5 + kCookieBytes);
    CHECK(first.size() < 96);  // shorter than what was sent
    if (first.size() != 5 + kCookieBytes) return;
    CHECK_EQ(first[4], kCookie);
    // A cookie made up is no better than none.
    CHECK(raw.sendTo(server, key(Bytes(kCookieBytes, 7))));
    const Bytes second = answer();
    CHECK(second.size() == 5 + kCookieBytes && second[4] == kCookie);
    // The right one, from the address it was given to: the server's key comes back.
    CHECK(raw.sendTo(server, key(Bytes(first.begin() + 5, first.end()))));
    const Bytes third = answer();
    CHECK(third.size() == 5 + 32 + 32 + 4 && third[4] == kServerKey);
    // From another address the same cookie is worth nothing.
    UdpSocket other;
    CHECK(other.open(0));
    CHECK(other.sendTo(server, key(Bytes(first.begin() + 5, first.end()))));
    Address from;
    Bytes data;
    bool gotKey = false;
    for (int i = 0; i < 200; ++i) {
        h.turn();
        if (other.receiveFrom(from, data)) gotKey = gotKey || (data.size() > 4 && data[4] == kServerKey);
    }
    CHECK(!gotKey);
    CHECK_EQ(h.server.players(), 0);
    // Whoever got a key and then says nothing more is forgotten after five seconds.
    h.now += 6000;
    h.spin(100);
    Client& ann = h.join("Ann");
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby; }));
}

// Client-side prediction: the picture runs ahead of the confirmed state, and is right
// whenever nobody changes what they are doing.
void testPrediction() {
    Harness h;
    CHECK(h.start("", 0));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    Client& cat = h.join("Cat");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 3 && bob.state() == Client::State::Lobby && cat.state() == Client::State::Lobby; }));
    CHECK(ann.prediction());
    ann.setPredictionSteps(4);
    bob.setPrediction(false);
    PlayerInput east;
    east.dir[1] = true;
    ann.setInput(east);
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));

    std::map<std::uint32_t, std::uint32_t> predicted, confirmed;
    int ahead = 0, samples = 0, least = 1000, most = -1000;
    auto watch = [&](int ms) {
        for (int waited = 0; waited < ms; waited += 5) {
            h.turn();
            if (ann.state() != Client::State::Round) break;
            // What is predicted is the own player (and the bombs and flames around it); other
            // players are drawn as confirmed. So the own player is what must come true.
            auto own = [](const World& w) { return static_cast<std::uint32_t>(w.player(0).x * 100000 + w.player(0).y * 10 + w.player(0).facing); };
            confirmed[ann.stepsApplied()] = own(*ann.world());
            if (ann.view() != ann.world()) {
                predicted[ann.viewStep()] = own(*ann.view());
                ahead += static_cast<int>(ann.viewStep()) - static_cast<int>(ann.stepsApplied());
                least = std::min(least, static_cast<int>(ann.viewStep()) - static_cast<int>(ann.stepsApplied()));
                most = std::max(most, static_cast<int>(ann.viewStep()) - static_cast<int>(ann.stepsApplied()));
                ++samples;
            }
        }
    };
    auto wrong = [&](std::uint32_t from, std::uint32_t to) {
        int count = 0, compared = 0;
        for (const auto& [step, hash] : predicted)
            if (step >= from && step < to && confirmed.count(step) != 0) {
                ++compared;
                count += confirmed[step] != hash ? 1 : 0;
            }
        return compared > 10 ? count : 1000;
    };
    // Ann keeps walking east: every predicted place of hers comes true.
    watch(4000);
    CHECK(samples > 100);
    // Four steps ahead was asked for. The client lets its target drift up to two steps from
    // that before pulling it back, so where it settles depends on when the server's steps
    // happen to arrive within a local step (on macOS it settled at five).
    CHECK(least >= 2 && most <= 6);
    CHECK(ahead >= samples * 3 && ahead <= samples * 6);
    if (least < 2 || most > 6 || ahead < samples * 3 || ahead > samples * 6)
        std::printf("  FAIL (values) ahead %d over %d samples, least %d, most %d, udp %d, confirmed steps %u, server steps %u\n", ahead, samples,
                    least, most, ann.udpActive() ? 1 : 0, ann.stepsApplied(), h.server.steps());
    const std::uint32_t steady = ann.stepsApplied();
    CHECK(steady > 40);
    CHECK_EQ(wrong(10, steady), 0);
    // The picture shows Ann where the server will only put her later.
    CHECK(ann.view()->player(0).x >= ann.world()->player(0).x);
    // Ann turns round: the steps already under way were guessed with the old key, the rest are right again.
    PlayerInput south;
    south.dir[2] = true;
    ann.setInput(south);
    watch(3000);
    const std::uint32_t later = ann.stepsApplied();
    CHECK(wrong(steady + 12, later) == 0);
    // Bob has prediction off and Ann's guesses never touch what is confirmed: all agree with the server.
    CHECK(bob.view() == bob.world());
    h.spin(50);
    if (ann.state() == Client::State::Round && ann.stepsApplied() == h.server.steps()) CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(ann.snapshotsLoaded(), 0);
    // Ann drops a bomb: its sound is reported exactly once, whether the prediction or the
    // server's confirmation gets there first (over the loopback interface either may).
    int dropsHeard = 0;
    Client::EventSink sink = [&](const World&, const std::vector<Event>& events) {
        for (const Event& e : events) dropsHeard += e.kind == EventKind::BombDropped && e.player == 0 ? 1 : 0;
    };
    PlayerInput drop;
    drop.button1 = true;
    ann.setInput(drop);
    for (int waited = 0; waited < 1500; waited += 5) {
        h.server.update(h.now);
        for (auto& c : h.clients) {
            c->update(h.now);
            c->advance(h.now, nullptr, c.get() == &ann ? sink : Client::EventSink());
        }
        h.now += 5;
        std::this_thread::yield();
        if (waited == 300) ann.setInput({});
    }
    CHECK_EQ(dropsHeard, 1);
    CHECK_EQ(h.server.world()->bombsOwnedBy(0), 1);
    // Somebody else's next move is what prediction cannot know.
    bob.setInput(east);
    for (int waited = 0; waited < 3000 && ann.state() == Client::State::Round; waited += 5) {
        h.turn();
        // Other players are drawn where the server has confirmed them, not where their last
        // movement would carry them: Bob in Ann's picture is Bob in Ann's confirmed state.
        if (ann.view() != ann.world()) CHECK(ann.view()->player(1).x == ann.world()->player(1).x && ann.view()->player(1).y == ann.world()->player(1).y);
    }
    CHECK(h.server.world()->player(1).x > cellToPixelX(0) || h.server.world()->player(1).facing == 1);
    CHECK_EQ(ann.snapshotsLoaded(), 0);
}

// One server, a client over IPv6 and one over IPv4, in the same round.
void testIpv6() {
    Harness h;
    CHECK(h.start("", 1));
    h.clients.push_back(std::make_unique<Client>());
    Client& six = *h.clients.back();
    six.connect("[::1]:" + std::to_string(h.server.port()), "Six", "", h.now);
    h.until([&] { return six.state() == Client::State::Lobby || six.state() == Client::State::Failed; });
    if (six.state() == Client::State::Failed) {
        // A machine without IPv6 (some build containers): nothing to test here.
        std::printf("     skipped: no IPv6 on this machine (%s)\n", six.error().c_str());
        return;
    }
    Client& four = h.join("Four");
    CHECK(h.until([&] { return four.state() == Client::State::Lobby && six.lobby().clients.size() == 2 && six.udpActive() && four.udpActive(); }));
    six.sendStart();
    CHECK(h.until([&] { return six.stepsApplied() > 60 && four.stepsApplied() > 60; }));
    CHECK(six.udpActive() && four.udpActive());
    CHECK(h.until([&] { return six.stepsApplied() == h.server.steps() && four.stepsApplied() == h.server.steps(); }, 3000));
    CHECK_EQ(six.snapshotsLoaded() + four.snapshotsLoaded(), 0);
}

// Opening the port on the router: the whole conversation against a router made of strings.
void testUpnp() {
    CHECK(httpHeader("HTTP/1.1 200 OK\r\nCache-Control: max-age=120\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\n\r\n", "Location") ==
          "http://192.168.1.1:5000/rootDesc.xml");
    CHECK(httpHeader("HTTP/1.1 200 OK\r\n\r\nLocation: in the body", "location").empty());
    std::string host, path;
    int port = 0;
    CHECK(splitUrl("http://192.168.1.1:5000/rootDesc.xml", &host, &port, &path) && host == "192.168.1.1" && port == 5000 && path == "/rootDesc.xml");
    CHECK(splitUrl("HTTP://router.lan", &host, &port, &path) && host == "router.lan" && port == 80 && path == "/");
    CHECK(!splitUrl("https://192.168.1.1/", &host, &port, &path));
    const std::string description =
        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n\r\n<?xml version=\"1.0\"?><root><device><serviceList><service>"
        "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/ctl/L3F</controlURL></service></serviceList>"
        "<deviceList><device><deviceList><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL></service></serviceList></device></deviceList></device></deviceList></device></root>";
    std::string control, service;
    CHECK(findWanService(description, &control, &service) && control == "/ctl/IPConn" && service == "urn:schemas-upnp-org:service:WANIPConnection:1");
    CHECK(!findWanService("<root><service><serviceType>urn:x:service:Other:1</serviceType><controlURL>/c</controlURL></service></root>", &control, &service));

    // A router that works.
    std::vector<std::string> requests;
    bool refuseUdp = false;
    UpnpTransport router;
    router.search = [&](const std::string& message) {
        requests.push_back("SEARCH " + message.substr(0, 8));
        return std::vector<std::string>{"HTTP/1.1 200 OK\r\nLocation: http://192.168.1.1:5000/rootDesc.xml\r\n\r\n"};
    };
    router.http = [&](const std::string& toHost, int toPort, const std::string& request, std::string* localIp) -> std::optional<std::string> {
        requests.push_back(toHost + ":" + std::to_string(toPort) + " " + request.substr(0, request.find("\r\n")));
        if (localIp != nullptr) *localIp = "192.168.1.23";
        if (request.rfind("GET /rootDesc.xml", 0) == 0) return description;
        if (request.find("#AddPortMapping") != std::string::npos) {
            if (request.find("<NewInternalClient>192.168.1.23</NewInternalClient>") == std::string::npos) return std::string("HTTP/1.1 500 Bad\r\n\r\n");
            if (refuseUdp && request.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos)
                return std::string("HTTP/1.1 500 Internal Server Error\r\n\r\n<errorCode>718</errorCode><errorDescription>ConflictInMappingEntry</errorDescription>");
            return std::string("HTTP/1.1 200 OK\r\n\r\n<u:AddPortMappingResponse/>");
        }
        if (request.find("#GetExternalIPAddress") != std::string::npos)
            return std::string("HTTP/1.1 200 OK\r\n\r\n<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>");
        if (request.find("#DeletePortMapping") != std::string::npos) return std::string("HTTP/1.1 200 OK\r\n\r\n");
        return std::nullopt;
    };
    UpnpResult mapped = upnpMapPort(router, 27410, "Atomic Bomberman");
    CHECK(mapped.ok);
    CHECK(mapped.externalIp == "203.0.113.7");
    CHECK(mapped.host == "192.168.1.1" && mapped.hostPort == 5000 && mapped.controlPath == "/ctl/IPConn");
    int adds = 0;
    for (const std::string& r : requests) adds += r == "192.168.1.1:5000 POST /ctl/IPConn HTTP/1.1" ? 1 : 0;
    CHECK_EQ(adds, 3);  // TCP, UDP, and the question for the public address
    requests.clear();
    upnpUnmapPort(router, mapped, 27410);
    CHECK_EQ(requests.size(), 2u);
    // A router that refuses the second mapping, and no router at all: reported, not fatal.
    refuseUdp = true;
    const UpnpResult refused = upnpMapPort(router, 27410, "Atomic Bomberman");
    CHECK(!refused.ok && refused.message.find("UDP") != std::string::npos && refused.message.find("ConflictInMappingEntry") != std::string::npos);
    router.search = [](const std::string&) { return std::vector<std::string>{}; };
    const UpnpResult nobody = upnpMapPort(router, 27410, "Atomic Bomberman");
    CHECK(!nobody.ok && !nobody.message.empty());
}

// Out of a match but not off the server: back to the lobby, the computer plays the seat,
// and the next match is joined as usual.
void testBackToLobby() {
    Harness h;
    CHECK(h.start("", 1));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
    ann.sendStart();
    CHECK(h.until([&] { return ann.stepsApplied() > 40 && bob.stepsApplied() > 40; }));
    ann.leaveMatch();
    CHECK(ann.state() == Client::State::Lobby && ann.sittingOut() && ann.world() == nullptr);
    h.spin(500);
    // Still connected, still in her seat for the next match; the match goes on.
    CHECK_EQ(h.server.players(), 2);
    CHECK(h.server.phase() == Phase::Round);
    CHECK(ann.state() == Client::State::Lobby);
    CHECK(ann.lobby().phase != Phase::Lobby && ann.seat() == 0);
    bool said = false;
    for (const ChatLine& line : bob.chat()) said = said || line.text == "Ann has gone back to the lobby";
    CHECK(said);
    // She can still talk.
    ann.sendChat("good luck");
    CHECK(h.until([&] {
        for (const ChatLine& line : bob.chat())
            if (line.name == "Ann" && line.text == "good luck") return true;
        return false;
    }));
    // The others play the match to its end (rounds until somebody has won), Ann's seat in
    // the computer's hands; she is not sent into the rounds in between.
    for (int rounds = 0; rounds < 30 && bob.state() != Client::State::Lobby; ++rounds) {
        CHECK(playToResult(h, {&bob}));
        CHECK(ann.state() == Client::State::Lobby);
        CHECK_EQ(bob.world()->stateHash(true), h.server.world()->stateHash(true));
        const bool decided = bob.score().matchWinner >= 0;
        const std::uint32_t round = bob.roundId();
        bob.sendContinue();  // Ann's key is not waited for
        if (decided) CHECK(h.until([&] { return bob.state() == Client::State::Lobby; }, 5000));
        else CHECK(h.until([&] { return bob.state() == Client::State::Round && bob.roundId() == round + 1; }, 5000));
    }
    CHECK(h.until([&] { return h.server.phase() == Phase::Lobby && ann.lobby().phase == Phase::Lobby && !ann.sittingOut(); }));
    // The next match has her in it again.
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round && ann.stepsApplied() > 20; }));
    // When everybody goes back to the lobby the match is over.
    ann.leaveMatch();
    bob.leaveMatch();
    CHECK(h.until([&] { return h.server.phase() == Phase::Lobby && ann.lobby().phase == Phase::Lobby && bob.lobby().phase == Phase::Lobby; }));
    CHECK_EQ(h.server.players(), 2);
    CHECK(!ann.sittingOut() && !bob.sittingOut());
}

// Two players at one computer: two seats, two inputs, both predicted.
void testTwoPlayersOneComputer() {
    Harness h;
    CHECK(h.start("", 0));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
    ann.sendLocalPlayers(2);
    CHECK(h.until([&] { return ann.seats()[1] >= 0 && bob.lobby().client(ann.id()) != nullptr && bob.lobby().client(ann.id())->players == 2; }));
    CHECK_EQ(ann.seats()[0], 0);
    CHECK_EQ(ann.seats()[1], 2);  // Bob has seat 1
    CHECK(bob.lobby().seatName(2) == "Ann (2)");
    // Each of the two changes team on its own.
    const int team = ann.lobby().seats[2].team;
    ann.sendTeam(1);
    CHECK(h.until([&] { return bob.lobby().seats[2].team == 1 - team; }));
    // One player fewer, one more: the seat is given back and taken again.
    ann.sendLocalPlayers(1);
    CHECK(h.until([&] { return ann.seats()[1] < 0 && ann.lobby().seats[2].kind == SeatKind::Empty; }));
    ann.sendLocalPlayers(9);  // more than a computer may have
    CHECK(h.until([&] { return ann.localPlayers() == 4 && ann.seats()[3] >= 0; }));
    ann.sendLocalPlayers(2);
    CHECK(h.until([&] { return ann.localPlayers() == 2 && ann.seats()[2] < 0; }));

    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    CHECK_EQ(h.server.world()->alivePlayers(), 3);
    PlayerInput east, south;
    east.dir[1] = true;
    south.dir[2] = true;
    ann.setInput(0, east);   // seat 0 starts top left
    ann.setInput(1, south);
    const int x0 = h.server.world()->player(0).x, y2 = h.server.world()->player(2).y, x2 = h.server.world()->player(2).x;
    CHECK(h.until([&] { return ann.stepsApplied() > 80; }));
    const World& w = *h.server.world();
    CHECK(w.player(0).x > x0 || w.player(0).facing == 1);          // the first player walked east
    CHECK(w.player(2).facing == 2 && w.player(2).x == x2 && w.player(2).y >= y2);  // the second turned south, on its own input
    CHECK(ann.view() != ann.world());
    CHECK_EQ(ann.snapshotsLoaded() + bob.snapshotsLoaded(), 0);
    CHECK(h.until([&] { return ann.stepsApplied() == h.server.steps() && ann.world()->stateHash(true) == h.server.world()->stateHash(true); }, 3000));
}

// The roulette over the network: the winner of one match starts the next with a prize.
void testNetRoulette() {
    Harness h;
    CHECK(h.start("", 3));
    Client& ann = h.join("Ann");
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    ann.sendOption(Option::Goldman, 1);
    CHECK(h.until([&] { return ann.lobby().settings.goldman; }));
    auto playMatch = [&]() {
        ann.sendStart();
        if (!h.until([&] { return ann.state() == Client::State::Round; })) return -2;
        for (int rounds = 0; rounds < 30; ++rounds) {
            if (!playToResult(h, {&ann})) return -2;
            const int winner = ann.score().matchWinner;
            ann.sendContinue();
            if (winner >= 0) {
                h.until([&] { return ann.state() == Client::State::Lobby; });
                return winner;
            }
            const std::uint32_t round = ann.roundId();
            if (!h.until([&] { return ann.state() == Client::State::Round && ann.roundId() != round; })) return -2;
        }
        return -2;
    };
    const int winner = playMatch();
    CHECK(winner >= 0);
    // The second match begins with the wheel: everybody is sent its seed, and running the
    // same wheel from that seed stops on the prize the server announces.
    ann.sendStart();
    CHECK(h.until([&] { return ann.roulette() != nullptr; }));
    CHECK(h.server.phase() == Phase::Roulette);
    {
        const RouletteMsg show = *ann.roulette();
        CHECK_EQ(show.winner, winner);
        const Values values = Values::defaults();
        Roulette wheel(values, show.seed);
        int frame = 0;
        for (; frame < 5000 && wheel.prize() < 0; ++frame) {
            wheel.step();
            if (frame == 50 || wheel.state() == Roulette::State::Stopped) wheel.press();
        }
        CHECK_EQ(wheel.prize(), show.prize);
        CHECK_EQ(frame, show.frames);
    }
    CHECK(h.until([&] { return ann.state() == Client::State::Round; }));
    int prizes = 0, type = -1;
    for (int i = 0; i < kMaxPlayers; ++i)
        if (ann.setup().prize[static_cast<std::size_t>(i)] >= 0) {
            ++prizes;
            type = ann.setup().prize[static_cast<std::size_t>(i)];
            CHECK_EQ(i, winner);
        }
    CHECK_EQ(prizes, 1);
    bool validPrize = false;
    for (int p : Roulette::kPrize) validPrize = validPrize || p == type;
    CHECK(validPrize);
    bool announced = false;
    for (const ChatLine& line : ann.chat()) announced = announced || (line.fromServer && line.text.rfind("The Gold Player", 0) == 0);
    CHECK(announced);
    // One more of it than a player without the prize (compare with the tuning value).
    if (type >= 0 && type != kPowClog) CHECK_EQ(ann.world()->player(winner).inventory[static_cast<std::size_t>(type)], Values::defaults().get(vid::kStartInventory + type) + 1);
    CHECK(ann.world()->player(winner).gold);
    CHECK_EQ(ann.world()->stateHash(true) == h.server.world()->stateHash(true) || ann.stepsApplied() != h.server.steps(), true);
}

// A campaign played over the network, with the free asset set as the server's game data.
void testNetCampaign() {
    const std::string dir = (std::filesystem::temp_directory_path() / "ab-net-campaign-test").string();
    std::filesystem::remove_all(dir);
    CHECK(ensureFreeAssets(dir, false));
    Harness h;
    h.gameDir = dir;
    CHECK(h.start("", 2));
    Client& ann = h.join("Ann");
    Client& bob = h.join("Bob");
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
    CHECK_EQ(ann.lobby().settings.campaign, 0);
    // The campaigns of the data folder, in name order, after "off".
    ann.sendOption(Option::Campaign, 1);
    CHECK(h.until([&] { return bob.lobby().settings.campaign == 1; }));
    CHECK(bob.lobby().settings.campaignTitle == "first-steps");
    ann.sendOption(Option::Campaign, -1);
    ann.sendOption(Option::Campaign, -1);
    CHECK(h.until([&] { return bob.lobby().settings.campaignTitle == "night-shift"; }));
    ann.sendOption(Option::Campaign, 1);
    ann.sendOption(Option::Campaign, 1);
    CHECK(h.until([&] { return bob.lobby().settings.campaignTitle == "first-steps"; }));

    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    // Stage 1 of first-steps: one rover, no computer players (the lobby's two stay out), no teams.
    CHECK(ann.setup().campaign);
    CHECK(ann.roundStart().stageName == "A Lonely Rover");
    CHECK_EQ(ann.roundStart().stage, 1);
    CHECK_EQ(ann.roundStart().stages, 6);
    CHECK_EQ(ann.world()->alivePlayers(), 2);
    CHECK(ann.world()->campaign());
    int rovers = 0;
    for (const Alien& a : ann.world()->aliens()) rovers += a.active && a.type == AlienType::Rover ? 1 : 0;
    CHECK_EQ(rovers, 1);
    bool stageSaid = false;
    for (const ChatLine& line : bob.chat()) stageSaid = stageSaid || line.text == "Stage 1 of 6: A Lonely Rover";
    CHECK(stageSaid);

    // Nobody moves: the stage ends by the rover or by the clock. Either way both clients
    // hold the server's state, and the server's verdict decides what comes next.
    CHECK(playToResult(h, {&ann, &bob}));
    CHECK(ann.result().campaign == 1 || ann.result().campaign == 2);
    CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(bob.world()->stateHash(true), h.server.world()->stateHash(true));
    CHECK_EQ(ann.result().campaign, h.server.world()->campaignResult());
    const bool again = ann.result().campaign == 2 && h.server.world()->campaignRetry();
    ann.sendContinue();
    bob.sendContinue();
    const std::uint32_t round = ann.roundId();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && ann.roundId() == round + 1; }));
    CHECK_EQ(ann.roundStart().stage, again ? 1 : 2);
    CHECK(ann.roundStart().stageName == (again ? "A Lonely Rover" : "Rovers in the Brickyard"));
    CHECK_EQ(ann.snapshotsLoaded() + bob.snapshotsLoaded(), 0);
    // Everybody leaves: the campaign is dropped and the server is in its lobby again.
    ann.disconnect();
    bob.disconnect();
    CHECK(h.until([&] { return h.server.phase() == Phase::Lobby; }));
    std::filesystem::remove_all(dir);
}

Bytes fromHex(const std::string& hex) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}
template <class Container>
std::string toHex(const Container& bytes) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) out += kDigits[b >> 4], out += kDigits[b & 15];
    return out;
}
Key keyFromHex(const std::string& hex) {
    Key k{};
    const Bytes b = fromHex(hex);
    std::copy(b.begin(), b.end(), k.begin());
    return k;
}

// The algorithms against the test vectors of their specifications.
void testCrypto() {
    // SHA-256 (FIPS 180-4 examples).
    const std::string abc = "abc";
    CHECK(toHex(sha256(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size())) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256(nullptr, 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(toHex(sha256(reinterpret_cast<const std::uint8_t*>(two.data()), two.size())) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // HMAC-SHA-256 (RFC 4231, test case 2, with the key padded to 32 bytes as HMAC does).
    Key jefe{};
    std::copy_n("Jefe", 4, jefe.begin());
    const std::string what = "what do ya want for nothing?";
    CHECK(toHex(hmacSha256(jefe, reinterpret_cast<const std::uint8_t*>(what.data()), what.size())) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // X25519 (RFC 7748, section 5.2 and the key exchange of section 6.1).
    CHECK(toHex(x25519(keyFromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                       keyFromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"))) ==
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    const Key alice = keyFromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Key bob = keyFromHex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    CHECK(toHex(x25519Base(alice)) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(toHex(x25519Base(bob)) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    CHECK(toHex(x25519(alice, x25519Base(bob))) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(x25519(bob, x25519Base(alice)) == x25519(alice, x25519Base(bob)));

    // ChaCha20 block (RFC 8439, 2.3.2) and Poly1305 (2.5.2).
    Key counting;
    for (std::size_t i = 0; i < 32; ++i) counting[i] = static_cast<std::uint8_t>(i);
    const std::array<std::uint8_t, 12> blockNonce = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    CHECK(toHex(chacha20Block(counting, 1, blockNonce)) ==
          "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
    const std::string forum = "Cryptographic Forum Research Group";
    CHECK(toHex(poly1305(keyFromHex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"), reinterpret_cast<const std::uint8_t*>(forum.data()), forum.size())) ==
          "a8061dc1305136c6c22b8baf0c0127a9");

    // The combined mode (RFC 8439, 2.8.2).
    Key aeadKey;
    for (std::size_t i = 0; i < 32; ++i) aeadKey[i] = static_cast<std::uint8_t>(0x80 + i);
    const std::array<std::uint8_t, 12> aeadNonce = {7, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
    const Bytes aad = fromHex("50515253c0c1c2c3c4c5c6c7");
    const std::string sunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const Bytes sealed = seal(aeadKey, aeadNonce, aad.data(), aad.size(), reinterpret_cast<const std::uint8_t*>(sunscreen.data()), sunscreen.size());
    CHECK_EQ(sealed.size(), sunscreen.size() + 16);
    CHECK(toHex(Bytes(sealed.begin(), sealed.begin() + 16)) == "d31a8d34648e60db7b86afbc53ef7ec2");
    CHECK(toHex(Bytes(sealed.end() - 16, sealed.end())) == "1ae10b594f09e26a7e902ecbd0600691");
    Bytes opened;
    CHECK(open(aeadKey, aeadNonce, aad.data(), aad.size(), sealed.data(), sealed.size(), opened));
    CHECK(std::string(opened.begin(), opened.end()) == sunscreen);
    // Any change is noticed: a flipped bit in the text, in the tag, in the associated data, or another nonce.
    Bytes altered = sealed;
    altered[5] ^= 1;
    CHECK(!open(aeadKey, aeadNonce, aad.data(), aad.size(), altered.data(), altered.size(), opened));
    altered = sealed;
    altered.back() ^= 0x80;
    CHECK(!open(aeadKey, aeadNonce, aad.data(), aad.size(), altered.data(), altered.size(), opened));
    Bytes otherAad = aad;
    otherAad[0] ^= 1;
    CHECK(!open(aeadKey, aeadNonce, otherAad.data(), otherAad.size(), sealed.data(), sealed.size(), opened));
    CHECK(!open(aeadKey, counterNonce(7), aad.data(), aad.size(), sealed.data(), sealed.size(), opened));
    CHECK(!open(aeadKey, aeadNonce, aad.data(), aad.size(), sealed.data(), 10, opened));

    // Both ends of a connection derive the same keys; the password proof depends on the
    // password and on the connection.
    Key a{}, b{};
    randomBytes(a.data(), 32);
    randomBytes(b.data(), 32);
    CHECK(a != b);
    // The server also has a lasting identity key; both ends mix in the secret between it and
    // the client's fresh key, so only the identity's owner arrives at the same keys.
    Key id{};
    randomBytes(id.data(), 32);
    const Key idPublic = x25519Base(id);
    SessionKeys client, server;
    CHECK(deriveSession(a, x25519Base(b), x25519Base(a), x25519Base(b), x25519(a, idPublic), idPublic, client));
    CHECK(deriveSession(b, x25519Base(a), x25519Base(a), x25519Base(b), x25519(id, x25519Base(a)), idPublic, server));
    CHECK(client.master == server.master && client.toServer == server.toServer && client.udpToClient == server.udpToClient);
    CHECK(client.toServer != client.toClient && client.udpToServer != client.toServer);
    CHECK(passwordProof("secret", client.master) == passwordProof("secret", server.master));
    CHECK(passwordProof("secret", client.master) != passwordProof("Secret", client.master));
    // Someone who shows that identity without owning it does not get the keys.
    Key thief{};
    randomBytes(thief.data(), 32);
    SessionKeys stolen;
    CHECK(deriveSession(b, x25519Base(a), x25519Base(a), x25519Base(b), x25519(thief, x25519Base(a)), idPublic, stolen));
    CHECK(stolen.master != client.master);
    CHECK_EQ(fingerprint(idPublic).size(), 19u);
    Key back{};
    CHECK(ab::net::fromHex(ab::net::toHex(id), back) && back == id && !ab::net::fromHex("xyz", back));
    SessionKeys bad;
    CHECK(!deriveSession(a, Key{}, x25519Base(a), Key{}, x25519(a, idPublic), idPublic, bad));  // a public key of zeros gives no secret
}

// Someone in the middle who passes every message on: sees only noise, and cannot change
// a message without ending the connection.
void testEncryptionOnTheWire() {
    Harness h;
    CHECK(h.start("open sesame", 1));
    // The relay: accepts the client, connects to the server, copies frames both ways.
    TcpListener listener;
    CHECK(listener.listen(0));
    TcpSocket toServer, toClient;
    std::string seen;        // every byte that passed, both directions
    int frames = 0, tamperAt = -1;
    bool linked = false;
    auto relay = [&]() {
        if (!toClient.open()) {
            if (auto accepted = listener.accept()) {
                toClient = std::move(*accepted);
                toServer.connect({kLoopback, h.server.port()});
            }
            return;
        }
        std::uint8_t type = 0;
        Bytes payload;
        toClient.pump();
        toServer.pump();
        if (linked && !toServer.open()) toClient.close();  // the server hung up: so does the relay
        linked = linked || toServer.connected();
        while (toClient.receive(type, payload)) {
            ++frames;
            seen += static_cast<char>(type);
            seen.append(payload.begin(), payload.end());
            if (frames == tamperAt && !payload.empty()) payload[payload.size() / 2] ^= 0x01;
            toServer.send(type, payload);
        }
        while (toServer.receive(type, payload)) {
            seen += static_cast<char>(type);
            seen.append(payload.begin(), payload.end());
            toClient.send(type, payload);
        }
        toClient.pump();
        toServer.pump();
    };
    auto spin = [&](const std::function<bool()>& done, int maxMs = 20000) {
        for (int waited = 0; waited < maxMs && !done(); waited += 5) {
            relay();
            h.turn();
        }
        return done();
    };
    h.clients.push_back(std::make_unique<Client>());
    Client& ann = *h.clients.back();
    ann.connect("127.0.0.1:" + std::to_string(listener.port()), "Annabel Lee", "open sesame", h.now);
    CHECK(spin([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    CHECK(ann.encrypted());
    ann.sendChat("attack at dawn by the old mill");
    CHECK(spin([&] {
        for (const ChatLine& line : ann.chat())
            if (line.text == "attack at dawn by the old mill") return true;
        return false;
    }));
    CHECK(seen.size() > 300);
    CHECK(seen.find("attack at dawn") == std::string::npos);   // the chat
    CHECK(seen.find("open sesame") == std::string::npos);      // the password
    CHECK(seen.find("Annabel") == std::string::npos);          // the name
    CHECK(seen.find("has joined") == std::string::npos);       // what the server says
    // The password is not even sent in disguise: its proof differs for every connection.
    const std::string firstSession = seen;
    // One flipped bit in a later packet: the server does not act on it. To the link it is a
    // lost packet, so the message is sent again and arrives once, as it was written.
    spin([] { return false; }, 400);  // (the server takes one chat line per 300 ms)
    tamperAt = frames + 1;
    ann.sendChat("this one is altered on the way");
    CHECK(spin([&] {
        for (const std::string& line : h.log)
            if (line.find("altered on the way") != std::string::npos) return true;
        return false;
    }));
    CHECK(frames > tamperAt);  // the altered one did pass, and a repeat after it
    int arrived = 0;
    for (const std::string& line : h.log) arrived += line == "CHAT  Annabel Lee: this one is altered on the way" ? 1 : 0;
    CHECK_EQ(arrived, 1);
    CHECK(ann.state() == Client::State::Lobby);
    ann.disconnect();
    CHECK(spin([&] { return h.server.players() == 0; }));

    // Speaking without the key exchange, or in the clear after it, gets nowhere.
    TcpSocket raw;
    CHECK(raw.connect({kLoopback, h.server.port()}));
    HelloMsg hello;
    hello.name = "Plain";
    for (int i = 0; i < 200 && !raw.connected(); ++i) raw.pump(), h.turn();
    raw.send(static_cast<std::uint8_t>(ClientMsg::Hello), encoded(hello));
    bool dropped = false;
    for (int i = 0; i < 400 && !dropped; ++i) {
        dropped = !raw.pump();
        h.turn();
    }
    CHECK(dropped);
    CHECK_EQ(h.server.players(), 0);
}

// Kicks, bans and the limits against guessing and flooding.
void testBansAndLimits() {
    const std::string banFile = (std::filesystem::temp_directory_path() / "ab-net-bans-test.txt").string();
    std::filesystem::remove(banFile);
    auto refused = [](Harness& h, const std::string& name, const std::string& password = "") {
        Client& c = h.join(name, password);
        h.until([&] { return c.state() == Client::State::Failed || c.state() == Client::State::Lobby; }, 12000);
        const bool out = c.state() == Client::State::Failed;
        c.disconnect();
        return out;
    };
    {
        // Kicked: the address stays out for five minutes, then may return.
        Harness h;
        CHECK(h.start());
        Client& ann = h.join("Ann");
        Client& bob = h.join("Bob");
        CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
        ann.sendKick(bob.id());
        CHECK(h.until([&] { return bob.state() == Client::State::Failed; }));
        CHECK(bob.error() == "Removed by the administrator");
        CHECK(refused(h, "Bob"));
        h.now += 5 * 60 * 1000 + 1000;
        CHECK(!refused(h, "Bob"));
    }
    {
        // Banned: out until the ban is lifted, and the ban outlives the server.
        Harness h;
        ServerConfig config;
        config.port = 0;
        config.discoverable = false;
        config.banFile = banFile;
        config.log = [&h](const std::string& line) { h.log.push_back(line); };
        CHECK(h.server.start(config));
        Client& ann = h.join("Ann");
        Client& bob = h.join("Bob");
        CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby; }));
        bob.sendBan(ann.id());  // not the administrator: nothing happens
        h.spin(300);
        CHECK(ann.state() == Client::State::Lobby);
        ann.sendBan(bob.id());
        CHECK(h.until([&] { return bob.state() == Client::State::Failed; }));
        CHECK(bob.error() == "Banned by the administrator");
        CHECK_EQ(h.server.bans().size(), 1u);
        CHECK(h.server.bans()[0] == "127.0.0.1");
        h.now += 24 * 60 * 60 * 1000;  // a day later: still banned
        CHECK(refused(h, "Bob"));
        // The administrator is told the address, to be able to lift the ban.
        bool told = false;
        for (const ChatLine& line : ann.chat()) told = told || line.text.find("/unban 127.0.0.1") != std::string::npos;
        CHECK(told);
        h.server.stop();
        // A new server with the same file knows the ban; lifting it lets the address in again.
        Harness again;
        config.log = [&again](const std::string& line) { again.log.push_back(line); };
        CHECK(again.server.start(config));
        CHECK_EQ(again.server.bans().size(), 1u);
        CHECK(refused(again, "Bob"));
        CHECK(!again.server.unban("10.0.0.1"));
        CHECK(again.server.unban("127.0.0.1"));
        CHECK(!refused(again, "Bob"));
    }
    {
        // Five wrong passwords in a minute: the address is shut out, the right password included.
        Harness h;
        CHECK(h.start("right"));
        for (int i = 0; i < 5; ++i) CHECK(refused(h, "Guess", "wrong" + std::to_string(i)));
        CHECK(refused(h, "Owner", "right"));
        h.now += 5 * 60 * 1000 + 1000;
        CHECK(!refused(h, "Owner", "right"));
    }
    {
        // A flood of messages: that connection is dropped, the others play on.
        Harness h;
        CHECK(h.start());
        Client& ann = h.join("Ann");
        CHECK(h.until([&] { return ann.state() == Client::State::Lobby; }));
        Client& flood = h.join("Flood");
        CHECK(h.until([&] { return flood.state() == Client::State::Lobby; }));
        for (int i = 0; i < 1000; ++i) flood.sendOption(Option::Wins, 1);
        CHECK(h.until([&] { return flood.state() == Client::State::Failed; }));
        CHECK(ann.state() == Client::State::Lobby);
        CHECK(h.until([&] { return ann.lobby().clients.size() == 1; }));
        // Too many connections at once from one address are not all accepted.
        h.now += 120000;
        ServerConfig small;
        small.port = 0;
        small.discoverable = false;
        small.maxPerAddress = 2;
        Harness few;
        CHECK(few.server.start(small));
        Client& one = few.join("One");
        Client& two = few.join("Two");
        CHECK(few.until([&] { return one.state() == Client::State::Lobby && two.state() == Client::State::Lobby; }));
        CHECK(refused(few, "Three"));
        CHECK_EQ(few.server.players(), 2);
    }
    {
        // The server owner's password makes its holder administrator, whoever came first.
        Harness h;
        ServerConfig config;
        config.port = 0;
        config.discoverable = false;
        config.adminPassword = "owner";
        CHECK(h.server.start(config));
        Client& first = h.join("First");
        Client& owner = h.join("Owner");
        CHECK(h.until([&] { return first.isAdmin() && owner.state() == Client::State::Lobby && owner.lobby().clients.size() == 2; }));
        owner.sendLogin("guess");
        h.spin(300);
        CHECK(first.isAdmin());
        owner.sendLogin("owner");
        CHECK(h.until([&] { return owner.isAdmin() && !first.isAdmin(); }));
        // When the owner leaves, the role goes back to the one who has been there longest.
        owner.disconnect();
        CHECK(h.until([&] { return first.isAdmin(); }));
    }
    std::filesystem::remove(banFile);
}

// Player keys and the certificate: the network library only carries them (the signature code
// belongs to the program around it), so a stand-in signature is used here: the SHA-256 of
// key and message, twice over.
std::array<std::uint8_t, 64> standInSign(const Key& key, const Bytes& message) {
    Bytes all(key.begin(), key.end());
    all.insert(all.end(), message.begin(), message.end());
    const Key h = sha256(all);
    std::array<std::uint8_t, 64> s{};
    std::copy(h.begin(), h.end(), s.begin());
    std::copy(h.begin(), h.end(), s.begin() + 32);
    return s;
}

void testPlayerKeysAndCertificate() {
    Harness h;
    ServerConfig config;
    config.port = 0;
    config.discoverable = false;
    config.certificate = "{\"this\":\"is passed on as it is\"}";
    config.verifySignature = [](const Key& key, const Bytes& message, const std::array<std::uint8_t, 64>& signature) { return standInSign(key, message) == signature; };
    config.log = [&h](const std::string& line) { h.log.push_back(line); };
    CHECK(h.server.start(config));
    auto withKey = [&](const std::string& name, std::uint8_t fill, bool honest = true) -> Client& {
        for (int waited = 0; waited < 1000 && h.connecting(); waited += 5) h.turn();
        h.clients.push_back(std::make_unique<Client>());
        Client& c = *h.clients.back();
        Key key{};
        key.fill(fill);
        c.setPlayerKey(key, [key, honest](const Bytes& message) {
            std::array<std::uint8_t, 64> s = standInSign(key, message);
            if (!honest) s[5] ^= 1;
            return s;
        });
        c.connect("127.0.0.1:" + std::to_string(h.server.port()), name, "", h.now);
        return c;
    };
    Client& ann = withKey("Ann", 0x11);
    Client& bob = withKey("Bob", 0x22);
    Client& cat = h.join("Cat");  // no key: just as welcome
    CHECK(h.until([&] { return ann.lobby().clients.size() == 3 && cat.state() == Client::State::Lobby && bob.state() == Client::State::Lobby; }));
    // Everybody sees who showed which key.
    Key annKey{}, bobKey{};
    annKey.fill(0x11);
    bobKey.fill(0x22);
    CHECK(cat.lobby().client(ann.id())->playerKey == annKey && cat.lobby().client(bob.id())->playerKey == bobKey && cat.lobby().client(cat.id())->playerKey == Key{});
    // The certificate reaches every client as the server holds it, with the identity it proved.
    CHECK(cat.certificate() == config.certificate && ann.certificate() == config.certificate);
    CHECK(cat.serverIdentityKey() == h.server.identityKeyHex() && cat.serverIdentityKey().size() == 64);
    // A key that is not proved (a signature that does not fit): not let in at all.
    Client& eve = withKey("Eve", 0x11, false);
    CHECK(h.until([&] { return eve.state() == Client::State::Failed; }));
    CHECK(eve.error() == "The player key was not proved (bad signature)");
    CHECK_EQ(h.server.players(), 3);
    // A proof made for one connection is worth nothing on another: the message signed
    // differs with every connection.
    CHECK(playerProofMessage(Key{}) != playerProofMessage(annKey));

    // A ban names the key too: with the address let in again, the key still keeps its owner out.
    ann.sendBan(bob.id());
    CHECK(h.until([&] { return bob.state() == Client::State::Failed; }));
    CHECK_EQ(h.server.bans().size(), 2u);
    CHECK(h.server.unban("127.0.0.1"));
    Client& again = withKey("Bob again", 0x22);
    CHECK(h.until([&] { return again.state() == Client::State::Failed; }));
    CHECK(again.error() == "Banned");
    Client& other = withKey("Somebody else", 0x33);
    CHECK(h.until([&] { return other.state() == Client::State::Lobby; }));
    CHECK(h.server.unban("key:" + toHex(bobKey)));
    Client& back = withKey("Bob back", 0x22);
    CHECK(h.until([&] { return back.state() == Client::State::Lobby; }));

    // A match for listed players only: a listed key gets in (once), any other and no key do
    // not, and the settings are not the administrator's to change.
    {
        Harness m;
        ServerConfig only = config;
        only.certificate.clear();
        only.allowedKeys = {annKey, bobKey};
        only.lockSettings = true;
        only.settings.winsNeeded = 3;
        only.log = [&m](const std::string& line) { m.log.push_back(line); };
        CHECK(m.server.start(only));
        auto key = [&](const std::string& name, std::uint8_t fill) -> Client& {
            for (int waited = 0; waited < 1000 && m.connecting(); waited += 5) m.turn();
            m.clients.push_back(std::make_unique<Client>());
            Client& c = *m.clients.back();
            Key k{};
            k.fill(fill);
            if (fill != 0) c.setPlayerKey(k, [k](const Bytes& message) { return standInSign(k, message); });
            c.connect("127.0.0.1:" + std::to_string(m.server.port()), name, "", m.now);
            return c;
        };
        Client& listed = key("Ann", 0x11);
        Client& stranger = key("Mallory", 0x44);
        Client& keyless = key("Nobody", 0);
        CHECK(m.until([&] { return listed.state() == Client::State::Lobby && stranger.state() == Client::State::Failed && keyless.state() == Client::State::Failed; }));
        CHECK(stranger.error() == "This match is for its listed players only" && keyless.error() == stranger.error());
        Client& twice = key("Ann again", 0x11);
        CHECK(m.until([&] { return twice.state() == Client::State::Failed; }));
        CHECK(twice.error() == "That player is already here");
        Client& second = key("Bob", 0x22);
        CHECK(m.until([&] { return second.state() == Client::State::Lobby && listed.lobby().clients.size() == 2; }));
        CHECK(listed.isAdmin());
        listed.sendOption(Option::Wins, 1);
        m.spin(300);
        CHECK_EQ(listed.lobby().settings.winsNeeded, 3);
        bool told = false;
        for (const ChatLine& line : listed.chat()) told = told || line.text == "The settings of this match are fixed";
        CHECK(told);
    }

    // A server that is given no way to check signatures takes no notice of keys.
    Harness plain;
    CHECK(plain.start());
    plain.clients.push_back(std::make_unique<Client>());
    Client& dan = *plain.clients.back();
    dan.setPlayerKey(annKey, [](const Bytes&) { return std::array<std::uint8_t, 64>{}; });
    dan.connect("127.0.0.1:" + std::to_string(plain.server.port()), "Dan", "", plain.now);
    CHECK(plain.until([&] { return dan.state() == Client::State::Lobby && !dan.lobby().clients.empty(); }));
    CHECK(dan.lobby().clients.front().playerKey == Key{});
    CHECK(dan.certificate().empty());
}

// Recorded rounds: the seed is nobody's choice, every player's inputs are in their own signed
// log, and the record plays again to the same result. Whatever is changed in it shows.
// Recorded rounds in which the keys keep changing to the very end, some of them on a
// connection that loses packets and with a computer that stalls again and again. Whatever the server
// applies for the player must still follow their own log in order: nothing held earlier may
// come back later. (It once did: at the end of a round a client stops guessing ahead and
// names an earlier step for its keys than before, while the server still held what had
// been guessed, with older keys, for the steps after that one.)
void testRecordInputOrder() {
    for (int run = 0; run < 6; ++run) {
        Harness h;
        ServerConfig config;
        config.port = 0;
        config.discoverable = false;
        config.seed = 7 + static_cast<std::uint32_t>(run);
        config.settings.computers = 1;
        config.settings.winsNeeded = 1;
        config.settings.enclosementDepth = 3;
        config.settings.playTime = 60;
        config.gameVersion = "test-version";
        const VerifyFn verify = [](const Key& key, const Bytes& message, const SignatureBytes& signature) { return standInSign(key, message) == signature; };
        config.verifySignature = verify;
        std::vector<MatchRecord> records;
        config.signRecord = [](const Bytes& bytes) { return "signed:" + std::string(bytes.begin(), bytes.end()); };
        config.recordSink = [&](const std::string&, const MatchRecord& record) { records.push_back(record); };
        CHECK(h.server.start(config));
        h.clients.push_back(std::make_unique<Client>());
        Client& ann = *h.clients.back();
        ann.setAlwaysReady(true);
        Key key{};
        key.fill(0x33);
        ann.setPlayerKey(key, [key](const Bytes& message) { return standInSign(key, message); });
        ann.setPredictionSteps(2 + run);  // as on a connection with that much delay: the server holds inputs for that many steps to come
        ann.connect("127.0.0.1:" + std::to_string(h.server.port()), "Ann", "", h.now);
        CHECK(h.until([&] { return ann.state() == Client::State::Lobby; }));
        ann.sendStart();
        CHECK(h.until([&] { return ann.state() == Client::State::Round; }));
        CHECK(ann.recorded());
        const bool bad = run % 2 == 1;  // every other run on a bad connection
        if (bad) setTestUdpLoss(20);
        // The keys change every 150 ms or so, going round twelve different combinations: one
        // that came back out of turn would not be found in the log again for a long while.
        std::uint32_t chance = 12345u + static_cast<std::uint32_t>(run) * 977u;
        int held = 0, until = 0;
        for (int i = 0; i < 20000 && ann.state() == Client::State::Round; ++i) {
            chance = chance * 1664525u + 1013904223u;
            if (i >= until) {
                ++held;
                until = i + 20 + static_cast<int>((chance >> 16) % 20);
            }
            PlayerInput a;
            a.dir[static_cast<std::size_t>(held % 4)] = true;
            a.button1 = (held / 4) % 3 == 1;  // bombs: the round is decided before long
            a.button2 = (held / 4) % 3 == 2;
            ann.setInput(a);
            if (bad && (chance >> 24) % 40 == 0) {
                // The player's computer stalls for up to 300 ms while the server goes on.
                const int stalled = 10 + static_cast<int>((chance >> 8) % 50);
                for (int k = 0; k < stalled; ++k) {
                    h.server.update(h.now);
                    h.now += 5;
                    Harness::pause();
                }
            }
            h.turn();
        }
        setTestUdpLoss(0);
        CHECK(ann.state() != Client::State::Round);
        CHECK(h.until([&] { return records.size() == 1; }, 5000));
        if (records.empty()) continue;
        const MatchRecord& record = records[0];
        CHECK_EQ(record.players.size(), 1u);
        for (const PlayerLog& p : record.players) {
            CHECK(p.haveStatement && p.values.size() > 5);
            CHECK(inputsFollowLog(record, p));
        }
        const RecordVerdict verdict = checkRecord(record, "test-version", verify);
        for (const std::string& p : verdict.problems) std::printf("     problem (run %d): %s\n", run, p.c_str());
        CHECK(verdict.ok);
    }
}

void testRecords() {
    // The pieces by themselves.
    {
        Key r1{}, r2{}, r3{};
        r1.fill(1), r2.fill(2), r3.fill(3);
        std::vector<SeedShare> shares = {{kServerSender, commitOf(r1), r1, true}, {4, commitOf(r2), r2, true}, {2, commitOf(r3), r3, true}};
        const std::uint32_t seed = seedOf(7, shares);
        CHECK(seed != 0);
        // The order they are listed in does not matter; any one number, or the round, does.
        std::vector<SeedShare> swapped = {shares[2], shares[0], shares[1]};
        CHECK_EQ(seedOf(7, swapped), seed);
        CHECK(recordIdOf(7, swapped) == recordIdOf(7, shares));
        for (std::size_t i = 0; i < shares.size(); ++i) {
            std::vector<SeedShare> other = shares;
            other[i].reveal[5] ^= 1;
            CHECK(seedOf(7, other) != seed);
            other = shares;
            other[i].commit[5] ^= 1;
            CHECK(recordIdOf(7, other) != recordIdOf(7, shares));
        }
        CHECK(seedOf(8, shares) != seed);
        // A number that was promised and never shown has no part in it.
        std::vector<SeedShare> silent = shares;
        silent[1].revealed = false;
        CHECK(seedOf(7, silent) != seed);
        silent[1].reveal.fill(9);
        CHECK_EQ(seedOf(7, silent), seedOf(7, {shares[0], shares[2]}));
        CHECK(commitOf(r1) != commitOf(r2) && commitOf(r1) != r1);
        // The running hash of a log: every value and the order count.
        const std::vector<InputTuple> log = {{1, 0, 0, 0}, {2, 0, 0, 0}, {1, 0, 0, 0}};
        CHECK(logChain(log, 0) == Key{} && logChain(log, 1) != logChain(log, 2) && logChain(log, 3) != logChain(log, 2));
        CHECK(logChain({log[1], log[0]}, 2) != logChain(log, 2));
        CHECK(logStatement(r1, 2, logChain(log, 2)) != logStatement(r2, 2, logChain(log, 2)));
        CHECK(logStatement(r1, 2, logChain(log, 2)) != logStatement(r1, 3, logChain(log, 2)));
    }
    // Inputs against a log: a walk forward through it, values may be passed over, never invented or gone back to.
    {
        MatchRecord r;
        r.seats[2] = {SeatKind::Human, 5, 0};
        r.seats[6] = {SeatKind::Human, 5, 1};
        PlayerLog log;
        log.client = 5;
        log.values = {{1, 0, 0, 0}, {1, 8, 0, 0}, {4, 8, 0, 0}, {0, 0, 0, 0}, {2, 0, 0, 0}};
        log.signedCount = 4;
        auto steps = [&](const std::vector<std::pair<int, int>>& applied) {
            r.inputs.clear();
            r.computer.clear();
            for (const auto& [a, b] : applied) {
                StepInputs in{};
                in[2] = static_cast<std::uint8_t>(a);
                in[6] = static_cast<std::uint8_t>(b);
                in[0] = 63;  // somebody else's seat: none of this client's business
                r.inputs.push_back(in);
                r.computer.push_back(0);
            }
        };
        std::size_t uncovered = 9;
        steps({{0, 0}, {0, 0}, {1, 0}, {1, 0}, {1, 8}, {4, 8}, {4, 8}, {0, 0}});
        CHECK(inputsFollowLog(r, log, &uncovered) && uncovered == 0);
        steps({{0, 0}, {4, 8}, {4, 8}, {2, 0}});  // two values passed over; the last is beyond the signed part
        CHECK(inputsFollowLog(r, log, &uncovered) && uncovered == 1);
        steps({{1, 0}, {4, 8}, {1, 8}});          // back to an earlier value
        CHECK(!inputsFollowLog(r, log));
        steps({{1, 0}, {16, 0}});                 // a bomb the player never laid
        CHECK(!inputsFollowLog(r, log));
        steps({{0, 8}});                          // never held in that combination
        CHECK(!inputsFollowLog(r, log));
        // A seat the computer played for a while is not held against the player.
        steps({{1, 0}, {16, 0}, {16, 0}, {1, 8}});
        CHECK(!inputsFollowLog(r, log));
        r.computer[1] = r.computer[2] = 1u << 2;  // the computer had the first seat in those two steps
        CHECK(inputsFollowLog(r, log));
        steps({{1, 0}, {16, 4}, {1, 8}});
        r.computer[1] = 1u << 2;                  // ...but the player's other seat is still held to the log
        CHECK(!inputsFollowLog(r, log));
    }

    // A server that records, two players with keys and a computer player.
    Harness h;
    ServerConfig config;
    config.port = 0;
    config.discoverable = false;
    config.seed = 99;
    config.settings.computers = 1;
    config.settings.winsNeeded = 1;
    config.settings.enclosementDepth = 3;
    config.settings.playTime = 60;
    config.gameVersion = "test-version";
    config.certificate = "a certificate, as it is";
    const VerifyFn verify = [](const Key& key, const Bytes& message, const SignatureBytes& signature) { return standInSign(key, message) == signature; };
    config.verifySignature = verify;
    std::vector<std::string> documents;
    std::vector<MatchRecord> records;
    config.signRecord = [](const Bytes& bytes) { return "signed:" + std::string(bytes.begin(), bytes.end()); };
    config.recordSink = [&](const std::string& document, const MatchRecord& record) {
        documents.push_back(document);
        records.push_back(record);
    };
    config.log = [&h](const std::string& line) { h.log.push_back(line); };
    CHECK(h.server.start(config));
    auto withKey = [&](const std::string& name, std::uint8_t fill) -> Client& {
        for (int waited = 0; waited < 1000 && h.connecting(); waited += 5) h.turn();
        h.clients.push_back(std::make_unique<Client>());
        Client& c = *h.clients.back();
        c.setAlwaysReady(true);
        Key key{};
        key.fill(fill);
        c.setPlayerKey(key, [key](const Bytes& message) { return standInSign(key, message); });
        c.connect("127.0.0.1:" + std::to_string(h.server.port()), name, "", h.now);
        return c;
    };
    Client& ann = withKey("Ann", 0x11);
    Client& bob = withKey("Bob", 0x22);
    CHECK(h.until([&] { return ann.lobby().clients.size() == 2 && bob.state() == Client::State::Lobby && bob.ready(); }));
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    CHECK(ann.recorded() && bob.recorded());
    CHECK(ann.roundStart().seed == 0);  // the seed is not told
    // Both walk about and lay bombs, each in their own way.
    for (int i = 0; i < 400 && ann.state() == Client::State::Round; ++i) {
        PlayerInput a, b;
        a.dir[static_cast<std::size_t>((i / 7) % 4)] = true;
        a.button1 = i % 23 == 0;
        b.dir[static_cast<std::size_t>((i / 11 + 1) % 4)] = true;
        b.button2 = i % 31 == 0;
        ann.setInput(a);
        bob.setInput(b);
        h.spin(50);
    }
    ann.setInput({});
    bob.setInput({});
    CHECK(playToResult(h, {&ann, &bob}));
    CHECK(h.until([&] { return documents.size() == 1 && ann.recordSerial() == 1 && bob.recordSerial() == 1; }, 5000));
    if (records.empty()) return;
    const MatchRecord& made = records[0];
    // Everybody in the round got the record as the server signed it.
    CHECK(ann.lastRecord() == documents[0] && bob.lastRecord() == documents[0]);
    // It survives being written down and read again.
    MatchRecord record;
    CHECK(documents[0].rfind("signed:", 0) == 0);
    const Bytes bytes(documents[0].begin() + 7, documents[0].end());
    CHECK(decodeRecord(bytes, record));
    CHECK(encodeRecord(record) == bytes);
    CHECK(record.gameVersion == "test-version" && record.certificate == config.certificate && record.inputs.size() == made.inputs.size());
    for (std::size_t cut = 0; cut < bytes.size(); cut += 97) CHECK(!decodeRecord(Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut)), record));
    CHECK(decodeRecord(bytes, record));

    // The seed: three shares (the server's and both players'), all shown, and each player
    // finds the number they gave and the promises they were shown.
    CHECK_EQ(record.shares.size(), 3u);
    for (const SeedShare& s : record.shares) CHECK(s.revealed && commitOf(s.reveal) == s.commit);
    CHECK_EQ(record.start.seed, seedOf(record.start.roundId, record.shares));
    CHECK(ann.roundStart().recordId == recordIdOf(record.start.roundId, record.shares));
    for (Client* c : {&ann, &bob}) {
        bool own = false;
        for (const SeedShare& s : record.shares) own = own || (s.client == c->id() && s.reveal == c->ownSeedReveal());
        CHECK(own);
        CHECK_EQ(c->seenCommits().size(), 3u);
        for (const auto& [client, commit] : c->seenCommits()) {
            bool there = false;
            for (const SeedShare& s : record.shares) there = there || (s.client == client && s.commit == commit);
            CHECK(there);
        }
    }
    // The logs: what each player kept is what the record holds, signed to the end.
    CHECK_EQ(record.players.size(), 2u);
    for (const PlayerLog& p : record.players) {
        const Client& c = p.client == ann.id() ? ann : bob;
        CHECK(p.haveStatement && p.signedCount == p.values.size() && p.values == c.ownLog() && p.values.size() > 20);
        std::size_t uncovered = 9;
        CHECK(inputsFollowLog(record, p, &uncovered) && uncovered == 0);
    }
    // Played again it is the same round.
    RecordVerdict verdict = checkRecord(record, "test-version", verify);
    for (const std::string& p : verdict.problems) std::printf("     problem: %s\n", p.c_str());
    CHECK(verdict.ok && verdict.problems.empty());
    CHECK(record.inputs.size() > 100 && record.hashes.size() == record.inputs.size() / 20);
    CHECK(!checkRecord(record, "another-version", verify).ok);

    // Whatever is changed shows.
    auto broken = [&](const std::function<void(MatchRecord&)>& change, const std::string& expected) {
        MatchRecord altered = record;
        change(altered);
        const RecordVerdict v = checkRecord(altered, "test-version", verify);
        bool said = false;
        for (const std::string& p : v.problems) said = said || p.find(expected) != std::string::npos;
        CHECK(!v.ok);
        if (!said) {
            ++g_failures;
            std::printf("  FAIL expected a problem with \"%s\"; got:", expected.c_str());
            for (const std::string& p : v.problems) std::printf(" [%s]", p.c_str());
            std::printf("\n");
        }
    };
    broken([](MatchRecord& r) { r.end.winner = r.end.winner == 0 ? 1 : 0; }, "the winner is");
    broken([](MatchRecord& r) { r.end.score.kills[0] += 1; }, "score");
    broken([](MatchRecord& r) { r.start.seed ^= 1; }, "seed is not the one");
    broken([](MatchRecord& r) { r.shares[1].reveal[0] ^= 1; }, "does not fit what was promised");
    broken([](MatchRecord& r) { r.shares.erase(r.shares.begin()); }, "server's share");
    broken([](MatchRecord& r) { r.finalHash ^= 1; }, "another state");
    broken([](MatchRecord& r) { r.inputs.pop_back(); }, "steps");
    // An input changed in the middle: the round goes another way, and it is not what the player logged.
    const int annSeat = ann.seat();
    broken([&](MatchRecord& r) {
        for (std::size_t i = 40; i < 60; ++i) r.inputs[i][static_cast<std::size_t>(annSeat)] ^= 16;
    }, "not the recorded one");
    broken([&](MatchRecord& r) {
        for (std::size_t i = 40; i < 60; ++i) r.inputs[i][static_cast<std::size_t>(annSeat)] = 32 | 16 | 1;
    }, "not the ones in their own log");
    // A log with another value in it: the signature no longer fits.
    broken([](MatchRecord& r) { r.players[0].values[3][0] ^= 1; }, "signature on the inputs");
    broken([](MatchRecord& r) { r.players[1].signature[9] ^= 1; }, "signature on the inputs");
    // A computer player's inputs are held to nothing but the replay.
    int computerSeat = -1;
    for (int s = 0; s < kMaxPlayers; ++s)
        if (record.seats[static_cast<std::size_t>(s)].kind == SeatKind::Computer) computerSeat = s;
    CHECK(computerSeat >= 0);
    broken([&](MatchRecord& r) {
        for (std::size_t i = 60; i + 25 < r.inputs.size(); ++i) r.inputs[i][static_cast<std::size_t>(computerSeat)] ^= 15;  // every direction turned round
    }, "not the recorded one");

    // The next round has a seed of its own and a record of its own.
    ann.sendContinue();
    bob.sendContinue();
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby || (ann.state() == Client::State::Round && ann.roundId() == record.start.roundId + 1); }));

    // A server without a signing key records nothing, and its rounds start as before.
    Harness plain;
    CHECK(plain.start("", 2));
    Client& dan = plain.join("Dan");
    CHECK(plain.until([&] { return dan.state() == Client::State::Lobby && !dan.lobby().clients.empty(); }));
    dan.sendStart();
    CHECK(plain.until([&] { return dan.state() == Client::State::Round; }));
    CHECK(!dan.recorded() && dan.recordSerial() == 0);
}

// A carrier made of two queues: what one end sends the other receives, some of it lost.
struct PipeEnd : Carrier {
    std::shared_ptr<std::deque<Bytes>> out, in;
    std::shared_ptr<bool> up;
    std::shared_ptr<int> lossPercent;
    std::shared_ptr<Rng> rng;
    std::string name;
    void send(const Bytes& packet) override {
        if (*up && rng->below(100) >= *lossPercent) out->push_back(packet);
    }
    bool receive(Bytes& packet) override {
        if (in->empty()) return false;
        packet = std::move(in->front());
        in->pop_front();
        return true;
    }
    bool alive() const override { return *up; }
    std::string remote() const override { return name; }
    std::string kind() const override { return "Pipe"; }
};

// A connection over a carrier (a path somebody else made, as ICE does): the key exchange,
// the lobby and a whole round, with a quarter of the packets lost; and its end.
void testCarrier() {
    Harness h;
    CHECK(h.start("pass", 2));
    auto a = std::make_shared<std::deque<Bytes>>(), b = std::make_shared<std::deque<Bytes>>();
    auto up = std::make_shared<bool>(true);
    auto loss = std::make_shared<int>(25);
    auto rng = std::make_shared<Rng>(5);
    auto end = [&](bool clientSide) {
        auto e = std::make_unique<PipeEnd>();
        e->out = clientSide ? a : b;
        e->in = clientSide ? b : a;
        e->up = up;
        e->lossPercent = loss;
        e->rng = rng;
        e->name = clientSide ? "the server" : "203.0.113.7:4000";
        return e;
    };
    CHECK(h.server.adopt(end(false)));
    h.clients.push_back(std::make_unique<Client>());
    Client& ann = *h.clients.back();
    ann.setAlwaysReady(true);
    ann.connectVia(end(true), "the server", "Ann", "pass", h.now);
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    CHECK(ann.encrypted() && !ann.udpActive());
    CHECK(ann.pathKind() == "Pipe");
    // The volume check: 64 KB there and back; it passes on a line that goes on carrying
    // (a quarter of the packets lost does not fail it)...
    CHECK(ann.volumeCheck() == Client::VolumeCheck::NotRun);
    ann.startVolumeCheck(h.now);
    CHECK(h.until([&] { return ann.volumeCheck() != Client::VolumeCheck::Running; }, 6000));
    CHECK(ann.volumeCheck() == Client::VolumeCheck::Passed);
    {
        // ...and fails on one that stops carrying after 16 KB, as some networks do to a new flow.
        auto e = std::make_shared<std::deque<Bytes>>(), f = std::make_shared<std::deque<Bytes>>();
        auto budget = std::make_shared<std::size_t>(1u << 30);
        struct Choking : PipeEnd {
            std::shared_ptr<std::size_t> left;
            void send(const Bytes& packet) override {
                if (*left < packet.size()) return;  // swallowed without a word
                *left -= packet.size();
                PipeEnd::send(packet);
            }
        };
        auto choking = [&](bool clientSide) {
            auto c = std::make_unique<Choking>();
            c->out = clientSide ? e : f;
            c->in = clientSide ? f : e;
            c->up = up;
            c->lossPercent = std::make_shared<int>(0);
            c->rng = rng;
            c->name = clientSide ? "the server" : "203.0.113.8:4000";
            c->left = budget;
            return c;
        };
        CHECK(h.server.adopt(choking(false)));
        h.clients.push_back(std::make_unique<Client>());
        Client& cy = *h.clients.back();
        cy.setAlwaysReady(true);
        cy.connectVia(choking(true), "the server", "Cy", "pass", h.now);
        CHECK(h.until([&] { return cy.state() == Client::State::Lobby; }));
        *budget = 16000;
        cy.startVolumeCheck(h.now);
        CHECK(h.until([&] { return cy.volumeCheck() != Client::VolumeCheck::Running; }, 6000));
        CHECK(cy.volumeCheck() == Client::VolumeCheck::Failed);
        cy.disconnect();
        *budget = 1u << 30;
        CHECK(h.until([&] { return h.server.players() == 1; }));
    }
    bool logged = false;
    for (const std::string& line : h.log) logged = logged || line.find("from 203.0.113.7:4000 (Pipe)") != std::string::npos;
    CHECK(logged);
    // An ordinary client beside it, over the server's own socket.
    Client& bob = h.join("Bob", "pass");
    CHECK(h.until([&] { return bob.state() == Client::State::Lobby && ann.lobby().clients.size() == 2 && bob.ready(); }));
    CHECK(bob.pathKind() == "Direct");
    ann.sendStart();
    CHECK(h.until([&] { return ann.state() == Client::State::Round && bob.state() == Client::State::Round; }));
    const bool played = playToResult(h, {&ann, &bob});
    CHECK(played);
    if (!played) return;
    CHECK_EQ(ann.world()->stateHash(true), h.server.world()->stateHash(true));
    // The lobby says how each is connected.
    CHECK(h.until([&] { return bob.lobby().clients.size() == 2; }));
    CHECK_EQ(bob.lobby().client(bob.id())->path, kPathDirect);
    CHECK(std::string(pathName(kPathRelay)) == "Relay" && std::string(pathName(kPathTcp)) == "TCP");
    // A wrong password over a carrier is refused like any other.
    auto c = std::make_shared<std::deque<Bytes>>(), d = std::make_shared<std::deque<Bytes>>();
    auto second = [&](bool clientSide) {
        auto e = end(clientSide);
        e->out = clientSide ? c : d;
        e->in = clientSide ? d : c;
        return e;
    };
    CHECK(h.server.adopt(second(false)));
    h.clients.push_back(std::make_unique<Client>());
    Client& eve = *h.clients.back();
    eve.connectVia(second(true), "the server", "Eve", "guess", h.now);
    CHECK(h.until([&] { return eve.state() == Client::State::Failed; }));
    CHECK(eve.error() == "Wrong password");
    // The path dies: both ends notice.
    *up = false;
    CHECK(h.until([&] { return ann.state() == Client::State::Failed; }));
    CHECK(h.until([&] { return h.server.players() == 1; }));
    // A banned address is not adopted.
    CHECK(h.server.kick("Bob", true));
    auto banned = std::make_unique<PipeEnd>(*end(false));
    banned->name = "127.0.0.1:9";
    *up = true;
    CHECK(!h.server.adopt(std::move(banned)));
}

// A server keeps its identity from one start to the next, and a client notices when the
// server at an address is not the one it knows.
void testServerIdentity() {
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string identityFile = (tmp / "ab-net-identity-test.key").string(), knownFile = (tmp / "ab-net-known-test.txt").string();
    std::filesystem::remove(identityFile);
    std::filesystem::remove(knownFile);
    ServerConfig config;
    config.discoverable = false;
    config.identityFile = identityFile;
    std::uint16_t port = 0;
    std::string identity;
    auto visit = [&](Harness& h, Client& c) {
        c.setKnownServersFile(knownFile);
        c.checkIdentityOnLoopback();  // the tests have only this machine
        c.connect("127.0.0.1:" + std::to_string(port), "Ann", "", h.now);
        h.until([&] { return c.state() == Client::State::Lobby || c.state() == Client::State::Failed; });
    };
    {
        Harness h;
        config.port = 0;
        CHECK(h.server.start(config));
        port = h.server.port();
        identity = h.server.identity();
        CHECK_EQ(identity.size(), 19u);
        h.clients.push_back(std::make_unique<Client>());
        Client& ann = *h.clients.back();
        visit(h, ann);
        CHECK(ann.state() == Client::State::Lobby);
        CHECK(ann.serverIdentity() == identity);
        CHECK(!ann.serverWasKnown());  // the first visit: remembered from now on
        ann.disconnect();
        visit(h, ann);
        CHECK(ann.state() == Client::State::Lobby && ann.serverWasKnown());
    }
    config.port = port;
    {
        // The same server started again: the same identity, from its file.
        Harness h;
        CHECK(h.server.start(config));
        CHECK(h.server.identity() == identity);
        h.clients.push_back(std::make_unique<Client>());
        visit(h, *h.clients.back());
        CHECK(h.clients.back()->state() == Client::State::Lobby && h.clients.back()->serverWasKnown());
    }
    {
        // Another server at that address (no identity file: a fresh identity): refused, with
        // the reason; after forgetting the old one it is a first visit again.
        Harness h;
        config.identityFile.clear();
        CHECK(h.server.start(config));
        CHECK(h.server.identity() != identity);
        h.clients.push_back(std::make_unique<Client>());
        Client& ann = *h.clients.back();
        visit(h, ann);
        CHECK(ann.state() == Client::State::Failed);
        CHECK(ann.identityChanged());
        CHECK(ann.error().find(identity) != std::string::npos && ann.error().find(h.server.identity()) != std::string::npos);
        CHECK_EQ(h.server.players(), 0);
        ann.forgetServer("127.0.0.1:" + std::to_string(port));
        visit(h, ann);
        CHECK(ann.state() == Client::State::Lobby && !ann.serverWasKnown() && !ann.identityChanged());
    }
    std::filesystem::remove(identityFile);
    std::filesystem::remove(knownFile);
}

// The network keys of the settings file.
void testNetSettings() {
    Settings s;
    s.parse("net_name=Ann\nnet_address=example.org:1234\nnet_server_name=Ann's game\nnet_port=30000\n");
    CHECK(s.netName == "Ann");
    CHECK(s.netAddress == "example.org:1234");
    CHECK(s.netServerName == "Ann's game");
    CHECK_EQ(s.netPort, 30000);
    Settings t;
    t.parse(s.serialize());
    CHECK(t.netName == "Ann" && t.netAddress == "example.org:1234" && t.netServerName == "Ann's game" && t.netPort == 30000);
    t.parse("net_port=99999\n");
    CHECK_EQ(t.netPort, 30000);
}

void testLanBrowser() {
    // Only the answer path is checked: a query sent straight to the server's port.
    Server server;
    ServerConfig config;
    config.port = 0;
    config.name = "Test server";
    config.password = "x";
    CHECK(server.start(config));
    UdpSocket udp;
    CHECK(udp.open(0));
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(static_cast<std::uint8_t>(UdpMsg::Query));
    w.u16(kProtocolVersion);
    CHECK(udp.sendTo({kLoopback, server.port()}, w.data()));
    ServerInfo info;
    bool answered = false;
    for (int i = 0; i < 2000 && !answered; ++i) {
        server.update(1000 + static_cast<std::uint64_t>(i));
        Address from;
        std::vector<std::uint8_t> data;
        if (udp.receiveFrom(from, data)) {
            ByteReader r(data);
            answered = r.u32() == kUdpMagic && static_cast<UdpMsg>(r.u8()) == UdpMsg::Info && decode(r, info);
        }
        std::this_thread::yield();
    }
    // The browser finds it although it plays on some other port: the search goes to the
    // discovery port, which every server listens on.
    LanBrowser browser;
    bool listed = false;
    for (int i = 0; i < 4000 && !listed; ++i) {
        server.update(5000 + static_cast<std::uint64_t>(i));
        browser.update(5000 + static_cast<std::uint64_t>(i));
        for (const LanBrowser::Entry& e : browser.servers()) listed = listed || (e.info.name == "Test server" && e.address.port == server.port());
        std::this_thread::yield();
    }
    CHECK(listed);
    CHECK(answered);
    CHECK(info.name == "Test server");
    CHECK_EQ(info.port, server.port());
    CHECK(info.password);
    CHECK_EQ(info.players, 0);
}

}  // namespace

// With an argument: only the test of that name (for running one of them many times).
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"codec", testCodec},
        {"link", testLink},
        {"snapshot fields", testSnapshotFields},
        {"the same on every machine", testSameOnEveryMachine},
        {"cryptography test vectors", testCrypto},
        {"lobby", testLobby},
        {"encryption on the wire", testEncryptionOnTheWire},
        {"bans and limits", testBansAndLimits},
        {"server identity", testServerIdentity},
        {"player keys and certificate", testPlayerKeysAndCertificate},
        {"records", testRecords},
        {"records: the inputs applied follow the log", testRecordInputOrder},
        {"carrier", testCarrier},
        {"ready", testReady},
        {"round over udp", [] { testRound(true); }},
        {"round over tcp only", [] { testRound(false); }},
        {"leave during round", testLeaveDuringRound},
        {"udp loss", testUdpLoss},
        {"loss from the first packet", testLossFromTheStart},
        {"no udp path", testNoUdpPath},
        {"left behind", testLeftBehind},
        {"cookie", testCookie},
        {"prediction", testPrediction},
        {"back to the lobby", testBackToLobby},
        {"two players at one computer", testTwoPlayersOneComputer},
        {"roulette prize", testNetRoulette},
        {"campaign", testNetCampaign},
        {"router port mapping", testUpnp},
        {"ipv6 and ipv4 together", testIpv6},
        {"settings keys", testNetSettings},
        {"lan answer", testLanBrowser},
    };
    for (const auto& [name, fn] : tests) {
        if (!only.empty() && name != only) continue;
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
