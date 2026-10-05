#include "net/client.hpp"

#include <algorithm>
#include <exception>

namespace ab::net {

namespace {
constexpr std::uint64_t kConnectTimeoutMs = 8000;
constexpr std::size_t kChatKept = 100;
constexpr int kMaxProbes = 12;
}  // namespace

void Client::fail(const std::string& why) {
    socket_.close();
    udp_.close();
    world_.reset();
    predicted_.reset();
    predictedValid_ = false;
    queue_.clear();
    udpOn_ = false;
    error_ = why;
    state_ = State::Failed;
}

void Client::disconnect() {
    socket_.close();
    udp_.close();
    world_.reset();
    predicted_.reset();
    predictedValid_ = false;
    queue_.clear();
    chat_.clear();
    lobby_ = LobbyState{};
    udpOn_ = false;
    error_.clear();
    state_ = State::Idle;
}

void Client::connect(const std::string& address, const std::string& name, const std::string& password, std::uint64_t nowMs) {
    disconnect();
    name_ = name;
    password_ = password;
    connectAt_ = nowMs;
    helloSent_ = false;
    welcomed_ = false;
    probes_ = 0;
    probeAt_ = 0;
    const auto to = resolveHostPort(address, kDefaultPort);
    if (!to) {
        fail("Cannot find \"" + address + "\"");
        return;
    }
    server_ = *to;
    if (!socket_.connect(server_)) {
        fail("Cannot connect to " + server_.text());
        return;
    }
    state_ = State::Connecting;
}

int Client::seat() const {
    const ClientInfo* me = lobby_.client(id_);
    return me != nullptr ? me->seat : -1;
}

void Client::sendChat(const std::string& text) {
    const std::string clean = cleanText(text, kMaxChat);
    if (clean.empty() || !welcomed_) return;
    ByteWriter w;
    w.text(clean);
    socket_.send(static_cast<std::uint8_t>(ClientMsg::Chat), w.data());
}

void Client::sendOption(Option option, int direction) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(option));
    w.i8(direction);
    socket_.send(static_cast<std::uint8_t>(ClientMsg::Option), w.data());
}

void Client::sendStart() { socket_.send(static_cast<std::uint8_t>(ClientMsg::Start), {}); }
void Client::sendTeam() { socket_.send(static_cast<std::uint8_t>(ClientMsg::Team), {}); }
void Client::sendContinue() { socket_.send(static_cast<std::uint8_t>(ClientMsg::Continue), {}); }

void Client::sendKick(std::uint8_t id) {
    ByteWriter w;
    w.u8(id);
    socket_.send(static_cast<std::uint8_t>(ClientMsg::Kick), w.data());
}

void Client::beginRound(const RoundStartMsg& m, std::uint64_t nowMs) {
    start_ = m;
    end_ = RoundEndMsg{};
    ended_ = false;
    queue_.clear();
    hashes_.clear();
    applied_ = 0;
    awaitingState_ = false;
    nextStepAt_ = nowMs;
    lastStepAt_ = nowMs;
    udpStepsAt_ = nowMs;
    inputSentAt_ = 0;
    predicted_.reset();
    predictedValid_ = false;
    target_ = 0;
    soundedUpTo_ = 0;
    heard_.clear();
    mine_.clear();
    lastInputs_ = {};
    nextLocalAt_ = nowMs;
    // The server's tuning table on top of the built-in one: the rules are the server's.
    Values values = Values::defaults();
    for (const auto& [id, value] : m.values) values.set(id, value);
    try {
        world_ = std::make_unique<World>(values, m.seed);
        applyRoundSetup(*world_, values, m.setup);
    } catch (const std::exception& e) {
        fail(std::string("The server sent a round this game cannot play (") + e.what() + ")");
        return;
    }
    state_ = State::Round;
}

void Client::handleSteps(const StepsMsg& m) {
    if (!world_ || m.roundId != start_.roundId || m.steps.empty()) return;
    const std::uint32_t have = applied_ + static_cast<std::uint32_t>(queue_.size());
    const std::uint32_t last = m.firstStep + static_cast<std::uint32_t>(m.steps.size());
    hashes_[last] = m.hash;
    if (m.firstStep > have || last <= have) return;  // a gap (the next message covers it) or nothing new
    for (std::uint32_t i = have - m.firstStep; i < m.steps.size(); ++i) queue_.push_back(m.steps[i]);
}

