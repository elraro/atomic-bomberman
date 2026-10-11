// Developer tool: a network client without a window. Joins a server, presses
// random keys during the rounds and reports what it saw. Used to exercise a
// real server (see docs/specifications/networking.md); not installed.
//   net_bot ADDRESS NAME [--start] [--start-after MS] [--tcp] [--password TEXT] [--matches N]
// With --online-config URL --online-key HEX the address may be "code:123456" (a game found
// on the master server by its code) or "listed:NAME" (the game of that name on its list).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "net/client.hpp"
#ifdef AB_ONLINE
#include "online/bootstrap.hpp"
#include "online/online.hpp"
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("Usage: net_bot ADDRESS NAME [--start] [--start-after MS] [--tcp] [--password TEXT] [--matches N]");
        return 2;
    }
    bool start = false;
    int matches = 1;
    std::uint64_t startAfter = 1500;
    std::string password, onlineConfig, onlineKey;
    ab::net::Client client;
    client.setAlwaysReady(true);
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--start") start = true;
        else if (a == "--start-after" && i + 1 < argc) start = true, startAfter = static_cast<std::uint64_t>(std::atoi(argv[++i]));
        else if (a == "--tcp") client.disableUdp();
        else if (a == "--password" && i + 1 < argc) password = argv[++i];
        else if (a == "--matches" && i + 1 < argc) matches = std::atoi(argv[++i]);
        else if (a == "--online-config" && i + 1 < argc) onlineConfig = argv[++i];
        else if (a == "--online-key" && i + 1 < argc) onlineKey = argv[++i];
    }
    using State = ab::net::Client::State;
    const std::string name = argv[2];
    std::string address = argv[1];
    std::vector<std::string> candidates;  // the ways to the server the master named
    if (address.rfind("code:", 0) == 0 || address.rfind("listed:", 0) == 0) {
#ifdef AB_ONLINE
        ab::online::Bootstrap bootstrap;
        std::string why;
        if (!ab::online::chooseBootstrap(onlineConfig, onlineKey, bootstrap, why)) {
            std::printf("[%s] failed: %s\n", name.c_str(), why.c_str());
            return 1;
        }
        ab::online::Online master;
        master.configure(bootstrap, "", name, ab::net::kProtocolVersion);
        auto wait = [&]() {
            for (int i = 0; i < 15000 && master.busy(); ++i) {
                master.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            master.update();
            if (master.busy() || !master.error().empty()) {
                std::printf("[%s] failed: master: %s\n", name.c_str(), master.busy() ? "no answer" : master.error().c_str());
                return false;
            }
            return true;
        };
        std::string id, code;
        if (address.rfind("code:", 0) == 0) {
            code = address.substr(5);
            master.findByCode(code);
            ab::online::LobbyInfo found;
            if (!wait() || !master.takeFound(found)) return 1;
            id = found.id;
            std::printf("[%s] master: code %s is \"%s\"\n", name.c_str(), code.c_str(), found.name.c_str());
        } else {
            master.refreshLobbies();
            if (!wait()) return 1;
            for (const ab::online::LobbyInfo& l : master.lobbies())
                if (l.name == address.substr(7)) id = l.id;
            std::printf("[%s] master: %zu game(s) listed\n", name.c_str(), master.lobbies().size());
            if (id.empty()) {
                std::printf("[%s] failed: no game of that name is listed\n", name.c_str());
                return 1;
            }
        }
        master.join(id, code);
        ab::online::JoinInfo info;
        if (!wait() || !master.takeJoin(info) || info.candidates.empty()) {
            std::printf("[%s] failed: the master named no way to the game\n", name.c_str());
            return 1;
        }
        candidates = info.candidates;
        client.expectIdentity(info.identityKey);
        address = candidates.front();
        std::printf("[%s] master: reach it at %s (and %zu more)\n", name.c_str(), address.c_str(), candidates.size() - 1);
#else
        std::printf("[%s] failed: built without online play\n", name.c_str());
        return 1;
#endif
    }
    std::size_t candidate = 0;
    client.connect(address, name, password, ab::net::clockMs());
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
            if (state == State::Failed && !client.answered() && candidate + 1 < candidates.size()) {
                // Nobody there: the next way the master named.
                client.connect(candidates[++candidate], name, password, now);
                last = State::Idle;
                continue;
            }
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
