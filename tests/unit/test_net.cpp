// Tests for the network mode (docs/specifications/networking.md): the message
// encoding, and a server with clients in one process over 127.0.0.1. Time is
// simulated (the server and clients take the clock as an argument), so a whole
// round runs in well under a second.
#include <algorithm>
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
#include "net/protocol.hpp"
#include "net/relay.hpp"
#include "net/server.hpp"
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

    StepsMsg steps;
    steps.roundId = 9;
    steps.firstStep = 100;
    steps.steps.resize(3);
    steps.steps[2][9] = 63;
    steps.hash = 0x12345678u;
    StepsMsg steps2;
    {
        const std::vector<std::uint8_t> sb = encoded(steps);
        ByteReader r(sb);
        CHECK(decode(r, steps2) && r.atEnd());
    }
    CHECK_EQ(steps2.steps.size(), 3u);
    CHECK_EQ(steps2.steps[2][9], 63);
    CHECK_EQ(steps2.hash, 0x12345678u);

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

// A server and its clients on one simulated clock.
struct Harness {
    Server server;
    std::vector<std::unique_ptr<Client>> clients;
    std::uint64_t now = 100000;
    std::vector<std::string> log;
    std::string gameDir;  // game data for the server (empty: the built-in arena)

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
        clients.push_back(std::make_unique<Client>());
        if (!udp) clients.back()->disableUdp();
        clients.back()->connect("127.0.0.1:" + std::to_string(server.port()), name, password, now);
        return *clients.back();
    }
    void turn() {
        server.update(now);
        for (auto& c : clients) {
            c->update(now);
            c->advance(now, nullptr, nullptr);
        }
        now += 5;
        std::this_thread::yield();
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

    // A client whose state has gone wrong is put right by the server.
    bob.world()->player(0).kills += 5;
    CHECK(h.until([&] { return bob.snapshotsLoaded() == 1; }));
    CHECK_EQ(ann.snapshotsLoaded(), 0);

    // Someone who connects now watches the round from here.
    Client& cat = h.join("Cat", "", udp);
    CHECK(h.until([&] { return cat.state() == Client::State::Round && cat.stepsApplied() > 0; }));
    CHECK_EQ(cat.seat(), -1);
    CHECK_EQ(cat.snapshotsLoaded(), 1);

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
    CHECK_EQ(ann.snapshotsLoaded(), 0);
    CHECK_EQ(bob.snapshotsLoaded(), 1);
    CHECK_EQ(cat.snapshotsLoaded(), 1);
    // Back in the lobby the watcher gets a seat.
    CHECK(h.until([&] { return ann.state() == Client::State::Lobby && cat.state() == Client::State::Lobby && cat.seat() >= 0; }));
    CHECK(ann.world() == nullptr);
    CHECK(h.server.phase() == Phase::Lobby);
    for (const std::string& line : h.log) CHECK(line.rfind("ERROR", 0) != 0);
}