void Client::handleFrame(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t nowMs) {
    ByteReader r(payload);
    switch (static_cast<ServerMsg>(type)) {
        case ServerMsg::Welcome: {
            WelcomeMsg m;
            if (!decode(r, m)) return fail("Bad answer from the server");
            id_ = m.id;
            token_ = m.token;
            serverName_ = m.serverName;
            welcomed_ = true;
            state_ = State::Lobby;
            if (udpAllowed_) udp_.open(0, false, false, server_.v6);
            break;
        }
        case ServerMsg::Reject: fail(r.text(256)); break;
        case ServerMsg::Lobby: {
            LobbyState m;
            if (!decode(r, m)) break;
            lobby_ = std::move(m);
            if (lobby_.phase == Phase::Lobby && (state_ == State::Round || state_ == State::Result)) {
                world_.reset();
                predicted_.reset();
                predictedValid_ = false;
                queue_.clear();
                state_ = State::Lobby;
            }
            break;
        }
        case ServerMsg::Chat: {
            ChatMsg m;
            if (!decode(r, m)) break;
            chat_.push_back({m.sender == kServerSender, cleanText(m.name, 64), cleanText(m.text, 512), nowMs});
            while (chat_.size() > kChatKept) chat_.pop_front();
            break;
        }
        case ServerMsg::RoundStart: {
            RoundStartMsg m;
            if (!decode(r, m)) return fail("The server sent a round this game cannot read");
            beginRound(m, nowMs);
            break;
        }
        case ServerMsg::Steps: {
            StepsMsg m;
            if (decode(r, m)) handleSteps(m);
            break;
        }
        case ServerMsg::RoundEnd: {
            RoundEndMsg m;
            if (!decode(r, m) || m.roundId != start_.roundId) break;
            end_ = m;
            ended_ = true;
            break;
        }
        case ServerMsg::Snapshot: {
            SnapshotMsg m;
            if (!decode(r, m) || !world_ || m.roundId != start_.roundId) break;
            if (!world_->loadState(m.state)) return fail("The server sent a game state this game cannot read");
            queue_.clear();
            hashes_.clear();
            applied_ = m.step;
            soundedUpTo_ = std::max(soundedUpTo_, m.step);
            mine_.clear();
            awaitingState_ = false;
            ++snapshots_;
            break;
        }
        case ServerMsg::Ping: socket_.send(static_cast<std::uint8_t>(ClientMsg::Pong), payload); break;
        default: break;
    }
}

void Client::sendInput(std::uint64_t nowMs) {
    if (input_ == inputSent_ && nowMs - inputSentAt_ < static_cast<std::uint64_t>(kStepMs) && inputSentAt_ != 0) return;
    InputMsg m;
    m.token = token_;
    m.roundId = start_.roundId;
    m.haveStep = applied_ + static_cast<std::uint32_t>(queue_.size());
    m.input = input_;
    if (udpOn_) udp_.sendTo(server_, datagram(UdpMsg::Input, m));
    else socket_.send(static_cast<std::uint8_t>(ClientMsg::Input), encoded(m));
    inputSent_ = input_;
    inputSentAt_ = nowMs;
}

void Client::update(std::uint64_t nowMs) {
    if (state_ == State::Idle || state_ == State::Failed) return;

    const bool alive = socket_.pump();
    if (socket_.connected() && !helloSent_) {
        HelloMsg hello;
        hello.name = name_;
        hello.password = password_;
        socket_.send(static_cast<std::uint8_t>(ClientMsg::Hello), encoded(hello));
        helloSent_ = true;
    }
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
    while (state_ != State::Failed && state_ != State::Idle && socket_.receive(type, payload)) handleFrame(type, payload, nowMs);
    if (state_ == State::Failed || state_ == State::Idle) return;
    if (!alive) return fail(welcomed_ ? "The connection to the server was lost" : "Cannot connect to " + server_.text());
    if (!welcomed_ && nowMs - connectAt_ > kConnectTimeoutMs) return fail("No answer from " + server_.text());

    if (welcomed_ && udp_.isOpen()) {
        // Until the server answers a probe, everything goes over TCP.
        if (!udpOn_ && probes_ < kMaxProbes && nowMs - probeAt_ >= 250) {
            ByteWriter w;
            w.u32(kUdpMagic);
            w.u8(static_cast<std::uint8_t>(UdpMsg::Probe));
            w.u32(token_);
            udp_.sendTo(server_, w.data());
            probeAt_ = nowMs;
            ++probes_;
        }
        Address from;
        std::vector<std::uint8_t> data;
        for (int n = 0; n < 256 && udp_.receiveFrom(from, data); ++n) {
            if (!(from == server_)) continue;
            ByteReader r(data);
            if (r.u32() != kUdpMagic) continue;
            const auto kind = static_cast<UdpMsg>(r.u8());
            if (kind == UdpMsg::ProbeAck && !udpOn_ && probes_ <= kMaxProbes) {
                udpOn_ = true;
                udpStepsAt_ = nowMs;
                ByteWriter w;
                w.flag(true);
                socket_.send(static_cast<std::uint8_t>(ClientMsg::UdpState), w.data());
            } else if (kind == UdpMsg::Steps) {
                StepsMsg m;
                if (decode(r, m)) {
                    handleSteps(m);
                    udpStepsAt_ = nowMs;
                }
            }
        }
        // Steps stopped coming by UDP in the middle of a round: back to TCP for good.
        if (udpOn_ && state_ == State::Round && !ended_ && nowMs - udpStepsAt_ > 3000) {
            udpOn_ = false;
            probes_ = kMaxProbes + 1;
            ByteWriter w;
            w.flag(false);
            socket_.send(static_cast<std::uint8_t>(ClientMsg::UdpState), w.data());
        }
    }

    if (state_ == State::Round) sendInput(nowMs);
    socket_.pump();
}

