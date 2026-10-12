// The game's side of the master server (docs/specifications/online-play.md,
// section 1; master/README.md for the calls): fetch and check the configuration,
// open a guest session, list games, find one by its code, join one, and, for a
// host, keep a lobby announced. Requests run on a thread of their own, one at a
// time; the owner calls update() and reads the results. Nothing here knows about
// the game's rules or its connections.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "online/config.hpp"
#include "online/http.hpp"

namespace ab::online {

struct LobbyInfo {
    std::string id;
    std::string name;
    int players = 0, seats = 0;
    std::string phase, level, region;
    int version = 0;
    bool certified = false;
};

// What a host tells the master about its game.
struct LobbyDesc {
    std::string name;
    bool listed = true;  // false: found by its code only
    int players = 0, seats = 10;
    std::string phase = "lobby", level, region;
    std::string identityKey;   // hex of the game server's identity key
    std::string certificate;   // its certificate, if it has one (passed on as it is)
    int port = 0;              // where the game server listens
    std::vector<std::string> candidates;  // further ways to reach it
    bool operator==(const LobbyDesc&) const = default;
};

// What comes back from joining: how to reach the game server and how to know it.
struct Signal {
    std::string from;  // "host", or a player's session id
    std::string data;
};

struct JoinInfo {
    LobbyInfo lobby;
    std::vector<std::string> candidates;
    std::string identityKey;
    std::string certificate;
    std::string relayUser, relayPassword;
    std::vector<std::string> relayUrls;
};

// "dir/online-serial.txt" and a key: "dir/online-serial-0123456789abcdef.txt".
std::string serialFileFor(const std::string& file, const PublicKey& key);

class Online {
public:
    // `serialFile`: where the highest configuration serial seen is kept (may be empty). The
    // file used is one per configuration key (serialFileFor): whoever plays with two
    // communities, each with a configuration of its own, keeps a number for each.
    void configure(const Bootstrap& bootstrap, const std::string& serialFile, const std::string& playerName, int protocolVersion);
    bool configured() const { return bootstrap_.valid(); }

    // Fetches the configuration and opens a session, unless that is done already.
    void connect();
    bool ready() const { return haveConfig_ && !token_.empty(); }
    // Only the configuration, without a session: for what needs the keys in it and not the
    // master (checking a server's certificate). haveConfig() says when it is there.
    void loadConfig();
    bool haveConfig() const { return haveConfig_; }
    bool busy() const { return !queue_.empty(); }
    // Why the last thing asked for did not work; empty if it did. Cleared by the next request.
    const std::string& error() const { return error_; }
    const OnlineConfig& config() const { return config_; }
    const std::string& sessionId() const { return sessionId_; }
    // Starts over: the next connect() fetches the configuration anew.
    void reset();

    // Once per frame: takes up finished requests and starts the next.
    void update();

    // --- players ---
    void refreshLobbies();
    const std::vector<LobbyInfo>& lobbies() const { return lobbies_; }
    int lobbiesTotal() const { return lobbiesTotal_; }
    std::uint64_t lobbiesSerial() const { return lobbiesSerial_; }  // goes up with every list received
    // By its six-digit code. The answer comes through takeFound().
    void findByCode(const std::string& code);
    bool takeFound(LobbyInfo& out);
    // `code` for a game that is not listed. The answer comes through takeJoin().
    void join(const std::string& lobbyId, const std::string& code);
    bool takeJoin(JoinInfo& out);

    // --- hosts ---
    // Announces the game, or updates the announcement; also says "still here". Call it
    // when the description changes and at least every ten seconds.
    void announce(const LobbyDesc& desc);
    bool announced() const { return !lobbyId_.empty(); }
    const std::string& lobbyId() const { return lobbyId_; }
    // Relay credentials for a host (a joining player gets theirs with the join). Asked for
    // again when they are older than half an hour.
    void requestRelay();
    const std::string& relayUser() const { return relayUser_; }
    const std::string& relayPassword() const { return relayPassword_; }

    // --- passing connection details between a host and a joining player ---
    // To "host" (from a player) or to a player's session id (from the host of `lobbyId`).
    void signal(const std::string& lobbyId, const std::string& to, const std::string& data, bool asHost);
    // Waits at the master for what is addressed to us (a request of its own, beside the
    // others: it may take twenty seconds to come back). Call it every frame while listening;
    // what has come is fetched with takeSignals().
    void pollSignals(const std::string& lobbyId, bool asHost);
    std::vector<Signal> takeSignals();
    const std::string& code() const { return code_; }  // the join code, once announced
    void withdraw();

    // For tests: the clock (seconds since 1970) the configuration's dates are checked against.
    void setClock(std::function<long long()> clock) { clock_ = std::move(clock); }

private:
    struct Op {
        std::function<HttpRequest()> make;                 // called when its turn comes
        std::function<void(const HttpResponse&)> done;
        bool needsSession = true;
    };
    bool begin();
    void dropQueue();
    void fetchConfig(std::size_t urlIndex);
    void openSession();
    HttpRequest call(const std::string& method, const std::string& path, const Json* body, bool hostToken = false, int timeoutMs = 10000) const;
    static bool failed(const HttpResponse& r, Json& body, std::string& error);
    void sessionLost();

    Bootstrap bootstrap_;
    std::string serialFile_, name_;
    int version_ = 0;
    std::function<long long()> clock_;
    bool haveConfig_ = false;
    OnlineConfig config_;
    std::string token_, sessionId_;
    std::string error_;
    std::deque<Op> queue_;
    HttpJob job_;
    bool jobRunning_ = false;

    std::vector<LobbyInfo> lobbies_;
    int lobbiesTotal_ = 0;
    std::uint64_t lobbiesSerial_ = 0;
    bool haveFound_ = false, haveJoin_ = false;
    LobbyInfo found_;
    JoinInfo join_;

    std::string lobbyId_, hostToken_, code_;
    std::string relayUser_, relayPassword_;
    long long relayAt_ = 0;
    HttpJob signalJob_;
    bool signalRunning_ = false;
    long long signalRetryAt_ = 0;
    std::vector<Signal> signals_;
    LobbyDesc announcedDesc_;
    bool announcing_ = false;
};

}  // namespace ab::online