// A player who leaves in the middle is played by the computer; the others go on.
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
    int ahead = 0, samples = 0;
    auto watch = [&](int ms) {
        for (int waited = 0; waited < ms; waited += 5) {
            h.turn();
            if (ann.state() != Client::State::Round) break;
            confirmed[ann.stepsApplied()] = ann.world()->stateHash(true);
            if (ann.view() != ann.world()) {
                predicted[ann.viewStep()] = ann.view()->stateHash(true);
                ahead += static_cast<int>(ann.viewStep()) - static_cast<int>(ann.stepsApplied());
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
    // Ann keeps walking east and the others stand still: every predicted state comes true.
    watch(4000);
    CHECK(samples > 100);
    CHECK(ahead >= samples * 3 && ahead <= samples * 5);  // about four steps ahead
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
    // Somebody else's change of mind is what prediction cannot know: Bob starts walking,
    // Ann's picture is briefly wrong about him and then right again.
    bob.setInput(east);
    watch(3000);
    CHECK(wrong(ann.stepsApplied() - 30, ann.stepsApplied()) == 0);
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
    // One flipped bit in a later message: the server ends the connection rather than act on it.
    tamperAt = frames + 1;
    ann.sendChat("this one is altered on the way");
    CHECK(spin([&] { return ann.state() == Client::State::Failed; }));
    CHECK_EQ(h.server.players(), 0);
    bool arrived = false;
    for (const std::string& line : h.log) arrived = arrived || line.find("altered on the way") != std::string::npos;
    CHECK(!arrived);

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

// A host nobody can reach and a player: joined through a relay, by a code.
void testRelay() {
    std::string code, relayAddress;
    CHECK(splitRelayAddress("ab12cd@relay.example:9", &code, &relayAddress) && code == "AB12CD" && relayAddress == "relay.example:9");
    CHECK(!splitRelayAddress("127.0.0.1:27410", &code, &relayAddress));
    CHECK(!splitRelayAddress("@relay", &code, &relayAddress) && !splitRelayAddress("CODE@", &code, &relayAddress));

    Relay relay;
    std::vector<std::string> relayLog;
    CHECK(relay.start(0, [&](const std::string& line) { relayLog.push_back(line); }));
    Harness h;
    ServerConfig config;
    config.port = 0;
    config.seed = 99;
    config.discoverable = false;
    config.password = "relayed";
    config.relay = "127.0.0.1:" + std::to_string(relay.port());
    config.settings.computers = 1;
    config.log = [&h](const std::string& line) { h.log.push_back(line); };
    CHECK(h.server.start(config));
    auto spin = [&](const std::function<bool()>& done, int maxMs = 20000) {
        for (int waited = 0; waited < maxMs && !done(); waited += 5) {
            relay.update(h.now);
            h.turn();
        }
        return done();
    };
    CHECK(spin([&] { return !h.server.relayCode().empty(); }));
    CHECK_EQ(h.server.relayCode().size(), 6u);
    CHECK_EQ(relay.hosts(), 1);
    const std::string through = h.server.relayCode() + "@127.0.0.1:" + std::to_string(relay.port());

    // A wrong code is refused by the relay, with words.
    Client& lost = h.join("Lost");
    lost.connect("ZZZZZZ@127.0.0.1:" + std::to_string(relay.port()), "Lost", "relayed", h.now);
    CHECK(spin([&] { return lost.state() == Client::State::Failed; }));
    CHECK(lost.error() == "No game with that code on this relay");

    // The right code: the usual key exchange, password and lobby, all through the relay.
    h.clients.push_back(std::make_unique<Client>());
    Client& ann = *h.clients.back();
    ann.connect(through, "Ann", "relayed", h.now);
    CHECK(spin([&] { return ann.state() == Client::State::Lobby && !ann.lobby().clients.empty(); }));
    CHECK(ann.viaRelay() && ann.encrypted() && !ann.udpActive());
    CHECK(ann.serverIdentity() == h.server.identity());  // the relay could not put itself in between
    CHECK_EQ(relay.links(), 1);
    // A second player the same way, with the wrong password: the server's own refusal arrives.
    h.clients.push_back(std::make_unique<Client>());
    Client& eve = *h.clients.back();
    eve.connect(through, "Eve", "guess", h.now);
    CHECK(spin([&] { return eve.state() == Client::State::Failed; }));
    CHECK(eve.error() == "Wrong password");
    // A round over the relay stays in step.
    ann.sendChat("through the relay");
    ann.sendStart();
    CHECK(spin([&] { return ann.state() == Client::State::Round && ann.stepsApplied() > 80; }));
    CHECK(spin([&] { return ann.stepsApplied() == h.server.steps() && ann.world()->stateHash(true) == h.server.world()->stateHash(true); }, 3000));
    CHECK_EQ(ann.snapshotsLoaded(), 0);
    bool chatSeen = false;
    for (const std::string& line : h.log) chatSeen = chatSeen || line.find("through the relay") != std::string::npos;
    CHECK(chatSeen);
    // The player leaves: the link goes. The host stops: the relay forgets it.
    ann.disconnect();
    CHECK(spin([&] { return relay.links() == 0; }));
    h.server.stop();
    CHECK(spin([&] { return relay.hosts() == 0; }));
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

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"codec", testCodec},
        {"cryptography test vectors", testCrypto},
        {"lobby", testLobby},
        {"encryption on the wire", testEncryptionOnTheWire},
        {"bans and limits", testBansAndLimits},
        {"server identity", testServerIdentity},
        {"relay", testRelay},
        {"round over udp", [] { testRound(true); }},
        {"round over tcp only", [] { testRound(false); }},
        {"leave during round", testLeaveDuringRound},
        {"udp loss", testUdpLoss},
        {"prediction", testPrediction},
        {"two players at one computer", testTwoPlayersOneComputer},
        {"roulette prize", testNetRoulette},
        {"campaign", testNetCampaign},
        {"router port mapping", testUpnp},
        {"ipv6 and ipv4 together", testIpv6},
        {"settings keys", testNetSettings},
        {"lan answer", testLanBrowser},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
