// The client side of the network mode (docs/specifications/networking.md):
// connection, lobby state, chat and the round, which it runs in its own World
// from the steps the server sends. No SDL; the front end feeds it input and
// draws what it holds.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/protocol.hpp"
#include "net/socket.hpp"

namespace ab::net {

struct ChatLine {
    bool fromServer = false;
    std::string name;
    std::string text;
    std::uint64_t atMs = 0;
};

class Client {
public:
    enum class State {
        Idle,
        Connecting,  // TCP connection and Hello under way
        Lobby,       // also while a match the client is not shown yet is running
        Round,
        Result,
        Failed,      // error() says why
    };

    // `address` is "host" or "host:port". The name lookup may block briefly.
    void connect(const std::string& address, const std::string& name, const std::string& password, std::uint64_t nowMs);
    void disconnect();
    // Where the identities of servers visited are remembered ("address fingerprint" lines).
    // A server whose identity differs from the remembered one is refused; forgetServer()
    // drops the memory of an address so that the next visit is a first one.
    void setKnownServersFile(const std::string& path) { knownFile_ = path; }
    void forgetServer(const std::string& address);
    bool identityChanged() const { return identityChanged_; }  // why the last connection failed
    const std::string& serverIdentity() const { return identity_; }   // fingerprint of the server in use
    bool serverWasKnown() const { return identityKnown_; }
    void checkIdentityOnLoopback() { identityOnLoopback_ = true; }  // for the tests
    // Network traffic. Does not advance the round.
    void update(std::uint64_t nowMs);
    // What happened in a step, for the sounds. With prediction on, this player's own actions
    // (dropping, punching, picking up...) are reported from the predicted step, at once, and
    // left out when the server confirms them.
    using EventSink = std::function<void(const World&, const std::vector<Event>&)>;
    // Applies the steps that are due. `before` is called before the picture changes (to
    // remember positions for drawing), `events` with each step's events.
    int advance(std::uint64_t nowMs, const std::function<void(World&)>& before, const EventSink& events);
    // How far the present time is between the last step and the next, 0-1 (for drawing).
    float stepAlpha(std::uint64_t nowMs) const;

    State state() const { return state_; }
    const std::string& error() const { return error_; }
    const std::string& serverName() const { return serverName_; }
    std::uint8_t id() const { return id_; }
    bool isAdmin() const { return lobby_.admin == id_; }
    int seat() const;  // the seat of this computer's first player, -1 while watching
    // The seats of this computer's players (-1: that player has none).
    std::array<int, kMaxLocalPlayers> seats() const;
    int localPlayers() const;  // how many players this computer has asked for
    const LobbyState& lobby() const { return lobby_; }
    const std::deque<ChatLine>& chat() const { return chat_; }
    bool udpActive() const { return udpOn_; }
    bool encrypted() const { return keyed_ && socket_.encrypted(); }

    void setInput(const PlayerInput& in) { input_[0] = packInput(in); }
    void setInput(int local, const PlayerInput& in) { input_[static_cast<std::size_t>(local)] = packInput(in); }
    void sendChat(const std::string& text);
    void sendOption(Option option, int direction);
    void sendStart();
    void sendTeam(int local = 0);
    void sendLocalPlayers(int count);  // 1-4 players at this computer (in the lobby)
    void sendKick(std::uint8_t id);
    void sendBan(std::uint8_t id);               // administrator: keep that player's address out for good
    void sendUnban(const std::string& address);  // administrator: lift a ban
    void sendAdmin(std::uint8_t id);             // administrator: hand the role to that player
    void sendLogin(const std::string& password); // become administrator with the server's administrator password
    void sendContinue();

    // Client-side prediction (on by default): the picture runs a few steps ahead of what the
    // server has confirmed, with this player's own keys applied at once, so moving does not
    // wait for the round trip. See view().
    void setPrediction(bool on) { prediction_ = on; }
    bool prediction() const { return prediction_; }
    // How many steps ahead to predict; 0 (the default) works it out from the measured ping.
    void setPredictionSteps(int steps) { predictionSteps_ = steps; }
    // What to draw: the predicted state while this player is in a running round with
    // prediction on, otherwise the confirmed state. Never used for results or scores.
    const World* view() const { return predicted_ && predictedValid_ ? predicted_.get() : world_.get(); }
    std::uint32_t viewStep() const { return predicted_ && predictedValid_ ? target_ : applied_; }

