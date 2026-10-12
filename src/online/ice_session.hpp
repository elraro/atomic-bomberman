// The two ends of finding a way to a game through routers
// (docs/specifications/online-play.md, "Connectivity"): a joining player offers
// its description to the host by way of the master server, the host answers
// with its own, the ICE agents on both sides try the addresses, and what comes
// out is a carrier for the game's connection. Header only: it needs the master
// client, the ICE wrapper and the game server together.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "net/server.hpp"
#include "online/ice.hpp"
#include "online/json.hpp"
#include "online/online.hpp"

namespace ab::online {

// A host that can be reached this way says so among the ways to reach it.
inline constexpr const char* kIceCandidate = "ice";

// The STUN server and the relays (those reached over UDP) the configuration names, with
// the credentials the master gave for the relay.
inline ice::Config iceConfig(const OnlineConfig& config, const std::string& relayUser, const std::string& relayPassword, bool relayOnly) {
    ice::Config c;
    c.relayOnly = relayOnly;
    if (!config.stun.empty()) {
        const std::string& first = config.stun.front();
        const std::size_t colon = first.rfind(':');
        c.stunHost = first.substr(0, colon);
        if (colon != std::string::npos) c.stunPort = std::atoi(first.c_str() + colon + 1);
    }
    if (!relayUser.empty())
        for (const TurnServer& t : config.turn)
            if (t.transport == "udp") c.turn.push_back({t.host, t.port, relayUser, relayPassword});
    return c;
}

inline std::string iceSignal(const char* type, const std::string& description) {
    Json j = Json::object();
    j.set("type", type).set("sdp", description);
    return j.dump();
}

// The host's side: answers every offer, and hands the server each way that comes about.
class IceHost {
public:
    void update(std::uint64_t nowMs, Online& online, net::Server& server, bool relayOnly) {
        if (!online.announced()) return;
        online.requestRelay();
        online.pollSignals(online.lobbyId(), true);
        for (const Signal& s : online.takeSignals()) {
            Json j;
            if (!Json::parse(s.data, j) || j["type"].str() != "offer" || s.from == "host") continue;  // "join" and "leave" are plain words
            if (pending_.size() >= 16) continue;
            Pending p;
            p.agent = std::make_unique<ice::Agent>();
            p.since = nowMs;
            if (!p.agent->start(iceConfig(online.config(), online.relayUser(), online.relayPassword(), relayOnly)) || !p.agent->setRemoteDescription(j["sdp"].str())) continue;
            pending_[s.from] = std::move(p);  // a second offer from the same player replaces the first
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            Pending& p = it->second;
            if (!p.answered && p.agent->gathered()) {
                online.signal(online.lobbyId(), it->first, iceSignal("answer", p.agent->localDescription()), true);
                p.answered = true;
            }
            if (p.agent->state() == ice::Agent::State::Connected) {
                server.adopt(std::move(p.agent));
                it = pending_.erase(it);
            } else if (p.agent->state() == ice::Agent::State::Failed || nowMs - p.since > 30000) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    void reset() { pending_.clear(); }

private:
    struct Pending {
        std::unique_ptr<ice::Agent> agent;
        std::uint64_t since = 0;
        bool answered = false;
    };
    std::map<std::string, Pending> pending_;
};

// The joining player's side.
class IceJoin {
public:
    enum class State { Idle, Working, Ready, Failed };

    void start(Online& online, const JoinInfo& info, bool relayOnly, std::uint64_t nowMs) {
        lobbyId_ = info.lobby.id;
        agent_ = std::make_unique<ice::Agent>();
        since_ = nowMs;
        offered_ = answered_ = false;
        state_ = agent_->start(iceConfig(online.config(), info.relayUser, info.relayPassword, relayOnly)) ? State::Working : State::Failed;
    }
    void cancel() {
        agent_.reset();
        state_ = State::Idle;
    }
    State state() const { return state_; }
    // What is being done, for the screen.
    std::string step() const { return !offered_ ? "Looking for ways to the host..." : !answered_ ? "Waiting for the host's answer..." : "Trying the ways to the host..."; }

    void update(std::uint64_t nowMs, Online& online) {
        if (state_ != State::Working) return;
        if (!offered_ && agent_->gathered()) {
            online.signal(lobbyId_, "host", iceSignal("offer", agent_->localDescription()), false);
            offered_ = true;
        }
        if (offered_ && !answered_) {
            online.pollSignals(lobbyId_, false);
            for (const Signal& s : online.takeSignals()) {
                Json j;
                if (s.from != "host" || !Json::parse(s.data, j) || j["type"].str() != "answer") continue;
                answered_ = agent_->setRemoteDescription(j["sdp"].str());
            }
        }
        if (agent_->state() == ice::Agent::State::Connected) state_ = State::Ready;
        else if (agent_->state() == ice::Agent::State::Failed || nowMs - since_ > 25000) state_ = State::Failed;
    }
    // The way found, once Ready.
    std::unique_ptr<net::Carrier> take() {
        state_ = State::Idle;
        return std::move(agent_);
    }

private:
    std::unique_ptr<ice::Agent> agent_;
    std::string lobbyId_;
    std::uint64_t since_ = 0;
    bool offered_ = false, answered_ = false;
    State state_ = State::Idle;
};

}  // namespace ab::online
