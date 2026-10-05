// The game server of the network mode (docs/specifications/networking.md).
// No window, no SDL: it is the whole of the dedicated server and also runs
// inside the game when a player hosts. Nothing blocks; the owner calls update().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/ai.hpp"
#include "game/match.hpp"
#include "net/protocol.hpp"
#include "net/socket.hpp"
#include "net/upnp.hpp"
#include "resources/campaign_file.hpp"
#include "resources/scheme_file.hpp"

namespace ab::net {

struct ServerConfig {
    std::string name = "Atomic Bomberman";
    std::uint16_t port = kDefaultPort;  // 0: any free port (see Server::port())
    std::string password;               // empty: open to everyone
    std::string gameDir;                // game data; empty: built-in arena and default values
    std::string userSchemesDir;         // schemes made with the editor (may be empty)
    std::string scheme = "basic";       // file name of the first scheme
    MatchSettings settings;             // what the lobby starts with
    std::uint32_t seed = 0;             // 0: taken from the clock
    bool discoverable = true;           // answer LAN queries
    bool upnp = false;                  // ask the router to forward the port (UPnP)
    std::function<void(const std::string&)> log;  // one line per event; may be empty
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const ServerConfig& config);
    void stop();
    bool running() const { return running_; }
    std::uint16_t port() const { return listener_.port(); }
    const std::string& error() const { return error_; }

    // Network and game logic up to the given time (ms of a monotonic clock).
    void update(std::uint64_t nowMs);

    Phase phase() const { return phase_; }
    int players() const;  // clients that have joined
    const LobbyState& lobby() const { return lobby_; }
    const World* world() const { return world_.get(); }
    std::uint32_t roundId() const { return roundId_; }
    std::uint32_t steps() const { return static_cast<std::uint32_t>(history_.size()); }
    const MatchScore& score() const { return score_; }
    // A line in everyone's chat, from the server.
    void say(const std::string& text);

private:
    struct Peer;

    void log(const std::string& line) const;
    void handleFrame(Peer& p, std::uint8_t type, const std::vector<std::uint8_t>& payload);
    void handleHello(Peer& p, const std::vector<std::uint8_t>& payload);
    void handleDatagram(const Address& from, const std::vector<std::uint8_t>& data, bool viaDiscovery);
    void applyInput(Peer& p, const InputMsg& m);
    void changeOption(Option option, int direction);
    void reject(Peer& p, const std::string& reason);
    void tell(Peer& p, const std::string& text);
    void leave(Peer& p);
    Peer* peerById(std::uint8_t id);
    Peer* peerAtSeat(int seat, int* local = nullptr);
    Peer* admin();

    void loadScheme();
    void rebuildSeats();
    void refreshLobby();
    void broadcast(ServerMsg type, const std::vector<std::uint8_t>& payload);
    std::string startMatch();  // empty, or why not
    void startRound();
    void stepRound();
    void sendSteps(Peer& p);
    void sendSnapshot(Peer& p);
    void endRound();
    void toLobby();

    ServerConfig config_;
    bool running_ = false;
    std::string error_;
    TcpListener listener_;
    UdpSocket udp_;
    UdpSocket discovery_;  // answers LAN searches on kDiscoveryPort
    PortMapper mapper_;
    bool mapperReported_ = false;
    std::string routerNotice_;  // told to everyone who joins
    std::vector<std::unique_ptr<Peer>> peers_;
    std::uint64_t now_ = 0;
    std::uint64_t joinCounter_ = 0;
    Rng rng_{1};

    // Game data.
    Values values_ = Values::defaults();
    std::vector<SchemeEntry> schemes_;
    SchemeFile schemeFile_;

    // Lobby.
    Phase phase_ = Phase::Lobby;
    MatchSettings settings_;
    std::array<int, kMaxPlayers> teams_{};
    std::array<SeatKind, kMaxPlayers> seatKind_{};
    std::array<SeatKind, kMaxPlayers> roundKind_{};  // who sits where in the round being played (a campaign stage seats its own computer players)
    std::vector<std::pair<std::string, std::string>> campaigns_;  // name, path
    std::vector<CampaignStage> stages_;
    int stageIndex_ = 0;
    int campaignNext_ = 0;
    bool campaignMode_ = false;
    int lastWinner_ = -1;   // winner of the last match (player or team), for the roulette
    int prizeType_ = -1;
    int prizeWinner_ = -1;
    LobbyState lobby_;
    bool lobbyDirty_ = true;
    std::uint64_t lobbySentAt_ = 0;
    std::uint64_t pingSentAt_ = 0;

    // Match and round.
    MatchScore score_;
    int level_ = 0;
    std::array<Cell, kMaxPlayers> startCells_{};
    std::vector<AiPlayer> ai_;
    std::unique_ptr<World> world_;
    std::uint32_t roundId_ = 0;
    std::vector<std::uint8_t> roundStart_;  // the encoded RoundStart of the current round
    std::vector<std::uint8_t> roundEnd_;    // and its RoundEnd, once decided
    std::vector<StepInputs> history_;
    std::vector<std::uint32_t> hashes_;
    std::uint64_t roundStartAt_ = 0;
    bool decided_ = false;
    int winner_ = -1;
    int afterSteps_ = 0;
    std::uint64_t resultAt_ = 0;
    std::uint64_t resendAt_ = 0;
};

}  // namespace ab::net
