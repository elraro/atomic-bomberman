// Developer tool: a network client without a window. Joins a server, presses
// random keys during the rounds and reports what it saw. Used to exercise a
// real server (see docs/specifications/networking.md); not installed.
//   net_bot ADDRESS NAME [--start] [--start-after MS] [--tcp] [--password TEXT] [--matches N]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "net/client.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("Usage: net_bot ADDRESS NAME [--start] [--start-after MS] [--tcp] [--password TEXT] [--matches N]");
        return 2;
    }
    bool start = false;
    int matches = 1;
    std::uint64_t startAfter = 1500;
    std::string password;
    ab::net::Client client;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--start") start = true;
        else if (a == "--start-after" && i + 1 < argc) start = true, startAfter = static_cast<std::uint64_t>(std::atoi(argv[++i]));
        else if (a == "--tcp") client.disableUdp();
        else if (a == "--password" && i + 1 < argc) password = argv[++i];
        else if (a == "--matches" && i + 1 < argc) matches = std::atoi(argv[++i]);
    }
    using State = ab::net::Client::State;
    const std::string name = argv[2];
    client.connect(argv[1], name, password, ab::net::clockMs());
    ab::Rng rng(static_cast<std::uint32_t>(ab::net::clockMs()) + static_cast<std::uint32_t>(name.size()) * 7919u);
    State last = State::Idle;
    std::uint64_t lobbySince = 0, inputAt = 0;
    std::size_t chatSeen = 0;
    int played = 0;
    bool inMatch = false;
    while (true) {
        const std::uint64_t now = ab::net::clockMs();
        client.update(now);
        client.advance(now, nullptr, nullptr);
        const State state = client.state();
        for (; chatSeen < client.chat().size(); ++chatSeen) {
            const ab::net::ChatLine& line = client.chat()[chatSeen];
            std::printf("[%s] chat %s: %s\n", name.c_str(), line.fromServer ? "*" : line.name.c_str(), line.text.c_str());
        }
        if (state != last) {
            if (state == State::Failed) {
                std::printf("[%s] failed: %s\n", name.c_str(), client.error().c_str());
                return 1;
            }
            if (state == State::Lobby) {
                std::printf("[%s] lobby: server \"%s\" seat=%d admin=%d udp=%d\n", name.c_str(), client.serverName().c_str(), client.seat(),
                            client.isAdmin() ? 1 : 0, client.udpActive() ? 1 : 0);
                lobbySince = now;
                if (inMatch && ++played >= matches) {
                    std::printf("[%s] done: matches=%d snapshots=%d udp=%d\n", name.c_str(), played, client.snapshotsLoaded(), client.udpActive() ? 1 : 0);
                    return client.snapshotsLoaded() == 0 ? 0 : 3;
                }
                inMatch = false;
            }
            if (state == State::Round) {
                inMatch = true;
                std::printf("[%s] round %u: level=%d players=%d\n", name.c_str(), client.roundId(), client.setup().level, client.world()->alivePlayers());
            }
            if (state == State::Result) {
                std::printf("[%s] result: steps=%u winner=%d match=%d hash=%08x snapshots=%d\n", name.c_str(), client.stepsApplied(),
                            client.result().winner, client.score().matchWinner, client.world()->stateHash(true), client.snapshotsLoaded());
                client.sendContinue();
            }
            std::fflush(stdout);
            last = state;
        }
        if (state == State::Lobby && start && client.isAdmin() && client.lobby().phase == ab::net::Phase::Lobby && now - lobbySince > startAfter) {
            client.sendChat("starting");
            client.sendStart();
            lobbySince = now;
        }
        if (state == State::Round && now - inputAt > 200) {
            inputAt = now;
            ab::PlayerInput in;
            if (rng.below(4) != 0) in.dir[static_cast<std::size_t>(rng.below(4))] = true;
            in.button1 = rng.below(6) == 0;
            in.button2 = rng.below(10) == 0;
            client.setInput(in);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
