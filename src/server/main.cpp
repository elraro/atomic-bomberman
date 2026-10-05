// Dedicated server of the network mode: the game server alone, with no window,
// no graphics and no SDL. See docs/specifications/networking.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/server.hpp"
#include "resources/settings.hpp"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

bool looksLikeGameDir(const std::string& dir) {
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
    "The first player to join is the administrator: changes the settings, starts the match.\n"
    "Commands on standard input: status, say TEXT, quit.\n";

}  // namespace

int main(int argc, char** argv) {
    ab::net::ServerConfig config;
    config.name = "Atomic Bomberman server";
    std::string gameDir;
    bool schemesDirGiven = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
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
        else {
            std::fprintf(stderr, "ERROR unknown argument %s (see --help)\n", a.c_str());
            return 2;
        }
    }
    const std::string requested = gameDir;
    config.gameDir = findGameDir(requested);
    if (!schemesDirGiven)
        if (const std::string user = userDataDir(); !user.empty()) config.userSchemesDir = user + "schemes";
    config.log = logLine;
    if (config.gameDir.empty())
        logLine("WARN  No game data found" + (requested.empty() ? std::string() : " under " + requested) +
                ": serving the built-in arena with default values (see --game-dir)");
    else
        logLine("INFO  Game data: " + config.gameDir);

    ab::net::Server server;
    if (!server.start(config)) {
        logLine("ERROR " + server.error());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

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
                static const char* const kPhase[3] = {"lobby", "round", "result"};
                const ab::net::LobbyState& lobby = server.lobby();
                logLine("INFO  Status phase=" + std::string(kPhase[static_cast<int>(server.phase())]) + " players=" +
                        std::to_string(server.players()) + " scheme=\"" + lobby.settings.schemeTitle + "\" level=" +
                        std::to_string(lobby.settings.level) + " wins=" + std::to_string(lobby.settings.winsNeeded) + " time=" +
                        ab::Settings::playTimeText(lobby.settings.playTime));
                for (const ab::net::ClientInfo& c : lobby.clients)
                    logLine("INFO    id=" + std::to_string(c.id) + " name=\"" + c.name + "\" seat=" + std::to_string(c.seat) + " ping=" +
                            std::to_string(c.pingMs) + "ms" + (c.id == lobby.admin ? " administrator" : ""));
            } else if (!line.empty()) {
                logLine("INFO  Commands: status, say TEXT, quit");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    logLine("INFO  Shutting down");
    server.stop();
    std::fflush(stdout);
    std::_Exit(0);  // the console thread may still be waiting for a line
}
