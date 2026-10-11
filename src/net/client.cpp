#include "net/client.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>

namespace ab::net {

namespace {
std::map<std::string, std::string> readKnown(const std::string& path) {
    std::map<std::string, std::string> known;
    std::ifstream in(path);
    std::string address, print;
    while (in >> address >> print) known[address] = print;
    return known;
}

void writeKnown(const std::string& path, const std::map<std::string, std::string>& known) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& [address, print] : known) out << address << " " << print << "\n";
}

constexpr std::uint64_t kConnectTimeoutMs = 8000;
constexpr std::size_t kChatKept = 100;
constexpr std::uint64_t kSilenceMs = 15000;  // the server sends something every second
}  // namespace

void Client::transmit(const Bytes& packet) {
    if (overTcp_) tcp_.send(kPacketFrame, packet);
    else udp_.sendTo(server_, packet);
}

void Client::send(ClientMsg type, const Bytes& payload) {
    if (keyed_) link_.sendMessage(static_cast<std::uint8_t>(type), payload);
}

void Client::fail(const std::string& why) {
    tcp_.close();
    udp_.close();
    link_.reset();
    keyed_ = false;
    world_.reset();
    predicted_.reset();
    predictedValid_ = false;
    queue_.clear();
    error_ = why;
    state_ = State::Failed;
}

void Client::disconnect() {
    if (keyed_) {
        // The server is told, twice in case one is lost; it would notice the silence anyway.
        link_.close();
        for (int i = 0; i < 2; ++i) link_.sendNow(clock_, [this](const Bytes& p) { transmit(p); });
        if (overTcp_) tcp_.pump();
    }
    tcp_.close();
    udp_.close();
    link_.reset();
    keyed_ = false;
    world_.reset();
    predicted_.reset();
    predictedValid_ = false;
    queue_.clear();
    chat_.clear();
    lobby_ = LobbyState{};
    error_.clear();
    state_ = State::Idle;
}

void Client::connect(const std::string& address, const std::string& name, const std::string& password, std::uint64_t nowMs) {
    disconnect();
    clock_ = nowMs;
    address_ = address;
    identity_.clear();
    identityKeyHex_.clear();
    certificate_.clear();
    identityKnown_ = false;
    identityChanged_ = false;
    name_ = name;
    password_ = password;
    connectAt_ = nowMs;
    satOut_ = false;
    keyed_ = false;
    welcomed_ = false;
    tcpIntro_ = false;
    udpHeard_ = false;
    answered_ = false;
    keySentAt_ = 0;
    cookie_.assign(kCookieBytes, 0);
    // "CODE@relay": the game is reached through a relay, by the code its host was given.
    std::string relay;
    viaRelay_ = splitRelayAddress(address, &relayCode_, &relay);
    const auto to = viaRelay_ ? resolveHostPort(relay, kRelayPort) : resolveHostPort(address, kDefaultPort);
    if (!to) {
        fail("Cannot find \"" + (viaRelay_ ? relay : address) + "\"");
        return;
    }
    server_ = *to;
    randomBytes(secret_.data(), secret_.size());
    public_ = x25519Base(secret_);
    // Over UDP if it can be had; a relay carries TCP only.
    overTcp_ = viaRelay_ || !udpAllowed_ || !udp_.open(0, false, false, server_.v6);
    if (overTcp_ && !tcp_.connect(server_)) {
        fail("Cannot connect to " + server_.text());
        return;
    }
    state_ = State::Connecting;
}

std::array<int, kMaxLocalPlayers> Client::seats() const {
    std::array<int, kMaxLocalPlayers> mine{-1, -1, -1, -1};
    if (!welcomed_) return mine;
    for (int s = 0; s < kMaxPlayers; ++s) {
        const Seat& seat = lobby_.seats[static_cast<std::size_t>(s)];
        if (seat.kind == SeatKind::Human && seat.client == id_ && lobby_.client(id_) != nullptr) mine[seat.local] = s;
    }
    return mine;
}

void Client::forgetServer(const std::string& address) {
    if (knownFile_.empty()) return;
    const auto at = resolveHostPort(address, kDefaultPort);
    if (!at) return;
    std::map<std::string, std::string> known = readKnown(knownFile_);
    if (known.erase(at->text()) > 0) writeKnown(knownFile_, known);
}

int Client::seat() const {
    for (int s : seats())
        if (s >= 0) return s;
    return -1;
}