    // The round as confirmed by the server (null in the lobby).
    World* world() { return world_.get(); }
    const World* world() const { return world_.get(); }
    const RoundSetup& setup() const { return start_.setup; }
    std::uint32_t roundId() const { return start_.roundId; }
    int winsNeeded() const { return start_.winsNeeded; }
    // Scores: those before the round while it runs, those after it on the result screen.
    const MatchScore& score() const { return state_ == State::Result ? end_.score : start_.score; }
    const RoundEndMsg& result() const { return end_; }
    const RoundStartMsg& roundStart() const { return start_; }
    // The bonus wheel being shown before a match, or null. `rouletteSince()` is when it began.
    const RouletteMsg* roulette() const { return rouletteOn_ ? &roulette_ : nullptr; }
    std::uint64_t rouletteSince() const { return rouletteAt_; }
    std::uint32_t stepsApplied() const { return applied_; }
    int snapshotsLoaded() const { return snapshots_; }

    // Test hooks: no UDP at all; and access to break the state on purpose.
    void disableUdp() { udpAllowed_ = false; }

private:
    // One step as the server sent it: everybody's input and that step's secrets.
    struct Step {
        StepInputs inputs{};
        World::Secrets secrets;
    };
    void fail(const std::string& why);
    void handleFrame(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t nowMs);
    void handleSteps(const StepsMsg& m);
    void beginRound(const RoundStartMsg& m, std::uint64_t nowMs);
    void sendInput(std::uint64_t nowMs);
    void applyStep(const Step& step, const EventSink& events);
    void predict(const EventSink& events);
    bool ownAction(const Event& e) const;

    State state_ = State::Idle;
    std::string error_;
    TcpSocket socket_;
    UdpSocket udp_;
    Address server_{};
    std::string name_;
    std::string address_;      // as given to connect()
    std::string knownFile_;
    std::string identity_;
    bool identityKnown_ = false;
    bool identityChanged_ = false;
    bool identityOnLoopback_ = false;
    std::string password_;
    std::uint64_t connectAt_ = 0;
    bool helloSent_ = false;   // the key exchange was started
    bool keyed_ = false;       // both sides hold the connection's keys
    Key secret_{}, public_{};
    SessionKeys keys_;
    std::uint64_t udpSent_ = 0, udpReceived_ = 0;  // datagram counters, one per direction
    bool welcomed_ = false;
    std::uint8_t id_ = 0;
    std::uint32_t token_ = 0;
    std::string serverName_;
    LobbyState lobby_;
    std::deque<ChatLine> chat_;

    bool udpAllowed_ = true;
    bool udpOn_ = false;
    int probes_ = 0;
    std::uint64_t probeAt_ = 0;
    std::uint64_t udpStepsAt_ = 0;   // when steps last arrived by UDP

    RoundStartMsg start_;
    RoundEndMsg end_;
    RouletteMsg roulette_;
    bool rouletteOn_ = false;
    std::uint64_t rouletteAt_ = 0;
    bool ended_ = false;             // RoundEnd received for this round
    std::unique_ptr<World> world_;
    std::deque<Step> queue_;         // received, not yet applied
    bool ready_ = false;             // the round's starting state has arrived
    std::uint32_t applied_ = 0;
    std::map<std::uint32_t, std::uint32_t> hashes_;  // steps applied -> the server's hash
    bool awaitingState_ = false;
    int snapshots_ = 0;
    std::uint64_t nextStepAt_ = 0;
    std::uint64_t lastStepAt_ = 0;
    std::array<std::uint8_t, kMaxLocalPlayers> input_{};
    bool prediction_ = true;
    int predictionSteps_ = 0;
    std::unique_ptr<World> predicted_;
    bool predictedValid_ = false;
    std::uint32_t target_ = 0;                        // the step the predicted state stands for
    std::map<std::uint32_t, std::array<std::uint8_t, kMaxLocalPlayers>> mine_;      // own input assumed for each step not yet confirmed
    StepInputs lastInputs_{};                         // everyone's input in the last confirmed step
    std::uint64_t nextLocalAt_ = 0;
    std::uint64_t now_ = 0;
    std::uint32_t soundedUpTo_ = 0;                   // predicted steps whose own-action sounds were given out
    struct Heard {
        EventKind kind;
        int player;
        std::uint64_t atMs;
    };
    std::vector<Heard> heard_;                        // own actions already reported from a prediction
    std::array<std::uint8_t, kMaxLocalPlayers> inputSent_{};
    std::uint64_t inputSentAt_ = 0;
};

// Finds servers on the local network, whatever port they play on (UDP broadcast to the discovery port).
class LanBrowser {
public:
    struct Entry {
        Address address;   // where to connect (TCP port from the answer)
        ServerInfo info;
        std::uint64_t seenAt = 0;
    };
    void update(std::uint64_t nowMs);  // asks every two seconds, collects answers
    void stop();
    const std::vector<Entry>& servers() const { return servers_; }

private:
    UdpSocket udp_;
    std::uint64_t askedAt_ = 0;
    bool asked_ = false;
    std::vector<Entry> servers_;
};

}  // namespace ab::net