// Things a player does with their own hands: worth hearing without the round trip.
bool Client::ownAction(const Event& e) const {
    if (e.player < 0 || e.player != seat()) return false;
    switch (e.kind) {
        case EventKind::BombDropped:
        case EventKind::BombPunched:
        case EventKind::BombGrabbed:
        case EventKind::BombThrown:
        case EventKind::Pickup:
        case EventKind::PickupJelly:
        case EventKind::PickupAwesome:
        case EventKind::TrampolineJump:
        case EventKind::Warped: return true;
        default: return false;
    }
}

void Client::applyStep(const StepInputs& bytes, const EventSink& events) {
    std::array<PlayerInput, kMaxPlayers> input{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = unpackInput(bytes[i]);
    world_->tick(kStepMs, input);
    lastInputs_ = bytes;
    ++applied_;
    std::vector<Event> happened = world_->takeEvents();
    // An own action that was already heard from the prediction is not heard twice. A
    // prediction that did not come true is forgotten after a second and a half.
    std::erase_if(heard_, [&](const Heard& h) { return now_ - h.atMs > 1500; });
    std::erase_if(happened, [&](const Event& e) {
        if (!ownAction(e)) return false;
        const auto it = std::find_if(heard_.begin(), heard_.end(), [&](const Heard& h) { return h.kind == e.kind && h.player == e.player; });
        if (it == heard_.end()) return false;
        heard_.erase(it);
        return true;
    });
    if (events && !happened.empty()) events(*world_, happened);
    if (const auto it = hashes_.find(applied_); it != hashes_.end()) {
        if (it->second != world_->stateHash() && !awaitingState_) {
            awaitingState_ = true;
            socket_.send(static_cast<std::uint8_t>(ClientMsg::NeedState), {});
        }
        hashes_.erase(hashes_.begin(), std::next(it));
    }
}

// The predicted state: a copy of the confirmed one, run forward to the step this player's
// present input should reach the server for. Own inputs are the ones remembered for each
// of those steps; everybody else is assumed to keep doing what they did last. It is thrown
// away and rebuilt on every local step, so a wrong guess lasts until the server's word arrives.
void Client::predict(const EventSink& events) {
    const int mySeat = seat();
    int ahead = predictionSteps_;
    if (ahead <= 0) {
        const ClientInfo* me = lobby_.client(id_);
        ahead = ((me != nullptr ? me->pingMs : 0) + kStepMs - 1) / kStepMs + 1;  // the round trip, and one step of sampling
    }
    ahead = std::clamp(ahead, 1, 10);
    const std::uint32_t wanted = applied_ + static_cast<std::uint32_t>(ahead);
    // The target moves on by one per local step, steadily, and is pulled back only when it
    // has drifted from where the confirmed step and the delay say it should be.
    ++target_;
    if (target_ + 2 < wanted || target_ > wanted + 2 || target_ <= applied_) target_ = wanted;
    mine_[target_] = input_;
    mine_.erase(mine_.begin(), mine_.upper_bound(applied_));
    if (!predicted_) predicted_ = std::make_unique<World>(*world_);
    else *predicted_ = *world_;
    for (std::uint32_t s = applied_ + 1; s <= target_ && !predicted_->roundOver(); ++s) {
        std::array<PlayerInput, kMaxPlayers> input{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = unpackInput(lastInputs_[i]);
        const auto own = mine_.find(s);
        input[static_cast<std::size_t>(mySeat)] = unpackInput(own != mine_.end() ? own->second : input_);
        predicted_->tick(kStepMs, input);
        // Sounds come from the confirmed steps, except this player's own actions: those are
        // given out the first time a step is predicted.
        std::vector<Event> happened = predicted_->takeEvents();
        if (s <= soundedUpTo_) continue;
        soundedUpTo_ = s;
        std::erase_if(happened, [&](const Event& e) { return !ownAction(e); });
        for (const Event& e : happened) heard_.push_back({e.kind, e.player, now_});
        if (events && !happened.empty()) events(*predicted_, happened);
    }
    predictedValid_ = true;
}

int Client::advance(std::uint64_t nowMs, const std::function<void(World&)>& before, const EventSink& after) {
    if (state_ != State::Round || !world_) return 0;
    now_ = nowMs;
    int ran = 0;
    const bool predicting = prediction_ && seat() >= 0;
    try {
        if (predicting) {
            // Confirmed steps are taken as soon as they arrive; the picture is paced by the local clock.
            while (!queue_.empty() && ran < 400) {
                const StepInputs bytes = queue_.front();
                queue_.pop_front();
                applyStep(bytes, after);
                ++ran;
            }
            if (world_->roundOver() || ended_) {
                // Nothing to guess once the round is decided: show the confirmed state.
                if (predictedValid_ && before) before(*predicted_);
                predictedValid_ = false;
            } else {
                if (nowMs > nextLocalAt_ + 500) nextLocalAt_ = nowMs;  // after a stall: no burst of steps
                while (nowMs >= nextLocalAt_) {
                    if (before) before(predictedValid_ ? *predicted_ : *world_);
                    predict(after);
                    nextLocalAt_ += kStepMs;
                    lastStepAt_ = nowMs;
                }
            }
        } else {
            predictedValid_ = false;
            while (!queue_.empty() && ran < 400 && (nowMs >= nextStepAt_ || queue_.size() > 20)) {
                const StepInputs bytes = queue_.front();
                queue_.pop_front();
                if (before) before(*world_);
                applyStep(bytes, after);
                ++ran;
                // Behind by more than a few steps: twice the speed until caught up.
                const std::uint64_t interval = queue_.size() > 3 ? kStepMs / 2 : kStepMs;
                nextStepAt_ = std::max(nextStepAt_, nowMs > 100 ? nowMs - 100 : 0) + interval;
                lastStepAt_ = nowMs;
            }
        }
    } catch (const std::exception& e) {
        fail(std::string("The round stopped with an error (") + e.what() + ")");
        return ran;
    }
    if (ended_ && applied_ >= end_.steps && queue_.empty() && !awaitingState_) {
        predictedValid_ = false;
        state_ = State::Result;
    }
    return ran;
}

float Client::stepAlpha(std::uint64_t nowMs) const {
    if (state_ != State::Round || nowMs <= lastStepAt_) return state_ == State::Round ? 0.0f : 1.0f;
    return std::min(1.0f, static_cast<float>(nowMs - lastStepAt_) / static_cast<float>(kStepMs));
}

// --- LAN browser -------------------------------------------------------------

void LanBrowser::stop() {
    udp_.close();
    servers_.clear();
    asked_ = false;
}

void LanBrowser::update(std::uint64_t nowMs) {
    if (!udp_.isOpen() && !udp_.open(0, true)) return;
    if (!asked_ || nowMs - askedAt_ >= 2000) {
        asked_ = true;
        askedAt_ = nowMs;
        ByteWriter w;
        w.u32(kUdpMagic);
        w.u8(static_cast<std::uint8_t>(UdpMsg::Query));
        w.u16(kProtocolVersion);
        udp_.sendTo({kBroadcast, kDiscoveryPort}, w.data());
        udp_.sendTo({kLoopback, kDiscoveryPort}, w.data());  // a server on this machine, where broadcast may not loop back
    }
    Address from;
    std::vector<std::uint8_t> data;
    for (int n = 0; n < 64 && udp_.receiveFrom(from, data); ++n) {
        ByteReader r(data);
        if (r.u32() != kUdpMagic || static_cast<UdpMsg>(r.u8()) != UdpMsg::Info) continue;
        ServerInfo info;
        if (!decode(r, info)) continue;
        info.name = cleanText(info.name, 32);
        Address at = from;
        at.port = info.port;
        // One machine may answer on several of its addresses: keep one line per server
        // name and port, preferring a non-loopback address.
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const Entry& e) {
            return e.address == at || (e.info.name == info.name && e.info.port == info.port && (e.address.loopback() || at.loopback()));
        });
        if (it == servers_.end()) {
            servers_.push_back({at, info, nowMs});
        } else {
            if (it->address.loopback()) it->address = at;
            it->info = info;
            it->seenAt = nowMs;
        }
    }
    std::erase_if(servers_, [&](const Entry& e) { return nowMs - e.seenAt > 7000; });
}

}  // namespace ab::net
