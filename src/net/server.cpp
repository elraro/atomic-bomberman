#include "net/server.hpp"

#include <algorithm>
#include <cctype>

#include "resources/settings.hpp"

namespace ab::net {

namespace {
constexpr std::size_t kMaxConnections = 32;
constexpr std::uint64_t kHelloTimeoutMs = 5000;
constexpr std::uint64_t kSilenceTimeoutMs = 15000;
constexpr std::uint64_t kRoundLeadMs = 1000;    // between RoundStart and the first step
constexpr int kStepsAfterDecision = 20;         // the field stays on screen for a second
constexpr std::uint64_t kResultMinMs = 1500;
constexpr std::uint64_t kResultMaxMs = 10000;
}  // namespace

struct Server::Peer {
    TcpSocket socket;
    std::uint8_t id = 0;
    bool joined = false;
    bool gone = false;
    std::string name;
    std::uint32_t token = 0;
    std::uint64_t order = 0;       // join order: the lowest is the administrator
    std::uint64_t connectedAt = 0;
    std::uint64_t lastHeard = 0;
    std::uint64_t lastChat = 0;
    std::uint64_t lastSnapshot = 0;
    int seat = -1;
    int pingMs = 0;
    bool udpKnown = false;
    Address udpAddress{};
    bool udpOn = false;
    std::uint32_t ackRound = 0;
    std::uint32_t ackStep = 0;     // steps the client holds (from its input messages)
    std::uint32_t sentStep = 0;    // steps sent over TCP
    std::uint8_t input = 0;
    bool wantsContinue = false;
};

Server::Server() = default;
Server::~Server() { stop(); }

void Server::log(const std::string& line) const {
    if (config_.log) config_.log(line);
}

bool Server::start(const ServerConfig& config) {
    stop();
    config_ = config;
    error_.clear();
    if (!startup() || !listener_.listen(config.port)) {
        error_ = "Cannot listen on TCP port " + std::to_string(config.port);
        return false;
    }
    if (!udp_.open(listener_.port(), false, false, true)) {
        // Not fatal: clients fall back to TCP; the server just cannot be found on the LAN.
        log("WARN  UDP port " + std::to_string(listener_.port()) + " is not available: TCP only, no LAN discovery");
    }
    if (config.discoverable && !discovery_.open(kDiscoveryPort, false, true))
        log("WARN  UDP port " + std::to_string(kDiscoveryPort) + " is not available: this server will not show up in LAN searches");
    values_ = Values::defaults();
    schemes_.clear();
    if (!config.gameDir.empty()) {
        Values fromFile;
        if (fromFile.loadFile(config.gameDir + "/data/res/valuelst.res"))
            for (const auto& [id, value] : fromFile.entries()) values_.set(id, value);
        else
            log("WARN  valuelst.res not found under " + config.gameDir + ": default values");
        schemes_ = listSchemes(config.gameDir + "/data/schemes", config.userSchemesDir);
    }
    rng_ = Rng(config.seed != 0 ? config.seed : static_cast<std::uint32_t>(clockMs() * 2654435761u + 12345u));
    settings_ = config.settings;
    settings_.schemeIndex = 0;
    std::string wanted = config.scheme;
    for (char& ch : wanted) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (std::size_t i = 0; i < schemes_.size(); ++i)
        if (schemes_[i].file == wanted) settings_.schemeIndex = static_cast<int>(i);
    loadScheme();
    phase_ = Phase::Lobby;
    score_ = {};
    roundId_ = 0;
    world_.reset();
    seatKind_ = {};
    rebuildSeats();
    refreshLobby();
    running_ = true;
    mapperReported_ = false;
    routerNotice_.clear();
    if (config.upnp) mapper_.start(listener_.port(), "Atomic Bomberman");
    log("INFO  Server \"" + config.name + "\" listening port=" + std::to_string(listener_.port()) + " schemes=" +
        std::to_string(schemes_.size()) + (config.password.empty() ? "" : " password=yes"));
    return true;
}

void Server::stop() {
    if (!running_) return;
    for (auto& p : peers_) {
        if (p->joined) reject(*p, "The server has shut down");
    }
    peers_.clear();
    mapper_.stop();
    listener_.close();
    udp_.close();
    discovery_.close();
    world_.reset();
    running_ = false;
}

int Server::players() const {
    int n = 0;
    for (const auto& p : peers_) n += p->joined && !p->gone ? 1 : 0;
    return n;
}

Server::Peer* Server::peerById(std::uint8_t id) {
    for (auto& p : peers_)
        if (p->joined && !p->gone && p->id == id) return p.get();
    return nullptr;
}

Server::Peer* Server::peerAtSeat(int seat) {
    for (auto& p : peers_)
        if (p->joined && !p->gone && p->seat == seat) return p.get();
    return nullptr;
}

Server::Peer* Server::admin() {
    Peer* first = nullptr;
    for (auto& p : peers_)
        if (p->joined && !p->gone && (first == nullptr || p->order < first->order)) first = p.get();
    return first;
}

void Server::broadcast(ServerMsg type, const std::vector<std::uint8_t>& payload) {
    for (auto& p : peers_)
        if (p->joined && !p->gone) p->socket.send(static_cast<std::uint8_t>(type), payload);
}

void Server::say(const std::string& text) {
    broadcast(ServerMsg::Chat, encoded(ChatMsg{kServerSender, "", text}));
    log("CHAT  * " + text);
}

void Server::tell(Peer& p, const std::string& text) {
    p.socket.send(static_cast<std::uint8_t>(ServerMsg::Chat), encoded(ChatMsg{kServerSender, "", text}));
}

void Server::reject(Peer& p, const std::string& reason) {
    ByteWriter w;
    w.text(reason);
    p.socket.send(static_cast<std::uint8_t>(ServerMsg::Reject), w.data());
    p.socket.pump();
    p.gone = true;
}

// The chosen scheme, or the built-in arena when there is no game data.
void Server::loadScheme() {
    schemeFile_ = SchemeFile{};
    schemeFile_.scheme = Scheme::pillars();
    schemeFile_.name = "Built-in arena";
    for (int i = 0; i < kMaxPlayers; ++i) schemeFile_.team[static_cast<std::size_t>(i)] = i & 1;
    if (!schemes_.empty()) {
        settings_.schemeIndex = std::clamp(settings_.schemeIndex, 0, static_cast<int>(schemes_.size()) - 1);
        const SchemeEntry& entry = schemes_[static_cast<std::size_t>(settings_.schemeIndex)];
        if (auto file = loadSchemeFile(entry.path)) schemeFile_ = *file;
        else log("WARN  cannot read scheme " + entry.path);
    } else {
        settings_.schemeIndex = 0;
    }
    settings_.schemeTitle = schemeFile_.name;
    for (int i = 0; i < kMaxPlayers; ++i) teams_[static_cast<std::size_t>(i)] = schemeFile_.team[static_cast<std::size_t>(i)] != 0 ? 1 : 0;
}

// Humans keep their seats; watchers get the lowest free seat; computer players
// fill what is left, up to the number wanted.
void Server::rebuildSeats() {
    seatKind_ = {};
    for (auto& p : peers_)
        if (p->joined && !p->gone && p->seat >= 0) seatKind_[static_cast<std::size_t>(p->seat)] = SeatKind::Human;
    for (auto& p : peers_) {
        if (!p->joined || p->gone || p->seat >= 0) continue;
        for (int s = 0; s < kMaxPlayers; ++s)
            if (seatKind_[static_cast<std::size_t>(s)] == SeatKind::Empty) {
                p->seat = s;
                seatKind_[static_cast<std::size_t>(s)] = SeatKind::Human;
                break;
            }
    }
    int computers = settings_.computers;
    for (int s = 0; s < kMaxPlayers && computers > 0; ++s)
        if (seatKind_[static_cast<std::size_t>(s)] == SeatKind::Empty) {
            seatKind_[static_cast<std::size_t>(s)] = SeatKind::Computer;
            --computers;
        }
    lobbyDirty_ = true;
}

void Server::refreshLobby() {
    lobby_.phase = phase_;
    lobby_.settings = settings_;
    const Peer* a = admin();
    lobby_.admin = a != nullptr ? a->id : kServerSender;
    for (int s = 0; s < kMaxPlayers; ++s) {
        Seat& seat = lobby_.seats[static_cast<std::size_t>(s)];
        seat.kind = seatKind_[static_cast<std::size_t>(s)];
        seat.team = teams_[static_cast<std::size_t>(s)];
        seat.client = 0;
        if (const Peer* p = peerAtSeat(s)) seat.client = p->id;
        else if (seat.kind == SeatKind::Human) seat.kind = SeatKind::Computer;  // left during the match
    }
    lobby_.clients.clear();
    for (const auto& p : peers_)
        if (p->joined && !p->gone) lobby_.clients.push_back({p->id, p->name, p->seat, p->pingMs});
}

void Server::leave(Peer& p) {
    if (!p.joined) return;
    p.joined = false;
    log("INFO  Left id=" + std::to_string(p.id) + " name=\"" + p.name + "\"");
    say(p.name + " has left");
    if (phase_ == Phase::Lobby) {
        rebuildSeats();
    } else {
        // A computer player takes the seat over until the match ends. With no human
        // left in a seat the match is abandoned.
        bool anyone = false;
        for (auto& q : peers_) anyone = anyone || (q->joined && !q->gone && q->seat >= 0);
        if (!anyone) toLobby();
    }
    lobbyDirty_ = true;
}

void Server::changeOption(Option option, int direction) {
    const int d = direction < 0 ? -1 : 1;
    MatchSettings& s = settings_;
    switch (option) {
        case Option::Level: s.level = (s.level + d + 1 + 12) % 12 - 1; break;  // random, then the eleven levels
        case Option::Scheme:
            if (!schemes_.empty()) {
                const int n = static_cast<int>(schemes_.size());
                s.schemeIndex = (s.schemeIndex + d + n) % n;
                loadScheme();
            }
            break;
        case Option::Wins: s.winsNeeded = std::clamp(s.winsNeeded + d, 1, 9); break;
        case Option::TeamPlay: s.teamPlay = !s.teamPlay; break;
        case Option::PlayTime: s.playTime = Settings::nextPlayTime(s.playTime, d); break;
        case Option::Enclosement: s.enclosementDepth = (s.enclosementDepth + d + 4) % 4; break;
        case Option::Computers:
            s.computers = std::clamp(s.computers + d, 0, kMaxPlayers - 1);
            rebuildSeats();
            break;
        case Option::RandomStart: s.randomStart = !s.randomStart; break;
        case Option::ConveyorSpeed: s.conveyorSpeed = (s.conveyorSpeed + d + 3) % 3; break;
        case Option::StompedBombs: s.stompedBombsDetonate = !s.stompedBombsDetonate; break;
        case Option::WinByKills: s.winByKills = !s.winByKills; break;
        case Option::DiseasesDestroyable: s.diseasesDestroyable = !s.diseasesDestroyable; break;
        case Option::Count: break;
    }
    lobbyDirty_ = true;
}

void Server::handleHello(Peer& p, const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    HelloMsg hello;
    if (!decode(r, hello)) {
        p.gone = true;  // not one of ours
        return;
    }
    if (hello.version != kProtocolVersion) {
        reject(p, "Different game version (server protocol " + std::to_string(kProtocolVersion) + ", yours " +
                      std::to_string(hello.version) + ")");
        return;
    }
    if (!config_.password.empty() && hello.password != config_.password) {
        reject(p, hello.password.empty() ? "This server needs a password" : "Wrong password");
        log("INFO  Refused (password) from " + p.socket.peer().text());
        return;
    }
    // A free id and a name nobody else has.
    for (int id = 0; id < 255; ++id)
        if (peerById(static_cast<std::uint8_t>(id)) == nullptr) {
            p.id = static_cast<std::uint8_t>(id);
            break;
        }
    std::string name = cleanText(hello.name, kMaxName);
    if (name.empty()) name = "Player";
    auto taken = [&](const std::string& n) {
        for (auto& q : peers_)
            if (q->joined && !q->gone && q->name == n) return true;
        return n == "Computer";
    };
    for (int n = 2; taken(name); ++n) name = cleanText(hello.name.empty() ? "Player" : hello.name, kMaxName - 2) + " " + std::to_string(n);
    p.name = name;
    p.token = static_cast<std::uint32_t>(rng_.next()) << 17 ^ static_cast<std::uint32_t>(rng_.next()) << 2 ^ static_cast<std::uint32_t>(now_);
    p.order = ++joinCounter_;
    p.joined = true;
    p.seat = -1;
    p.socket.send(static_cast<std::uint8_t>(ServerMsg::Welcome), encoded(WelcomeMsg{p.id, p.token, config_.name}));
    log("INFO  Joined id=" + std::to_string(p.id) + " name=\"" + p.name + "\" from " + p.socket.peer().text());
    if (phase_ == Phase::Lobby) {
        rebuildSeats();
    } else {
        // In the middle of a match: watch it from the present state.
        p.socket.send(static_cast<std::uint8_t>(ServerMsg::RoundStart), roundStart_);
        sendSnapshot(p);
        if (phase_ == Phase::Result) p.socket.send(static_cast<std::uint8_t>(ServerMsg::RoundEnd), roundEnd_);
    }
    lobbyDirty_ = true;
    if (!routerNotice_.empty()) tell(p, routerNotice_);
    say(p.name + " has joined" + (p.seat < 0 && phase_ != Phase::Lobby ? " (watching until the match ends)" : ""));
}

void Server::applyInput(Peer& p, const InputMsg& m) {
    if (m.roundId != roundId_) return;
    p.input = m.input;
    if (p.ackRound != roundId_) {
        p.ackRound = roundId_;
        p.ackStep = 0;
    }
    if (m.haveStep > p.ackStep && m.haveStep <= history_.size()) p.ackStep = m.haveStep;
}

void Server::handleFrame(Peer& p, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    p.lastHeard = now_;
    if (!p.joined) {
        if (static_cast<ClientMsg>(type) == ClientMsg::Hello) handleHello(p, payload);
        else p.gone = true;
        return;
    }
    ByteReader r(payload);
    switch (static_cast<ClientMsg>(type)) {
        case ClientMsg::Chat: {
            const std::string text = cleanText(r.text(1024), kMaxChat);
            if (!r.ok() || text.empty() || now_ - p.lastChat < 300) break;
            p.lastChat = now_;
            broadcast(ServerMsg::Chat, encoded(ChatMsg{p.id, p.name, text}));
            log("CHAT  " + p.name + ": " + text);
            break;
        }
        case ClientMsg::Option: {
            const int option = r.u8();
            const int direction = r.i8();
            if (!r.ok() || &p != admin() || phase_ != Phase::Lobby || option >= static_cast<int>(Option::Count)) break;
            changeOption(static_cast<Option>(option), direction);
            break;
        }
        case ClientMsg::Start:
            if (&p == admin() && phase_ == Phase::Lobby)
                if (const std::string why = startMatch(); !why.empty()) tell(p, why);
            break;
        case ClientMsg::Team:
            if (phase_ == Phase::Lobby && p.seat >= 0) {
                teams_[static_cast<std::size_t>(p.seat)] ^= 1;
                lobbyDirty_ = true;
            }
            break;
        case ClientMsg::Kick: {
            const std::uint8_t id = r.u8();
            Peer* target = r.ok() ? peerById(id) : nullptr;
            if (&p != admin() || target == nullptr || target == &p) break;
            say(target->name + " was removed by the administrator");
            reject(*target, "Removed by the administrator");
            break;
        }
        case ClientMsg::Input: {
            InputMsg m;
            if (decode(r, m)) applyInput(p, m);
            break;
        }
        case ClientMsg::UdpState: {
            const bool on = r.flag();
            if (!r.ok()) break;
            if (on && p.udpKnown) {
                p.udpOn = true;
            } else if (p.udpOn) {
                p.udpOn = false;
                p.sentStep = p.ackRound == roundId_ ? p.ackStep : 0;  // go on over TCP from what it has
            }
            break;
        }
        case ClientMsg::NeedState:
            if (phase_ != Phase::Lobby && world_ && (p.lastSnapshot == 0 || now_ - p.lastSnapshot >= 1000)) {
                log("WARN  State resent to id=" + std::to_string(p.id) + " name=\"" + p.name + "\" step=" + std::to_string(history_.size()));
                sendSnapshot(p);
            }
            break;
        case ClientMsg::Continue: p.wantsContinue = true; break;
        case ClientMsg::Pong: {
            const std::uint32_t sent = r.u32();
            if (!r.ok()) break;
            const auto elapsed = static_cast<std::uint32_t>(now_) - sent;
            if (elapsed < 60000) p.pingMs = static_cast<int>(elapsed);
            break;
        }
        default: break;  // unknown types are ignored: a newer client may send more
    }
}

void Server::handleDatagram(const Address& from, const std::vector<std::uint8_t>& data, bool viaDiscovery) {
    ByteReader r(data);
    if (r.u32() != kUdpMagic) return;
    const auto type = static_cast<UdpMsg>(r.u8());
    if (!r.ok()) return;
    if (type == UdpMsg::Query) {
        if (!config_.discoverable) return;
        ServerInfo info;
        info.port = listener_.port();
        info.name = config_.name;
        info.players = players();
        info.phase = phase_;
        info.password = !config_.password.empty();
        // Answered from the socket the question came to.
        (viaDiscovery ? discovery_ : udp_).sendTo(from, datagram(UdpMsg::Info, info));
        return;
    }
    if (type == UdpMsg::Probe) {
        const std::uint32_t token = r.u32();
        if (!r.ok()) return;
        for (auto& p : peers_)
            if (p->joined && !p->gone && p->token == token) {
                p->udpKnown = true;
                p->udpAddress = from;
                p->lastHeard = now_;
                udp_.sendTo(from, datagram(UdpMsg::ProbeAck));
            }
        return;
    }
    if (type == UdpMsg::Input) {
        InputMsg m;
        if (!decode(r, m)) return;
        for (auto& p : peers_)
            if (p->joined && !p->gone && p->token == m.token) {
                p->udpAddress = from;  // follows a changed NAT mapping
                p->udpKnown = true;
                p->lastHeard = now_;
                applyInput(*p, m);
            }
    }
}

std::string Server::startMatch() {
    int occupied = 0;
    std::array<int, 2> perTeam{};
    for (int s = 0; s < kMaxPlayers; ++s)
        if (seatKind_[static_cast<std::size_t>(s)] != SeatKind::Empty) {
            ++occupied;
            ++perTeam[static_cast<std::size_t>(teams_[static_cast<std::size_t>(s)])];
        }
    if (occupied < 2) return "Must have at least two players selected!";
    if (settings_.teamPlay && (perTeam[0] == 0 || perTeam[1] == 0)) return "Must have at least one player on each team!";

    score_ = {};
    level_ = settings_.level;
    if (level_ < 0) {
        // "Random Each Game": values 1150-1160 say which levels may come up.
        for (int tries = 0; tries < 100; ++tries) {
            level_ = rng_.below(11);
            if (!values_.has(1150 + level_) || values_.get(1150 + level_) != 0) break;
        }
    }
    startCells_ = schemeFile_.scheme.start;
    if (settings_.randomStart)
        for (int n = 0; n < 200; ++n)
            std::swap(startCells_[static_cast<std::size_t>(rng_.below(kMaxPlayers))], startCells_[static_cast<std::size_t>(rng_.below(kMaxPlayers))]);
    ai_.clear();
    for (int i = 0; i < kMaxPlayers; ++i) ai_.emplace_back(static_cast<std::uint32_t>(rng_.next()) * 31u + static_cast<std::uint32_t>(i) * 977u + 5u);
    log("INFO  Match started level=" + std::to_string(level_) + " scheme=\"" + settings_.schemeTitle + "\" players=" + std::to_string(occupied));
    startRound();
    return {};
}

void Server::startRound() {
    ++roundId_;
    RoundStartMsg start;
    start.roundId = roundId_;
    start.seed = (static_cast<std::uint32_t>(rng_.next()) << 16) ^ static_cast<std::uint32_t>(rng_.next());
    start.values = values_.entries();
    start.winsNeeded = settings_.winsNeeded;
    start.score = score_;
    RoundSetup& setup = start.setup;
    setup.level = level_;
    setup.scheme = schemeFile_.scheme;
    setup.scheme.start = startCells_;
    setup.powers = schemeFile_.powers;
    if (!config_.gameDir.empty()) setup.extras = loadExtrasFile(config_.gameDir + "/data/res/extra" + std::to_string(level_) + ".res");
    setup.conveyorSpeed = settings_.conveyorSpeed;
    setup.teamPlay = settings_.teamPlay;
    setup.teams = teams_;
    setup.enclosementDepth = settings_.enclosementDepth;
    setup.stompedBombsDetonate = settings_.stompedBombsDetonate;
    setup.diseasesDestroyable = settings_.diseasesDestroyable;
    setup.playTime = settings_.playTime;
    setup.winByKills = settings_.winByKills;
    for (int s = 0; s < kMaxPlayers; ++s) {
        setup.present[static_cast<std::size_t>(s)] = seatKind_[static_cast<std::size_t>(s)] != SeatKind::Empty;
        setup.human[static_cast<std::size_t>(s)] = seatKind_[static_cast<std::size_t>(s)] == SeatKind::Human;
    }
    world_ = std::make_unique<World>(values_, start.seed);
    applyRoundSetup(*world_, values_, setup);
    history_.clear();
    hashes_.clear();
    decided_ = false;
    winner_ = -1;
    afterSteps_ = 0;
    roundStartAt_ = now_ + kRoundLeadMs;
    roundStart_ = encoded(start);
    roundEnd_.clear();
    for (auto& p : peers_) {
        p->ackRound = roundId_;
        p->ackStep = 0;
        p->sentStep = 0;
        p->input = 0;
        p->wantsContinue = false;
    }
    phase_ = Phase::Round;
    lobbyDirty_ = true;
    broadcast(ServerMsg::RoundStart, roundStart_);
    log("INFO  Round " + std::to_string(roundId_) + " started seed=" + std::to_string(start.seed));
}

void Server::stepRound() {
    if (now_ < roundStartAt_) return;
    std::uint64_t due = (now_ - roundStartAt_) / kStepMs + 1;
    if (due > history_.size() + 20) {  // a long stall: do not replay it at speed
        roundStartAt_ += (due - history_.size() - 5) * kStepMs;
        due = history_.size() + 5;
    }
    int ran = 0;
    while (history_.size() < due && ran < 5 && phase_ == Phase::Round) {
        StepInputs bytes{};
        std::array<PlayerInput, kMaxPlayers> input{};
        for (int s = 0; s < kMaxPlayers; ++s) {
            const auto i = static_cast<std::size_t>(s);
            if (!world_->player(s).present) continue;
            const Peer* human = seatKind_[i] == SeatKind::Human ? peerAtSeat(s) : nullptr;
            bytes[i] = human != nullptr ? human->input : packInput(ai_[i].decide(*world_, s, kStepMs));
            input[i] = unpackInput(bytes[i]);
        }
        world_->tick(kStepMs, input);
        world_->takeEvents();
        history_.push_back(bytes);
        hashes_.push_back(world_->stateHash());
        ++ran;
        if (world_->roundOver()) {
            if (!decided_) {
                decided_ = true;
                winner_ = score_.roundDecided(*world_, settings_.winsNeeded, settings_.winByKills);
            }
            if (++afterSteps_ > kStepsAfterDecision) endRound();
        }
    }
    if (ran > 0)
        for (auto& p : peers_)
            if (p->joined && !p->gone) sendSteps(*p);
}

void Server::sendSteps(Peer& p) {
    const auto total = static_cast<std::uint32_t>(history_.size());
    auto message = [&](std::uint32_t from, std::uint32_t count) {
        StepsMsg m;
        m.roundId = roundId_;
        m.firstStep = from;
        m.steps.assign(history_.begin() + from, history_.begin() + from + count);
        m.hash = hashes_[from + count - 1];
        return m;
    };
    if (p.udpOn && p.udpKnown) {
        // Everything the client has not acknowledged, so a lost datagram needs no request.
        const std::uint32_t from = p.ackRound == roundId_ ? std::min(p.ackStep, total) : 0;
        const std::uint32_t count = std::min<std::uint32_t>(total - from, kMaxStepsPerMessage);
        if (count > 0) udp_.sendTo(p.udpAddress, datagram(UdpMsg::Steps, message(from, count)));
        return;
    }
    while (p.sentStep < total) {
        const std::uint32_t count = std::min<std::uint32_t>(total - p.sentStep, kMaxStepsPerMessage);
        p.socket.send(static_cast<std::uint8_t>(ServerMsg::Steps), encoded(message(p.sentStep, count)));
        p.sentStep += count;
    }
}

void Server::sendSnapshot(Peer& p) {
    if (!world_) return;
    SnapshotMsg m;
    m.roundId = roundId_;
    m.step = static_cast<std::uint32_t>(history_.size());
    m.state = world_->saveState();
    p.socket.send(static_cast<std::uint8_t>(ServerMsg::Snapshot), encoded(m));
    p.lastSnapshot = now_;
    p.ackRound = roundId_;
    p.ackStep = m.step;
    p.sentStep = m.step;
}

void Server::endRound() {
    RoundEndMsg end;
    end.roundId = roundId_;
    end.steps = static_cast<std::uint32_t>(history_.size());
    end.winner = winner_;
    end.teamPlay = settings_.teamPlay;
    end.score = score_;
    roundEnd_ = encoded(end);
    broadcast(ServerMsg::RoundEnd, roundEnd_);
    phase_ = Phase::Result;
    resultAt_ = now_;
    lobbyDirty_ = true;
    const std::string who = winner_ < 0 ? "draw" : settings_.teamPlay ? "team " + std::to_string(winner_ + 1) : lobby_.seatName(winner_);
    log("INFO  Round " + std::to_string(roundId_) + " over steps=" + std::to_string(end.steps) + " winner=" + who +
        (score_.matchWinner >= 0 ? " (match decided)" : ""));
}

void Server::toLobby() {
    phase_ = Phase::Lobby;
    world_.reset();
    history_.clear();
    hashes_.clear();
    rebuildSeats();
    log("INFO  Back in the lobby");
}

void Server::update(std::uint64_t nowMs) {
    if (!running_) return;
    now_ = nowMs;

    while (auto socket = listener_.accept()) {
        if (peers_.size() >= kMaxConnections) continue;  // dropped: the socket closes here
        auto p = std::make_unique<Peer>();
        p->socket = std::move(*socket);
        p->connectedAt = p->lastHeard = now_;
        peers_.push_back(std::move(p));
    }

    for (std::size_t i = 0; i < peers_.size(); ++i) {
        Peer& p = *peers_[i];
        if (p.gone) continue;
        const bool alive = p.socket.pump();
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        while (!p.gone && p.socket.receive(type, payload)) handleFrame(p, type, payload);
        if (!alive) p.gone = true;
        if (!p.joined && now_ - p.connectedAt > kHelloTimeoutMs) p.gone = true;
        if (p.joined && !p.gone && now_ - p.lastHeard > kSilenceTimeoutMs) {
            log("INFO  Timed out id=" + std::to_string(p.id));
            reject(p, "Connection timed out");
        }
    }
    if (udp_.isOpen()) {
        Address from;
        std::vector<std::uint8_t> data;
        for (int n = 0; n < 256 && udp_.receiveFrom(from, data); ++n) handleDatagram(from, data, false);
    }
    if (discovery_.isOpen()) {
        Address from;
        std::vector<std::uint8_t> data;
        for (int n = 0; n < 64 && discovery_.receiveFrom(from, data); ++n) handleDatagram(from, data, true);
    }
    // Departures, one at a time: each may change seats or end the match.
    for (std::size_t i = 0; i < peers_.size();) {
        if (!peers_[i]->gone) {
            ++i;
            continue;
        }
        std::unique_ptr<Peer> p = std::move(peers_[i]);
        peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        leave(*p);
    }

    if (phase_ == Phase::Round) {
        stepRound();
    } else if (phase_ == Phase::Result) {
        // Clients on UDP may still miss the last steps.
        if (now_ - resendAt_ >= 100) {
            resendAt_ = now_;
            for (auto& p : peers_)
                if (p->joined && p->udpOn && p->ackRound == roundId_ && p->ackStep < history_.size()) sendSteps(*p);
        }
        bool everyone = true, anyone = false;
        for (auto& p : peers_)
            if (p->joined && p->seat >= 0) {
                anyone = true;
                everyone = everyone && p->wantsContinue;
            }
        const std::uint64_t waited = now_ - resultAt_;
        if (!anyone) {
            toLobby();
        } else if ((everyone && waited >= kResultMinMs) || waited >= kResultMaxMs) {
            if (score_.matchWinner >= 0) {
                const std::string who = settings_.teamPlay ? "Team " + std::to_string(score_.matchWinner + 1) : lobby_.seatName(score_.matchWinner);
                toLobby();
                say(who + " wins the match!");
            } else {
                startRound();
            }
        }
    }

    if (mapper_.started() && mapper_.finished() && !mapperReported_) {
        mapperReported_ = true;
        const UpnpResult r = mapper_.result();
        log(std::string(r.ok ? "INFO  " : "WARN  ") + "Router: " + r.message + (r.externalIp.empty() ? "" : "; public address " + r.externalIp));
        routerNotice_ = r.ok ? "The router lets this game through" + (r.externalIp.empty() ? std::string() : ": others can join at " + r.externalIp + ":" + std::to_string(listener_.port()))
                             : "The router did not open the port by itself (" + r.message + "). Players outside your network need port " +
                                   std::to_string(listener_.port()) + " (TCP and UDP) forwarded by hand.";
        say(routerNotice_);
    }
    if (now_ - pingSentAt_ >= 1000) {
        pingSentAt_ = now_;
        ByteWriter w;
        w.u32(static_cast<std::uint32_t>(now_));
        broadcast(ServerMsg::Ping, w.data());
    }
    if (lobbyDirty_ || now_ - lobbySentAt_ >= 3000) {  // the periodic one carries fresh ping times
        lobbyDirty_ = false;
        lobbySentAt_ = now_;
        refreshLobby();
        broadcast(ServerMsg::Lobby, encoded(lobby_));
    }
    for (auto& p : peers_)
        if (!p->gone && !p->socket.pump()) p->gone = true;
}

}  // namespace ab::net
