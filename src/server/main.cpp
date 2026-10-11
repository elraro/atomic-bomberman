// Dedicated server of the network mode: the game server alone, with no window,
// no graphics and no SDL. See docs/specifications/networking.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "free/free_data.hpp"
#include "net/relay.hpp"
#ifdef AB_ONLINE
#include "online/announcer.hpp"
#include "online/bootstrap.hpp"
#include "online/certificate.hpp"
#include "online/json.hpp"
#include "online/tournament.hpp"
#endif
#include "net/server.hpp"
#include "resources/settings.hpp"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

bool looksLikeGameDir(const std::string& dir) {
    if (ab::isFreeAssetDir(dir)) return true;
    return std::ifstream(dir + "/color.pal").good() && std::ifstream(dir + "/data/res/valuelst.res").good();
}

// The per-user folder the game itself uses (SDL's "pref path"), worked out without SDL.
std::string userDataDir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) return std::string(appdata) + "\\atomic-bomberman-modern\\atomic\\";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0')
        return std::string(xdg) + "/atomic-bomberman-modern/atomic/";
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/.local/share/atomic-bomberman-modern/atomic/";
#endif
    return "";
}

std::string findGameDir(const std::string& requested) {
    std::vector<std::string> candidates;
    if (!requested.empty()) candidates.push_back(requested);
    if (const char* env = std::getenv("ATOMIC_GAME_DIR")) candidates.emplace_back(env);
    candidates.emplace_back("assets");
    if (const std::string user = userDataDir(); !user.empty()) candidates.push_back(user + "assets");
    candidates.emplace_back("game");
    for (const std::string& c : candidates)
        if (looksLikeGameDir(c)) return c;
    return "";
}

void logLine(const std::string& line) {
    const std::time_t t = std::time(nullptr);
    char stamp[32] = "";
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::printf("%s %s\n", stamp, line.c_str());
    std::fflush(stdout);
}

const char* const kUsage =
    "Usage: atomic_server [options]\n"
    "  --version          print the release number and exit\n"
    "  --port N           TCP and UDP port (default 27410)\n"
    "  --name TEXT        server name shown to players\n"
    "  --password TEXT    players must give this password\n"
    "  --game-dir DIR     imported assets or an original game folder (for schemes, values, level extras)\n"
    "  --schemes-dir DIR  extra schemes (default: the per-user schemes folder of the game)\n"
    "  --scheme NAME      first scheme, e.g. BASIC\n"
    "  --level N          level 0-10, or -1 for a random level each match (default 0)\n"
    "  --wins N           round wins needed for the match (default 2)\n"
    "  --computers N      computer players (default 1)\n"
    "  --play-time N      seconds per round: 60 90 120 150 180 240 300 600, or 0 for no limit (default 150)\n"
    "  --team-play        two teams\n"
    "  --hidden           do not answer searches on the local network\n"
    "  --upnp             ask the router to forward the port (UPnP), for a server behind a home router\n"
    "The first player to join is the administrator: changes the settings, starts the match.\n"
    "  --admin-password TEXT  a player who types /login TEXT in the chat becomes administrator\n"
    "  --relay ADDRESS        register with a relay, for players who cannot reach this machine (they join with CODE@ADDRESS)\n"
    "  --relay-server         be a relay instead of a game server (port 27408 unless --port is given)\n"
    "  --identity-file FILE   the server's lasting key (default: server.key in the per-user folder)\n"
    "  --certificate FILE     this server's certificate (who answers for it), shown to every player\n"
    "  --signing-key-file FILE  the key match records are signed with (default: server.sign.key in the per-user folder)\n"
    "  --records-dir DIR      where the signed record of every round is kept (default: records in the per-user folder)\n"
    "  --tournament FILE#KEY  serve a match of that tournament (a file or an address, and the organiser's key)\n"
    "  --match ID             which one: only its players are let in, and its rules cannot be changed\n"
    "  --show-keys            print the two keys a certificate is made out to, and exit\n"
    "  --online listed|code   announce the game on the master server: in its list, or to be found by its code only\n"
    "  --online-config URL    where the signed configuration of online play is (several: commas)\n"
    "  --online-key HEX       the key that must have signed it\n"
    "  --ban-file FILE    where banned addresses are kept (default: bans.txt in the per-user folder)\n"
    "Commands on standard input: status, say TEXT, kick NAME, ban NAME, unban ADDRESS, bans, quit.\n";

}  // namespace

