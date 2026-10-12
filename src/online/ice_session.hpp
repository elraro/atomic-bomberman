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

// Which of the relays the configuration names are used: those reached over UDP (the normal
// case), or those reached over TCP or TLS, for a network that lets no UDP out or stops
// carrying it after a while (TLS alone: where only that passes, and for tests).
enum class Ways { Udp, Stream, Tls };

// The STUN server and the relays, with the credentials the master gave for the relay. Over a
// stream nothing but relayed ways are used, on both sides: the other side's relayed address
// is what is reached, so that nothing goes around the stream.
inline ice::Config iceConfig(const OnlineConfig& config, const std::string& relayUser, const std::string& relayPassword, bool relayOnly, Ways ways = Ways::Udp) {
    ice::Config c;
    c.relayOnly = relayOnly || ways != Ways::Udp;
    if (!config.stun.empty() && ways == Ways::Udp) {
        const std::string& first = config.stun.front();
        const std::size_t colon = first.rfind(':');
        c.stunHost = first.substr(0, colon);
        if (colon != std::string::npos) c.stunPort = std::atoi(first.c_str() + colon + 1);
    }
    if (!relayUser.empty())
        for (const TurnServer& t : config.turn) {
            const bool wanted = ways == Ways::Udp ? t.transport == "udp" : ways == Ways::Tls ? t.transport == "tls" : t.transport != "udp";
            if (wanted && c.turn.size() < 2) c.turn.push_back({t.host, t.port, relayUser, relayPassword, t.transport});
        }
    return c;
}

// The configuration names a relay that can be reached over TCP or TLS.
inline bool hasStreamRelay(const OnlineConfig& config) {
    for (const TurnServer& t : config.turn)
        if (t.transport != "udp") return true;
    return false;
}

// (`attempt`: which try of the joining player's this belongs to; the answer says it back.)
inline std::string iceSignal(const char* type, const std::string& description, int attempt = 0) {
    Json j = Json::object();
    j.set("type", type).set("sdp", description).set("try", static_cast<long long>(attempt));
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
            p.attempt = static_cast<int>(j["try"].integer());
            if (!p.agent->start(iceConfig(online.config(), online.relayUser(), online.relayPassword(), relayOnly)) || !p.agent->setRemoteDescription(j["sdp"].str())) continue;
            pending_[s.from] = std::move(p);  // a second offer from the same player replaces the first
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            Pending& p = it->second;
            if (!p.answered && p.agent->gathered()) {
                online.signal(online.lobbyId(), it->first, iceSignal("answer", p.agent->localDescription(), p.attempt), true);
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
        int attempt = 0;
        bool answered = false;
    };
    std::map<std::string, Pending> pending_;
};

// The joining player's side. A first try over UDP (straight, or by the relay); where that
// finds nothing, or the network lets no UDP out at all, a second by the relay over TCP or
// TLS, if the configuration names one.
class IceJoin {
public:
    enum class State { Idle, Working, Ready, Failed };

    // `first`: other than Udp, the try over UDP is left out.
    void start(Online& online, const JoinInfo& info, bool relayOnly, std::uint64_t nowMs, Ways first = Ways::Udp) {
        lobbyId_ = info.lobby.id;
        relayUser_ = info.relayUser;
        relayPassword_ = info.relayPassword;
        relayOnly_ = relayOnly;
        attempt_ = 0;
        begin(online, nowMs, first);
    }
    void cancel() {
        agent_.reset();
        state_ = State::Idle;
    }
    State state() const { return state_; }
    bool overStream() const { return ways_ != Ways::Udp; }
    // What is being done, for the screen.
    std::string step() const {
        const std::string by = overStream() ? " by the relay over TCP" : "";
        return !offered_ ? "Looking for ways to the host" + by + "..." : !answered_ ? "Waiting for the host's answer..." : "Trying the ways to the host" + by + "...";
    }

    void update(std::uint64_t nowMs, Online& online) {
        if (state_ != State::Working) return;
        if (!offered_ && agent_->gathered()) {
            // Neither the STUN server nor a relay answered: no UDP leaves this network.
            if (!overStream() && asksOutside_ && !agent_->reachedOutside() && again(online, nowMs)) return;
            online.signal(lobbyId_, "host", iceSignal("offer", agent_->localDescription(), attempt_), false);
            offered_ = true;
        }
        if (offered_ && !answered_) {
            online.pollSignals(lobbyId_, false);
            for (const Signal& s : online.takeSignals()) {
                Json j;
                if (s.from != "host" || !Json::parse(s.data, j) || j["type"].str() != "answer" || j["try"].integer() != attempt_) continue;
                answered_ = agent_->setRemoteDescription(j["sdp"].str());
            }
        }
        if (agent_->state() == ice::Agent::State::Connected) state_ = State::Ready;
        else if ((agent_->state() == ice::Agent::State::Failed || nowMs - since_ > 25000) && !again(online, nowMs)) state_ = State::Failed;
    }
    // The way found, once Ready.
    std::unique_ptr<net::Carrier> take() {
        state_ = State::Idle;
        return std::move(agent_);
    }
    // The way found turned out not to carry (the volume check): once more, by the relay over
    // TCP or TLS. False: that has been tried already, or there is no such relay.
    bool again(Online& online, std::uint64_t nowMs) {
        if (overStream() || relayUser_.empty() || !hasStreamRelay(online.config())) return false;
        begin(online, nowMs, Ways::Stream);
        return state_ == State::Working;
    }

private:
    void begin(Online& online, std::uint64_t nowMs, Ways ways) {
        ways_ = ways;
        ++attempt_;
        agent_ = std::make_unique<ice::Agent>();
        since_ = nowMs;
        offered_ = answered_ = false;
        const ice::Config config = iceConfig(online.config(), relayUser_, relayPassword_, relayOnly_, ways_);
        asksOutside_ = !config.stunHost.empty() || !config.turn.empty();
        state_ = agent_->start(config) ? State::Working : State::Failed;
    }

    std::unique_ptr<ice::Agent> agent_;
    std::string lobbyId_, relayUser_, relayPassword_;
    std::uint64_t since_ = 0;
    int attempt_ = 0;
    Ways ways_ = Ways::Udp;
    bool relayOnly_ = false, asksOutside_ = false;
    bool offered_ = false, answered_ = false;
    State state_ = State::Idle;
};

}  // namespace ab::online