int Client::localPlayers() const {
    const ClientInfo* me = lobby_.client(id_);
    return me != nullptr ? me->players : 1;
}

void Client::sendChat(const std::string& text) {
    const std::string clean = cleanText(text, kMaxChat);
    if (clean.empty() || !welcomed_) return;
    ByteWriter w;
    w.text(clean);
    send(ClientMsg::Chat, w.data());
}

void Client::sendOption(Option option, int direction) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(option));
    w.i8(direction);
    send(ClientMsg::Option, w.data());
}

void Client::sendStart() { send(ClientMsg::Start, {}); }
void Client::sendReady(bool ready) { send(ClientMsg::Ready, {static_cast<std::uint8_t>(ready ? 1 : 0)}); }
bool Client::ready() const {
    const ClientInfo* me = lobby_.client(id_);
    return me != nullptr && me->ready;
}
void Client::sendTeam(int local) { send(ClientMsg::Team, {static_cast<std::uint8_t>(local)}); }
void Client::sendLocalPlayers(int count) { send(ClientMsg::Locals, {static_cast<std::uint8_t>(count)}); }
void Client::sendContinue() { send(ClientMsg::Continue, {}); }

void Client::sendKick(std::uint8_t id) {
    ByteWriter w;
    w.u8(id);
    send(ClientMsg::Kick, w.data());
}

void Client::leaveMatch() {
    if (state_ != State::Round && state_ != State::Result) return;
    send(ClientMsg::LeaveMatch, {});
    satOut_ = true;
    world_.reset();
    predicted_.reset();
    predictedValid_ = false;
    queue_.clear();
    rouletteOn_ = false;
    state_ = State::Lobby;
}

void Client::sendBan(std::uint8_t id) {
    ByteWriter w;
    w.u8(id);
    send(ClientMsg::Ban, w.data());
}

void Client::sendUnban(const std::string& address) {
    ByteWriter w;
    w.text(address);
    send(ClientMsg::Unban, w.data());
}

void Client::sendAdmin(std::uint8_t id) {
    ByteWriter w;
    w.u8(id);
    send(ClientMsg::Admin, w.data());
}

void Client::sendLogin(const std::string& password) {
    ByteWriter w;
    w.text(password);
    send(ClientMsg::Login, w.data());
}

void Client::note(const std::string& text, std::uint64_t nowMs) {
    chat_.push_back({true, "", text, nowMs});
    while (chat_.size() > kChatKept) chat_.pop_front();
}

void Client::logInput() {
    if (!start_.recorded || ended_ || seat() < 0) return;
    if (log_.empty() ? input_ != InputTuple{} : input_ != log_.back()) log_.push_back(input_);
}

// More of the log to the server, with a signature over all of it so far.
void Client::sendLog(bool final) {
    if (!start_.recorded || !signWithPlayerKey_) return;
    InputLogMsg m;
    m.roundId = start_.roundId;
    m.firstIndex = static_cast<std::uint32_t>(logSent_);
    m.values.assign(log_.begin() + static_cast<std::ptrdiff_t>(logSent_), log_.end());
    m.signedCount = static_cast<std::uint32_t>(log_.size());
    m.signature = signWithPlayerKey_(logStatement(start_.recordId, m.signedCount, logChain(log_, log_.size())));
    m.final = final;
    send(ClientMsg::InputLog, encoded(m));
    logSent_ = log_.size();
    logAt_ = clock_;
    recordedLog_ = log_;
}

void Client::beginRound(const RoundStartMsg& m, std::uint64_t nowMs) {
    start_ = m;
    log_.clear();
    logSent_ = 0;
    logAt_ = nowMs;
    logFinalSent_ = false;
    if (m.recorded) {
        // What is kept to hold the record against, when it comes.
        recordedRound_ = m.roundId;
        recordedLog_.clear();
        recordedReveal_ = seedRound_ == m.roundId ? seedReveal_ : Key{};
        recordedCommits_ = seedRound_ == m.roundId ? seedCommits_ : std::vector<std::pair<std::uint8_t, Key>>{};
    }
    rouletteOn_ = false;
    end_ = RoundEndMsg{};
    ended_ = false;
    queue_.clear();
    held_.clear();
    haveState_ = false;
    newest_ = 0;
    applied_ = 0;
    ready_ = false;  // until the snapshot that follows the round start
    nextStepAt_ = nowMs;
    lastStepAt_ = nowMs;
    inputSentAt_ = 0;
    predicted_.reset();
    predictedValid_ = false;
    target_ = 0;
    soundedUpTo_ = 0;
    heard_.clear();
    mine_.clear();
    nextLocalAt_ = nowMs;
    // The server's tuning table on top of the built-in one: the rules are the server's.
    Values values = Values::defaults();
    for (const auto& [id, value] : m.values) values.set(id, value);
    try {
        // Built here only to have the round's rules in place; what is on the field comes with
        // the snapshot the server sends next, without the hidden powerups.
        world_ = std::make_unique<World>(values, 1);
        applyRoundSetup(*world_, values, m.setup);
    } catch (const std::exception& e) {
        fail(std::string("The server sent a round this game cannot play (") + e.what() + ")");
        return;
    }
    state_ = State::Round;
}