int main(int argc, char** argv) {
    ab::net::ServerConfig config;
    config.name = "Atomic Bomberman server";
    std::string gameDir;
    bool schemesDirGiven = false;
    bool relayServer = false, portGiven = false;
    std::string onlineMode, onlineConfig, onlineKey;
    std::string certificateFile, signingKeyFile, recordsDir, tournamentAt, matchId;
    bool showKeys = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--version") {
            std::printf("Atomic Bomberman (modern) server " AB_VERSION " (protocol %d)\n", static_cast<int>(ab::net::kProtocolVersion));
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--port") {
            const int port = std::atoi(next().c_str());
            if (port < 1 || port > 65535) {
                std::fprintf(stderr, "ERROR --port needs a number from 1 to 65535\n");
                return 2;
            }
            config.port = static_cast<std::uint16_t>(port);
            portGiven = true;
        } else if (a == "--name") config.name = ab::net::cleanText(next(), 32);
        else if (a == "--password") config.password = next();
        else if (a == "--game-dir") gameDir = next();
        else if (a == "--schemes-dir") config.userSchemesDir = next(), schemesDirGiven = true;
        else if (a == "--scheme") config.scheme = next();
        else if (a == "--level") config.settings.level = std::clamp(std::atoi(next().c_str()), -1, 10);
        else if (a == "--wins") config.settings.winsNeeded = std::clamp(std::atoi(next().c_str()), 1, 9);
        else if (a == "--computers") config.settings.computers = std::clamp(std::atoi(next().c_str()), 0, ab::kMaxPlayers - 1);
        else if (a == "--play-time") {
            const int seconds = std::atoi(next().c_str());
            config.settings.playTime = seconds <= 0 ? ab::kInfinitePlayTime : std::clamp(seconds, 30, 1000);
        } else if (a == "--team-play") config.settings.teamPlay = true;
        else if (a == "--hidden") config.discoverable = false;
        else if (a == "--upnp") config.upnp = true;
        else if (a == "--ban-file") config.banFile = next();
        else if (a == "--admin-password") config.adminPassword = next();
        else if (a == "--identity-file") config.identityFile = next();
        else if (a == "--relay") config.relay = next();
        else if (a == "--relay-server") relayServer = true;
        else if (a == "--certificate") certificateFile = next();
        else if (a == "--signing-key-file") signingKeyFile = next();
        else if (a == "--records-dir") recordsDir = next();
        else if (a == "--tournament") tournamentAt = next();
        else if (a == "--match") matchId = next();
        else if (a == "--show-keys") showKeys = true;
        else if (a == "--online") onlineMode = next();
        else if (a == "--online-config") onlineConfig = next();
        else if (a == "--online-key") onlineKey = next();
        else {
            std::fprintf(stderr, "ERROR unknown argument %s (see --help)\n", a.c_str());
            return 2;
        }
    }
    if (relayServer) {
        // A relay: links hosts and players who cannot reach each other. It carries their
        // encrypted traffic and knows nothing of the game.
        ab::net::Relay relay;
        if (!relay.start(portGiven ? config.port : ab::net::kRelayPort, logLine)) {
            logLine("ERROR Cannot listen on TCP port " + std::to_string(portGiven ? config.port : ab::net::kRelayPort));
            return 1;
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        while (!g_stop) {
            relay.update(ab::net::clockMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        logLine("INFO  Shutting down");
        return 0;
    }
    const std::string requested = gameDir;
    config.gameDir = findGameDir(requested);
    if (!schemesDirGiven)
        if (const std::string user = userDataDir(); !user.empty()) config.userSchemesDir = user + "schemes";
    if (config.banFile.empty() && !userDataDir().empty()) config.banFile = userDataDir() + "bans.txt";
    if (config.identityFile.empty() && !userDataDir().empty()) config.identityFile = userDataDir() + "server.key";
    config.log = logLine;
    if (config.gameDir.empty()) {
        // No original game data: the arenas and level extras of the free asset set.
        const std::string freeDir = userDataDir().empty() ? std::string("free-assets") : userDataDir() + "free-assets";
        if (ab::ensureFreeAssets(freeDir, false)) config.gameDir = freeDir;
    }
    if (config.gameDir.empty())
        logLine("WARN  No game data found" + (requested.empty() ? std::string() : " under " + requested) +
                ": serving the built-in arena with default values (see --game-dir)");
    else
        logLine("INFO  Game data: " + config.gameDir + (ab::isFreeAssetDir(config.gameDir) ? " (free asset set)" : ""));

#ifdef AB_ONLINE
    // The signing key (for match records) and the way to check players' keys.
    if (signingKeyFile.empty() && !userDataDir().empty()) signingKeyFile = userDataDir() + "server.sign.key";
    ab::online::Seed signingSeed{};
    const bool haveSigningKey = !signingKeyFile.empty() && ab::online::loadOrCreateSeed(signingKeyFile, signingSeed);
    if (!haveSigningKey) logLine("WARN  No signing key (cannot read or write " + (signingKeyFile.empty() ? std::string("a key file") : signingKeyFile) + ")");
    config.verifySignature = [](const ab::net::Key& key, const std::vector<std::uint8_t>& message, const std::array<std::uint8_t, 64>& signature) {
        ab::online::PublicKey k{};
        std::copy(key.begin(), key.end(), k.begin());
        return ab::online::verify(k, std::string(message.begin(), message.end()), signature);
    };
    // With a signing key every round leaves a signed record, kept here and handed to its players.
    if (recordsDir.empty() && !userDataDir().empty()) recordsDir = userDataDir() + "records";
    config.gameVersion = AB_VERSION;
    if (haveSigningKey && !showKeys) {
        config.signRecord = [signingSeed](const std::vector<std::uint8_t>& bytes) { return ab::online::signDocument(signingSeed, std::string(bytes.begin(), bytes.end())); };
        config.recordSink = [recordsDir](const std::string& document, const ab::net::MatchRecord& record) {
            if (recordsDir.empty()) return;
            std::error_code ec;
            std::filesystem::create_directories(recordsDir, ec);
            char stamp[32];
            const std::time_t at = static_cast<std::time_t>(record.startedUnix);
            std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::gmtime(&at));
            const std::string file = recordsDir + "/" + stamp + "-round" + std::to_string(record.start.roundId) + ".abrec";
            std::ofstream out(file, std::ios::binary);
            out << document;
            logLine(out ? "INFO  Record kept: " + file : "WARN  Cannot write the record " + file);
        };
    }
    if (!certificateFile.empty()) {
        std::ifstream in(certificateFile, std::ios::binary);
        config.certificate.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (config.certificate.empty() || config.certificate.size() > 8000) {
            logLine("ERROR cannot read a certificate from " + certificateFile);
            return 2;
        }
    }
#else
    if (!certificateFile.empty() || showKeys || !tournamentAt.empty()) {
        logLine("ERROR this server was built without online play: no certificates, no tournaments");
        return 2;
    }
#endif
#ifdef AB_ONLINE
    // A tournament's match: its players only, its rules, and this server must be the one the file names for it.
    ab::online::Tournament tournament;
    const ab::online::TournamentMatch* tournamentMatch = nullptr;
    if (!tournamentAt.empty() && !showKeys) {
        std::string location, keyHex, envelope, why;
        ab::online::splitInvitation(tournamentAt, location, keyHex);
        ab::online::PublicKey organiser{};
        if (!ab::online::fromHex(keyHex, organiser.data(), organiser.size())) {
            logLine("ERROR --tournament needs the organiser's key after a #: FILE#KEY");
            return 2;
        }
        if (!ab::online::fetchTournament(location, envelope, why) || !ab::online::readTournament(envelope, &organiser, 0, tournament, why)) {
            logLine("ERROR tournament: " + why);
            return 2;
        }
        tournamentMatch = tournament.match(matchId);
        if (tournamentMatch == nullptr) {
            logLine("ERROR the tournament has no match \"" + matchId + "\" (see --match)");
            return 2;
        }
        if (tournament.rules.gameVersion != AB_VERSION || tournament.rules.protocol != ab::net::kProtocolVersion) {
            logLine("ERROR the tournament is played with release " + tournament.rules.gameVersion + " (protocol " + std::to_string(tournament.rules.protocol) + "); this is " AB_VERSION " (protocol " +
                    std::to_string(ab::net::kProtocolVersion) + ")");
            return 2;
        }
        for (const std::string& who : tournamentMatch->players) {
            const ab::online::TournamentPlayer* player = tournament.player(who);
            if (player == nullptr) {
                logLine("ERROR match " + matchId + " is not ready: " + who + " has not been decided");
                return 2;
            }
            ab::net::Key key{};
            std::copy(player->key.begin(), player->key.end(), key.begin());
            config.allowedKeys.push_back(key);
        }
        config.lockSettings = true;
        config.password.clear();
        config.name = ab::net::cleanText(tournament.name + " " + matchId, 32);
        config.scheme = tournament.rules.scheme;
        config.settings.winsNeeded = tournament.rules.wins;
        config.settings.level = tournament.rules.level;
        config.settings.playTime = tournament.rules.playTime;
        config.settings.enclosementDepth = tournament.rules.enclosement;
        config.settings.teamPlay = tournament.rules.teamPlay;
        config.settings.computers = tournament.rules.computers;
        if (!haveSigningKey) {
            logLine("ERROR a tournament match needs a signing key for its records");
            return 2;
        }
    }
#endif
    if (showKeys) {
        config.port = 0;
        config.discoverable = false;
        config.upnp = false;
        config.relay.clear();
        config.log = {};
    }
    ab::net::Server server;
    if (!server.start(config)) {
        logLine("ERROR " + server.error());
        return 1;
    }
#ifdef AB_ONLINE
    if (showKeys) {
        // What the owner of the root key needs to make out a certificate (abconfig cert).
        std::printf("identity key: %s\nsigning key:  %s\n", server.identityKeyHex().c_str(),
                    haveSigningKey ? ab::online::toHex(ab::online::publicKeyOf(signingSeed)).c_str() : "(none)");
        server.stop();
        return haveSigningKey ? 0 : 1;
    }
    if (tournamentMatch != nullptr) {
        const ab::online::TournamentServer* named = tournament.server(tournamentMatch->server);
        if (named == nullptr || named->identityKey != server.identityKeyHex() || named->signingKey != ab::online::publicKeyOf(signingSeed)) {
            logLine("ERROR this is not the server the tournament names for match " + matchId + " (other keys; see --show-keys)");
            return 2;
        }
        std::string who;
        for (const std::string& name : tournamentMatch->players) who += (who.empty() ? "" : ", ") + name;
        logLine("INFO  Tournament \"" + tournament.name + "\" match " + matchId + ": " + who + " (first to " + std::to_string(tournament.rules.wins) + " rounds)");
    }
    if (!config.certificate.empty()) {
        // Is it this server's? (Whether it holds is for the players' games to judge: they have the root keys.)
        ab::online::Json envelope, document;
        std::string text;
        const bool readable = ab::online::Json::parse(config.certificate, envelope) && ab::online::fromBase64(envelope["document"].str(), text) &&
                              ab::online::Json::parse(text, document) && document["kind"].str() == "server-certificate";
        if (!readable || document["identity_key"].str() != server.identityKeyHex() ||
            !haveSigningKey || document["signing_key"].str() != ab::online::toHex(ab::online::publicKeyOf(signingSeed))) {
            logLine("ERROR " + certificateFile + " is not a certificate for this server's keys (see --show-keys)");
            return 2;
        }
        const long long left = (document["expires"].integer() - static_cast<long long>(std::time(nullptr))) / 86400;
        logLine(std::string(left < 14 ? "WARN  " : "INFO  ") + "Certificate " + std::to_string(document["serial"].integer()) + " of \"" +
                ab::net::cleanText(document["operator"]["name"].str(), 60) + "\": " + (left < 0 ? "EXPIRED" : std::to_string(left) + " days left"));
    }
#endif
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // Announced on the master server, if asked for.
    bool online = false;
#ifdef AB_ONLINE
    ab::online::Online master;
    ab::online::HostAnnouncer announcer;
    std::string onlineSaid;
    if (!onlineMode.empty()) {
        ab::online::Bootstrap bootstrap;
        std::string why;
        if (onlineMode != "listed" && onlineMode != "code") {
            logLine("ERROR --online needs \"listed\" or \"code\"");
            return 2;
        }
        if (!config.password.empty()) {
            logLine("ERROR a game announced online has no password; to keep it private use --online code");
            return 2;
        }
        if (!ab::online::chooseBootstrap(onlineConfig, onlineKey, bootstrap, why)) {
            logLine("ERROR " + why);
            return 2;
        }
        master.configure(bootstrap, userDataDir().empty() ? std::string() : userDataDir() + "online-serial.txt", config.name, ab::net::kProtocolVersion);
        online = true;
    }
#else
    if (!onlineMode.empty()) {
        logLine("ERROR this server was built without online play");
        return 2;
    }
#endif

    // Console commands, read on a thread of their own so the server never waits for input.
    std::mutex commandsMutex;
    std::vector<std::string> commands;
    std::thread([&commandsMutex, &commands] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const std::lock_guard<std::mutex> lock(commandsMutex);
            commands.push_back(line);
        }
    }).detach();

    while (!g_stop) {
        server.update(ab::net::clockMs());
#ifdef AB_ONLINE
        if (online) {
            master.update();
            announcer.update(ab::net::clockMs(), master, server, config.name, onlineMode == "listed");
            // Said once each time it changes: the code to give out, or why it is not announced.
            const std::string now = master.announced() ? "INFO  Online: " + onlineMode + ", code " + master.code()
                                    : !master.error().empty() ? "WARN  Online: " + master.error() : std::string();
            if (!now.empty() && now != onlineSaid) logLine(onlineSaid = now);
        }
#endif
        std::vector<std::string> todo;
        {
            const std::lock_guard<std::mutex> lock(commandsMutex);
            todo.swap(commands);
        }
        for (const std::string& line : todo) {
            if (line == "quit" || line == "exit") {
                g_stop = true;
            } else if (line.rfind("say ", 0) == 0) {
                server.say(ab::net::cleanText(line.substr(4), ab::net::kMaxChat));
            } else if (line == "status") {
                static const char* const kPhase[4] = {"lobby", "round", "result", "roulette"};
                const ab::net::LobbyState& lobby = server.lobby();
                logLine("INFO  Status phase=" + std::string(kPhase[static_cast<int>(server.phase())]) + " players=" +
                        std::to_string(server.players()) + " scheme=\"" + lobby.settings.schemeTitle + "\" level=" +
                        std::to_string(lobby.settings.level) + " wins=" + std::to_string(lobby.settings.winsNeeded) + " time=" +
                        ab::Settings::playTimeText(lobby.settings.playTime));
                for (const ab::net::ClientInfo& c : lobby.clients)
                    logLine("INFO    id=" + std::to_string(c.id) + " name=\"" + c.name + "\" seat=" + std::to_string(c.seat) + " ping=" +
                            std::to_string(c.pingMs) + "ms" + (c.id == lobby.admin ? " administrator" : ""));
            } else if (line.rfind("kick ", 0) == 0 || line.rfind("ban ", 0) == 0) {
                const bool ban = line[0] == 'b';
                if (!server.kick(line.substr(ban ? 4 : 5), ban)) logLine("INFO  Nobody of that name is connected");
            } else if (line.rfind("unban ", 0) == 0) {
                if (!server.unban(line.substr(6))) logLine("INFO  That address is not banned");
            } else if (line == "bans") {
                for (const std::string& host : server.bans()) logLine("INFO    banned " + host);
                if (server.bans().empty()) logLine("INFO  Nobody is banned");
            } else if (!line.empty()) {
                logLine("INFO  Commands: status, say TEXT, kick NAME, ban NAME, unban ADDRESS, bans, quit");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    logLine("INFO  Shutting down");
#ifdef AB_ONLINE
    if (online && master.announced()) {
        // The master is told that the game is gone (it would notice within half a minute anyway).
        master.withdraw();
        for (int i = 0; i < 1500 && master.busy(); ++i) {
            master.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
#endif
    (void)online;
    server.stop();
    std::fflush(stdout);
    std::_Exit(0);  // the console thread may still be waiting for a line
}
