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
    // Network traffic. Does not advance the round.
    void update(std::uint64_t nowMs);
    // Applies the steps that are due. `before` and `after` are called around each one
    // (to remember positions for drawing, and to play the step's sounds).
    int advance(std::uint64_t nowMs, const std::function<void(World&)>& before, const std::function<void(World&)>& after);
    // How far the present time is between the last step and the next, 0-1 (for drawing).
    float stepAlpha(std::uint64_t nowMs) const;

    State state() const { return state_; }
    const std::string& error() const { return error_; }
    const std::string& serverName() const { return serverName_; }
    std::uint8_t id() const { return id_; }
    bool isAdmin() const { return lobby_.admin == id_; }
    int seat() const;  // own seat, -1 while watching
    const LobbyState& lobby() const { return lobby_; }
    const std::deque<ChatLine>& chat() const { return chat_; }
    bool udpActive() const { return udpOn_; }

    void setInput(const PlayerInput& in) { input_ = packInput(in); }
    void sendChat(const std::string& text);
    void sendOption(Option option, int direction);
    void sendStart();
    void sendTeam();
    void sendKick(std::uint8_t id);
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
    std::uint32_t stepsApplied() const { return applied_; }
    int snapshotsLoaded() const { return snapshots_; }

    // Test hooks: no UDP at all; and access to break the state on purpose.
    void disableUdp() { udpAllowed_ = false; }

private:
    void fail(const std::string& why);
    void handleFrame(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t nowMs);
    void handleSteps(const StepsMsg& m);
    void beginRound(const RoundStartMsg& m, std::uint64_t nowMs);
    void sendInput(std::uint64_t nowMs);
    void applyStep(const StepInputs& bytes, const std::function<void(World&)>& after);
    void predict();

    State state_ = State::Idle;
    std::string error_;
    TcpSocket socket_;
    UdpSocket udp_;
    Address server_{};
    std::string name_;
    std::string password_;
    std::uint64_t connectAt_ = 0;
    bool helloSent_ = false;
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
    bool ended_ = false;             // RoundEnd received for this round
    std::unique_ptr<World> world_;
    std::deque<StepInputs> queue_;   // received, not yet applied
    std::uint32_t applied_ = 0;
    std::map<std::uint32_t, std::uint32_t> hashes_;  // steps applied -> the server's hash
    bool awaitingState_ = false;
    int snapshots_ = 0;
    std::uint64_t nextStepAt_ = 0;
    std::uint64_t lastStepAt_ = 0;
    std::uint8_t input_ = 0;
    bool prediction_ = true;
    int predictionSteps_ = 0;
    std::unique_ptr<World> predicted_;
    bool predictedValid_ = false;
    std::uint32_t target_ = 0;                        // the step the predicted state stands for
    std::map<std::uint32_t, std::uint8_t> mine_;      // own input assumed for each step not yet confirmed
    StepInputs lastInputs_{};                         // everyone's input in the last confirmed step
    std::uint64_t nextLocalAt_ = 0;
    std::uint8_t inputSent_ = 0;
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