// A state has arrived whole or been rebuilt from a difference.
void Client::accept(std::uint32_t step, Fields fields, std::vector<Event> events) {
    held_[step] = fields;
    // The server makes differences from states up to 64 steps old; older ones are of no use.
    while (!held_.empty() && held_.begin()->first + 64 < step) held_.erase(held_.begin());
    newest_ = step;
    haveState_ = true;
    queue_.push_back({step, std::move(fields), std::move(events)});
}

void Client::handleDelta(const DeltaMsg& m) {
    if (!world_ || m.roundId != start_.roundId || satOut_) return;
    if (haveState_ && m.step <= newest_) return;  // an old one, or one already had
    const auto base = held_.find(m.baseStep);
    if (base == held_.end()) return;  // made from a state not held: the server learns it from the next input message
    Fields fields;
    if (!decodeDelta(base->second, m.delta, fields)) return;
    // The events of every step after the base are in it; those after the newest state held are new.
    std::vector<Event> events;
    for (const StepEvent& e : m.events)
        if (e.step > newest_) events.push_back(e.event);
    accept(m.step, std::move(fields), std::move(events));
}

void Client::handleFrame(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t nowMs) {
    ByteReader r(payload);
    // Having gone back to the lobby, the rest of the match is none of this client's business.
    if (satOut_ && (static_cast<ServerMsg>(type) == ServerMsg::RoundStart || static_cast<ServerMsg>(type) == ServerMsg::Delta ||
                    static_cast<ServerMsg>(type) == ServerMsg::Snapshot || static_cast<ServerMsg>(type) == ServerMsg::RoundEnd ||
                    static_cast<ServerMsg>(type) == ServerMsg::Roulette))
        return;
    switch (static_cast<ServerMsg>(type)) {
        case ServerMsg::Welcome: {
            WelcomeMsg m;
            if (!decode(r, m)) return fail("Bad answer from the server");
            id_ = m.id;
            token_ = m.token;
            serverName_ = m.serverName;
            welcomed_ = true;
            state_ = State::Lobby;
            break;
        }
        case ServerMsg::Reject: fail(r.text(256)); break;
        case ServerMsg::SeedAsk: {
            // A share in the next round's seed: a random number, promised now and shown when everybody has promised.
            const std::uint32_t round = r.u32();
            if (!r.ok()) break;
            seedRound_ = round;
            seedCommits_.clear();
            randomBytes(seedReveal_.data(), seedReveal_.size());
            ByteWriter w;
            w.u32(round);
            for (std::uint8_t b : commitOf(seedReveal_)) w.u8(b);
            send(ClientMsg::SeedCommit, w.data());
            break;
        }
        case ServerMsg::SeedCommits: {
            SeedCommitsMsg m;
            if (!decode(r, m) || m.roundId != seedRound_ || !seedCommits_.empty()) break;
            // Shown only if our own promise is among them as we made it (and once only).
            bool ours = false;
            for (const auto& [client, commit] : m.commits) ours = ours || (client == id_ && commit == commitOf(seedReveal_));
            if (!ours) break;
            seedCommits_ = m.commits;
            ByteWriter w;
            w.u32(m.roundId);
            for (std::uint8_t b : seedReveal_) w.u8(b);
            send(ClientMsg::SeedReveal, w.data());
            break;
        }
        case ServerMsg::Record: {
            const std::uint32_t round = r.u32();
            const Bytes text = r.bytes(1u << 20);
            if (!r.ok() || round != recordedRound_) break;
            lastRecord_.assign(text.begin(), text.end());
            ++recordSerial_;
            break;
        }
        case ServerMsg::Certificate: {
            const Bytes text = r.bytes(8192);
            if (r.ok()) certificate_.assign(text.begin(), text.end());
            break;
        }
        case ServerMsg::Lobby: {
            LobbyState m;
            if (!decode(r, m)) break;
            lobby_ = std::move(m);
            if (lobby_.phase == Phase::Lobby) rouletteOn_ = false, satOut_ = false;
            if (alwaysReady_ && lobby_.phase == Phase::Lobby && !ready() && clock_ - readySentAt_ > 500) {
                readySentAt_ = clock_;
                sendReady(true);
            }
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
        case ServerMsg::Delta: {  // one too large for a datagram
            DeltaMsg m;
            if (decode(r, m)) handleDelta(m);
            break;
        }
        case ServerMsg::Roulette: {
            RouletteMsg m;
            if (!decode(r, m)) break;
            roulette_ = m;
            rouletteOn_ = true;
            rouletteAt_ = nowMs;
            break;
        }
        case ServerMsg::RoundEnd: {
            RoundEndMsg m;
            if (!decode(r, m) || m.roundId != start_.roundId) break;
            end_ = m;
            ended_ = true;
            // The last word on what was pressed, for the record.
            if (!logFinalSent_ && seat() >= 0) {
                logFinalSent_ = true;
                sendLog(true);
            }
            break;
        }
        case ServerMsg::Snapshot: {
            SnapshotMsg m;
            if (!decode(r, m) || !world_ || m.roundId != start_.roundId) break;
            if (haveState_ && m.step <= newest_) break;
            Fields fields;
            if (!decodeDelta({}, m.state, fields)) return fail("The server sent a game state this game cannot read");
            if (haveState_) ++snapshots_;  // the one a round begins with is not a repair
            accept(m.step, std::move(fields), {});
            break;
        }
        case ServerMsg::Ping: send(ClientMsg::Pong, payload); break;
        default: break;
    }
}

void Client::sendInput(std::uint64_t nowMs) {
    // A recorded round: every different thing held down goes into the log before it is sent,
    // and the log goes to the server about once a second.
    logInput();
    if (start_.recorded && !ended_ && seat() >= 0 && log_.size() > logSent_ && nowMs - logAt_ >= 1000) sendLog(false);
    if (input_ == inputSent_ && newest_ == ackSent_ && nowMs - inputSentAt_ < static_cast<std::uint64_t>(kStepMs) && inputSentAt_ != 0) return;
    InputMsg m;
    m.roundId = start_.roundId;
    m.haveState = haveState_;
    m.haveStep = newest_;
    if (!mine_.empty() && predictedValid_) {
        // What was assumed for the step being guessed and the ones before it.
        m.step = mine_.rbegin()->first;
        for (int k = 0; k < kInputsPerMessage; ++k) {
            const auto it = mine_.find(m.step - static_cast<std::uint32_t>(k));
            if (it == mine_.end()) break;
            m.inputs.push_back(it->second);
        }
        m.inputs[0] = input_;  // the keys as they are now
    } else {
        m.step = applied_ + 1;
        m.inputs.push_back(input_);
    }
    link_.sendDatagram(static_cast<std::uint8_t>(ClientMsg::Input), encoded(m));
    inputSent_ = input_;
    ackSent_ = newest_;
    inputSentAt_ = nowMs;
}

// The first packet, in the clear: a fresh public key, and the cookie if the server asked for
// one. Longer than the answer, so that nobody can use a server to send much to a forged address.
void Client::sendKey(std::uint64_t nowMs) {
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(kClientKey);
    w.u16(kProtocolVersion);
    for (std::uint8_t b : public_) w.u8(b);
    for (std::uint8_t b : cookie_) w.u8(b);
    while (w.data().size() < 96) w.u8(0);
    transmit(w.data());
    keySentAt_ = nowMs;
}

void Client::handlePacket(const Bytes& packet, std::uint64_t nowMs) {
    ByteReader r(packet);
    if (r.u32() != kUdpMagic) return;
    const std::uint8_t kind = r.u8();
    if (kind == kSealedDatagram) {
        if (keyed_) link_.receive(packet, nowMs);
        return;
    }
    if (keyed_ || !r.ok()) return;  // once the keys are agreed nothing in the clear is listened to
    if (kind != kCookie && kind != kClearReject && kind != kServerKey) return;
    answered_ = true;
    if (!overTcp_) udpHeard_ = true;
    if (kind == kCookie) {
        // "Say that again with this": shows the server that the answer reaches us.
        cookie_.assign(kCookieBytes, 0);
        for (std::uint8_t& b : cookie_) b = r.u8();
        if (r.ok()) sendKey(nowMs);
        return;
    }
    if (kind == kClearReject) return fail(cleanText(r.text(256), 200));
    if (kind != kServerKey) return;
    Key theirs{}, identity{};
    for (std::uint8_t& b : theirs) b = r.u8();
    for (std::uint8_t& b : identity) b = r.u8();
    const std::uint32_t token = r.u32();
    if (!r.ok()) return fail("Bad answer from the server");
    // Is this the server that was at this address before? (A server on this very
    // machine is not asked.) Whoever answers must also own the identity it shows:
    // without that key it cannot arrive at the connection's keys.
    identity_ = fingerprint(identity);
    identityKeyHex_ = toHex(identity);
    if (!expectedIdentity_.empty()) {
        Key expected{};
        if (!fromHex(expectedIdentity_, expected) || expected != identity) return fail("This is not the game server the master server named (another identity)");
    }
    // (Not through a relay either: its codes are new each time, so there is nothing to remember by.)
    if (expectedIdentity_.empty() && !knownFile_.empty() && !viaRelay_ && (!server_.loopback() || identityOnLoopback_)) {
        std::map<std::string, std::string> known = readKnown(knownFile_);
        const auto it = known.find(server_.text());
        if (it != known.end() && it->second != identity_) {
            identityChanged_ = true;
            return fail("This is not the server that was at " + server_.text() + " before (identity " + identity_ + ", remembered " + it->second +
                        "). Someone may be listening in, or the server was set up anew.");
        }
        identityKnown_ = it != known.end();
        if (!identityKnown_) {
            known[server_.text()] = identity_;
            writeKnown(knownFile_, known);
        }
    }
    if (!deriveSession(secret_, theirs, public_, theirs, x25519(secret_, identity), identity, keys_)) return fail("Bad answer from the server");
    token_ = token;
    link_.start(keys_.udpToServer, keys_.udpToClient, token, nowMs);
    keyed_ = true;
    secret_ = {};
    HelloMsg hello;
    hello.name = name_;
    // Not the password itself: a value that shows we know it, good for this connection only.
    if (!password_.empty()) hello.proof = passwordProof(password_, keys_.master);
    if (signWithPlayerKey_) {
        hello.playerKey = playerKey_;
        hello.playerSignature = signWithPlayerKey_(playerProofMessage(keys_.master));
    }
    send(ClientMsg::Hello, encoded(hello));
}

void Client::update(std::uint64_t nowMs) {
    if (state_ == State::Idle || state_ == State::Failed) return;
    clock_ = nowMs;
    const auto over = [this]() { return state_ == State::Failed || state_ == State::Idle; };

    if (overTcp_) {
        const bool alive = tcp_.pump();
        if (tcp_.connected() && !tcpIntro_) {
            if (viaRelay_) {
                // First a word to the relay; everything after it is passed on to the host.
                ByteWriter join;
                join.u32(kRelayMagic);
                join.text(relayCode_);
                tcp_.send(static_cast<std::uint8_t>(RelayMsg::Join), join.data());
            }
            tcpIntro_ = true;
            sendKey(nowMs);  // once: this line loses nothing
        }
        std::uint8_t type = 0;
        Bytes payload;
        while (!over() && tcp_.receive(type, payload)) {
            if (!keyed_ && viaRelay_ && static_cast<RelayMsg>(type) == RelayMsg::Refuse) {
                ByteReader r(payload);
                return fail(cleanText(r.text(256), 200));
            }
            if (type == kPacketFrame) handlePacket(payload, nowMs);
        }
        if (over()) return;
        if (!alive) return fail(welcomed_ ? "The connection to the server was lost" : "Cannot connect to " + server_.text());
    } else {
        Address from;
        Bytes data;
        for (int n = 0; n < 256 && !over() && udp_.receiveFrom(from, data); ++n)
            if (from == server_) handlePacket(data, nowMs);
        if (over()) return;
        if (!keyed_) {
            if (keySentAt_ == 0 || nowMs - keySentAt_ >= 250) sendKey(nowMs);
            // Not a word over UDP: perhaps this network lets none through. The same inside a TCP connection.
            if (!udpHeard_ && nowMs - connectAt_ > 2000) {
                udp_.close();
                overTcp_ = true;
                if (!tcp_.connect(server_)) return fail("Cannot connect to " + server_.text());
            }
        }
    }
    if (!welcomed_ && nowMs - connectAt_ > kConnectTimeoutMs) return fail("No answer from " + server_.text());
    if (!keyed_) {
        if (overTcp_) tcp_.pump();
        return;
    }

    std::uint8_t type = 0;
    Bytes payload;
    while (!over() && link_.nextMessage(type, payload)) handleFrame(type, payload, nowMs);
    while (!over() && link_.nextDatagram(type, payload)) {
        if (static_cast<ServerMsg>(type) != ServerMsg::Delta || satOut_) continue;
        ++deltas_;
        largestDelta_ = std::max(largestDelta_, payload.size());
        ByteReader r(payload);
        DeltaMsg m;
        if (decode(r, m)) handleDelta(m);
    }
    if (over()) return;
    if (link_.closedByPeer() || link_.broken() || nowMs - link_.lastHeard() > kSilenceMs) return fail("The connection to the server was lost");

    if (state_ == State::Round) sendInput(nowMs);
    link_.update(nowMs, [this](const Bytes& p) { transmit(p); });
    if (overTcp_) tcp_.pump();
}

// Things a player does with their own hands: worth hearing without the round trip.
bool Client::ownAction(const Event& e) const {
    const auto mine = seats();
    if (e.player < 0 || std::find(mine.begin(), mine.end(), e.player) == mine.end()) return false;
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

// The server's word on a step: the state after it, and what happened in it.
bool Client::applyState(const Received& state, const EventSink& events) {
    if (!world_->loadSnapshotFields(state.fields)) {
        fail("The server sent a game state this game cannot read");
        return false;
    }
    applied_ = state.step;
    ready_ = true;
    std::vector<Event> happened = state.events;
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
    return true;
}

// The predicted state: a copy of the newest state from the server, run forward with the
// same gameplay core to the step this player's present input should reach the server for.
// Own inputs are the ones remembered (and sent) for each of those steps; everybody else is
// assumed to keep doing what they did last. It is thrown away and rebuilt on every local
// step, so a wrong guess lasts until the server's word arrives.
void Client::predict(const EventSink& events) {
    const std::array<int, kMaxLocalPlayers> mySeats = seats();
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
    logInput();  // whatever is sent for a step is in the log first
    mine_[target_] = input_;
    mine_.erase(mine_.begin(), mine_.upper_bound(applied_));
    if (!predicted_) predicted_ = std::make_unique<World>(*world_);
    else *predicted_ = *world_;
    predicted_->ownRandom(applied_ * 2654435761u + 7u);  // chance cannot be guessed: any numbers will do until the server's arrive
    for (std::uint32_t s = applied_ + 1; s <= target_ && !predicted_->roundOver(); ++s) {
        std::array<PlayerInput, kMaxPlayers> input{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = world_->player(static_cast<int>(i)).lastInput;
        const auto own = mine_.find(s);
        for (std::size_t l = 0; l < mySeats.size(); ++l)
            if (mySeats[l] >= 0) input[static_cast<std::size_t>(mySeats[l])] = unpackInput(own != mine_.end() ? own->second[l] : input_[l]);
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
    // Nothing decided by chance, and nobody else's next move, is shown before the server has decided it.
    std::array<bool, kMaxPlayers> own{};
    for (int seat : mySeats)
        if (seat >= 0) own[static_cast<std::size_t>(seat)] = true;
    predicted_->keepToConfirmed(*world_, own);
    predictedValid_ = true;
}

int Client::advance(std::uint64_t nowMs, const std::function<void(World&)>& before, const EventSink& after) {
    if (state_ != State::Round || !world_ || (!ready_ && queue_.empty())) return 0;
    now_ = nowMs;
    int ran = 0;
    const bool predicting = prediction_ && seat() >= 0;
    try {
        if (predicting) {
            // The server's states are taken as soon as they arrive; the picture is paced by the local clock.
            while (!queue_.empty() && ran < 400) {
                const Received next = std::move(queue_.front());
                queue_.pop_front();
                if (!applyState(next, after)) return ran;
                ++ran;
            }
            if (!ready_) return ran;
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
                const Received next = std::move(queue_.front());
                queue_.pop_front();
                if (before && ready_) before(*world_);
                if (!applyState(next, after)) return ran;
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
    if (ended_ && ready_ && applied_ >= end_.steps && queue_.empty()) {
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
