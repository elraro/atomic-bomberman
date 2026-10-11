#include "net/server.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include <filesystem>
#include <fstream>

#include "game/roulette.hpp"
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
// Limits against misuse.
constexpr std::uint64_t kForever = ~0ull;
constexpr std::uint64_t kKickBlockMs = 5 * 60 * 1000;       // a kicked player's address stays out this long
constexpr std::size_t kConnectsPerTenSeconds = 20;          // more from one address: blocked for a minute
constexpr std::size_t kPasswordTriesPerMinute = 5;          // more wrong passwords: blocked for five minutes
constexpr std::size_t kStatesKept = 64;                     // differences are made from states up to this many steps old
constexpr std::uint64_t kSeedStageMs = 1500;                // for the promises, and again for the numbers
constexpr std::uint64_t kRecordWaitMs = 1200;               // for the players' last statements, before the record is sealed
constexpr std::uint32_t kRecordHashEvery = 20;
constexpr int kFramesPerSecond = 400;                       // a client sends about 40

// "a.b.c.d" or "[x::y]" without the port: what bans and limits go by.
std::string hostOf(const Address& a) {
    const std::string text = a.text();
    return text.substr(0, text.rfind(':'));
}
}  // namespace

struct Server::Peer {
    Link link;
    bool viaTcp = false;           // the packets travel as frames of this TCP connection (no UDP path, or a relay)
    TcpSocket socket;
    Address address{};             // where its packets come from and go to
    Key clientPublic{};            // the key it opened the connection with
    Bytes keyAnswer;               // what was answered, to say again if the answer got lost
    std::uint64_t closeUntil = 0;
    std::uint8_t id = 0;
    bool joined = false;
    bool gone = false;
    std::string name;
    std::string host;              // the address bans and limits go by (the real one also for a player who came through a relay)
    std::uint32_t token = 0;       // session number: names the connection in its datagrams (not a secret)
    bool keyed = false;            // the key exchange is done
    SessionKeys keys;
    std::uint64_t order = 0;       // join order: the lowest is the administrator
    std::uint64_t connectedAt = 0;
    std::uint64_t lastChat = 0;
    std::uint64_t lastSnapshot = 0;
    std::uint64_t frameWindowAt = 0;
    int frames = 0;
    std::array<int, kMaxLocalPlayers> seats{-1, -1, -1, -1};  // one per player at that computer
    int wanted = 1;                // players at that computer
    bool seated() const { return seats[0] >= 0 || seats[1] >= 0 || seats[2] >= 0 || seats[3] >= 0; }
    bool out = false;              // went back to the lobby during this match: its seats are played by the computer until it ends
    bool playing() const { return seated() && !out; }
    int pingMs = 0;
    std::uint32_t ackRound = 0;
    bool haveState = false;        // it has said that it holds a state of this round...
    std::uint32_t ackStep = 0;     // ...the one after this step (from its input messages)
    std::array<std::uint8_t, kMaxLocalPlayers> input{};  // what its players hold down, as last applied
    std::uint32_t inputStep = 0;   // the step that input was meant for
    std::map<std::uint32_t, std::array<std::uint8_t, kMaxLocalPlayers>> inputs;  // for steps not yet run
    bool wantsContinue = false;
    Key playerKey{};               // the player key proved when joining; zeros: none
    bool ready = false;            // said so in the lobby; asked again after every match
};

Server::Server() = default;
Server::~Server() { stop(); }

void Server::transmit(Peer& p, const Bytes& packet) {
    if (p.viaTcp) p.socket.send(kPacketFrame, packet);
    else udp_.sendTo(p.address, packet);
}

void Server::send(Peer& p, ServerMsg type, const Bytes& payload) {
    if (p.keyed) p.link.sendMessage(static_cast<std::uint8_t>(type), payload);
}

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
        // Not fatal: clients fall back to a TCP connection; the server just cannot be found on the LAN.
        log("WARN  UDP port " + std::to_string(listener_.port()) + " is not available: slower connections inside TCP only, no LAN discovery");
    }
    if (config.discoverable && !discovery_.open(kDiscoveryPort, false, true))
        log("WARN  UDP port " + std::to_string(kDiscoveryPort) + " is not available: this server will not show up in LAN searches");
    values_ = Values::defaults();
    schemes_.clear();
    campaigns_.clear();
    campaignMode_ = false;
    lastWinner_ = -1;
    prizeType_ = -1;
    if (!config.gameDir.empty()) {
        Values fromFile;
        if (fromFile.loadFile(config.gameDir + "/data/res/valuelst.res"))
            for (const auto& [id, value] : fromFile.entries()) values_.set(id, value);
        else
            log("WARN  valuelst.res not found under " + config.gameDir + ": default values");
        schemes_ = listSchemes(config.gameDir + "/data/schemes", config.userSchemesDir);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(config.gameDir + "/data/res", ec)) {
            std::string ext = entry.path().extension().string();
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext == ".cam") campaigns_.emplace_back(entry.path().stem().string(), entry.path().string());
        }
        std::sort(campaigns_.begin(), campaigns_.end());
    }
    // The identity: a key kept from one start to the next, so that players' games can tell
    // this server from one pretending to be it.
    bool haveIdentity = false;
    if (!config.identityFile.empty()) {
        std::ifstream in(config.identityFile);
        std::string hex;
        in >> hex;
        haveIdentity = fromHex(hex, identitySecret_);
    }
    if (!haveIdentity) {
        randomBytes(identitySecret_.data(), identitySecret_.size());
        if (!config.identityFile.empty()) {
            std::ofstream out(config.identityFile, std::ios::binary);
            out << toHex(identitySecret_) << "\n";
            if (!out) log("WARN  Cannot write the identity file " + config.identityFile + ": a new identity at every start");
        }
    }
    identityPublic_ = x25519Base(identitySecret_);
    randomBytes(cookieSecret_.data(), cookieSecret_.size());
    adminId_ = -1;
    blocked_.clear();
    connects_.clear();
    passwordFails_.clear();
    if (!config.banFile.empty()) {
        std::ifstream in(config.banFile);
        std::string line;
        while (std::getline(in, line)) {
            if (auto semi = line.find(';'); semi != std::string::npos) line.erase(semi);
            line = cleanText(line, 64);
            if (!line.empty()) blocked_[line] = kForever;
        }
        if (!bans().empty()) log("INFO  Banned addresses: " + std::to_string(bans().size()));
    }
    rng_ = Rng(config.seed != 0 ? config.seed : static_cast<std::uint32_t>(clockMs() * 2654435761u + 12345u));
    settings_ = config.settings;
    settings_.schemeIndex = 0;
    settings_.campaign = 0;
    settings_.campaignTitle.clear();
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
        std::to_string(schemes_.size()) + (config.password.empty() ? "" : " password=yes") + " identity=" + identity());
    return true;
}

void Server::stop() {
    if (!running_) return;
    for (auto& p : peers_) {
        if (p->joined) reject(*p, "The server has shut down");
    }
    // Said twice in case one is lost; a client would notice the silence anyway.
    for (auto* list : {&peers_, &closing_})
        for (auto& p : *list) {
            if (!p->keyed) continue;
            p->link.close();
            for (int i = 0; i < 2; ++i) p->link.sendNow(now_, [&](const Bytes& packet) { transmit(*p, packet); });
            if (p->viaTcp) p->socket.pump();
        }
    peers_.clear();
    closing_.clear();
    seeding_ = Seeding{};
    recordPending_ = false;
    mapper_.stop();
    relayControl_.close();
    adopting_.clear();
    relayCode_.clear();
    relayNotice_.clear();
    relayResolved_ = false;
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

Server::Peer* Server::peerAtSeat(int seat, int* local) {
    for (auto& p : peers_) {
        if (!p->joined || p->gone) continue;
        for (int l = 0; l < kMaxLocalPlayers; ++l)
            if (p->seats[static_cast<std::size_t>(l)] == seat) {
                if (local != nullptr) *local = l;
                return p.get();
            }
    }
    return nullptr;
}

void Server::makeAdmin(Peer& p, const std::string& how) {
    adminId_ = p.id;
    lobbyDirty_ = true;
    log("INFO  Administrator is now id=" + std::to_string(p.id) + " name=\"" + p.name + "\" (" + how + ")");
    say(p.name + " is now the administrator");
}

// The chosen administrator while connected; otherwise whoever has been here longest.
Server::Peer* Server::admin() {
    if (adminId_ >= 0)
        if (Peer* chosen = peerById(static_cast<std::uint8_t>(adminId_))) return chosen;
    Peer* first = nullptr;
    for (auto& p : peers_)
        if (p->joined && !p->gone && (first == nullptr || p->order < first->order)) first = p.get();
    return first;
}

void Server::broadcast(ServerMsg type, const std::vector<std::uint8_t>& payload) {
    for (auto& p : peers_)
        if (p->joined && !p->gone) send(*p, type, payload);
}

void Server::say(const std::string& text) {
    broadcast(ServerMsg::Chat, encoded(ChatMsg{kServerSender, "", text}));
    log("CHAT  * " + text);
}

void Server::block(const std::string& host, std::uint64_t untilMs) {
    std::uint64_t& until = blocked_[host];
    until = std::max(until, untilMs);
}

bool Server::blocked(const std::string& host) const {
    const auto it = blocked_.find(host);
    return it != blocked_.end() && it->second > now_;
}

std::vector<std::string> Server::bans() const {
    std::vector<std::string> out;
    for (const auto& [host, until] : blocked_)
        if (until == kForever) out.push_back(host);
    return out;
}

void Server::saveBans() const {
    if (config_.banFile.empty()) return;
    std::ofstream out(config_.banFile, std::ios::binary);
    out << "; Addresses banned from this Atomic Bomberman server, one per line.\n";
    for (const std::string& host : bans()) out << host << "\n";
    if (!out) log("WARN  Cannot write the ban file " + config_.banFile);
}

bool Server::unban(const std::string& address) {
    const auto it = blocked_.find(address);
    if (it == blocked_.end() || it->second != kForever) return false;
    blocked_.erase(it);
    saveBans();
    log("INFO  Unbanned " + address);
    return true;
}

// Sends a player away. Kicked: the address may come back in five minutes. Banned: not until unbanned.
void Server::remove(Peer& p, bool ban, const std::string& by) {
    const std::string host = p.host;
    block(host, ban ? kForever : now_ + kKickBlockMs);
    // A ban also names the player's key, if they showed one: a new address does not bring them back under it.
    if (ban && p.playerKey != Key{}) block("key:" + toHex(p.playerKey), kForever);
    if (ban) saveBans();
    log(std::string("INFO  ") + (ban ? "Banned" : "Kicked") + " name=\"" + p.name + "\" address=" + host + " by " + by);
    say(p.name + (ban ? " was banned by " : " was removed by ") + by);
    reject(p, ban ? "Banned by " + by : "Removed by " + by);
}

bool Server::kick(const std::string& name, bool ban) {
    for (auto& p : peers_)
        if (p->joined && !p->gone && p->name == name) {
            remove(*p, ban, "the server");
            return true;
        }
    return false;
}

void Server::passwordFailed(Peer& p) {
    const std::string host = p.host;
    std::deque<std::uint64_t>& tries = passwordFails_[host];
    tries.push_back(now_);
    while (!tries.empty() && now_ - tries.front() > 60000) tries.pop_front();
    if (tries.size() >= kPasswordTriesPerMinute) {
        block(host, now_ + kKickBlockMs);
        tries.clear();
        log("WARN  Too many wrong passwords from " + host + ": blocked for five minutes");
    }
}

void Server::tell(Peer& p, const std::string& text) {
    send(p, ServerMsg::Chat, encoded(ChatMsg{kServerSender, "", text}));
}

void Server::reject(Peer& p, const std::string& reason) {
    ByteWriter w;
    w.text(reason);
    send(p, ServerMsg::Reject, w.data());
    p.gone = true;  // its link goes on until the message has arrived (update())
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
    for (auto& p : peers_) {
        if (!p->joined || p->gone) continue;
        for (int l = 0; l < kMaxLocalPlayers; ++l) {
            int& seat = p->seats[static_cast<std::size_t>(l)];
            if (l >= p->wanted) seat = -1;  // a player fewer at that computer
            if (seat >= 0) seatKind_[static_cast<std::size_t>(seat)] = SeatKind::Human;
        }
    }
    for (auto& p : peers_) {
        if (!p->joined || p->gone) continue;
        for (int l = 0; l < p->wanted; ++l) {
            if (p->seats[static_cast<std::size_t>(l)] >= 0) continue;
            for (int s = 0; s < kMaxPlayers; ++s)
                if (seatKind_[static_cast<std::size_t>(s)] == SeatKind::Empty) {
                    p->seats[static_cast<std::size_t>(l)] = s;
                    seatKind_[static_cast<std::size_t>(s)] = SeatKind::Human;
                    break;
                }
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
        seat.local = 0;
        int local = 0;
        if (const Peer* p = peerAtSeat(s, &local)) seat.client = p->id, seat.local = static_cast<std::uint8_t>(local);
        else if (seat.kind == SeatKind::Human) seat.kind = SeatKind::Computer;  // left during the match
    }
    lobby_.clients.clear();
    for (const auto& p : peers_)
        if (p->joined && !p->gone) {
            ClientInfo info;
            info.id = p->id;
            info.name = p->name;
            for (int l = kMaxLocalPlayers - 1; l >= 0; --l)
                if (p->seats[static_cast<std::size_t>(l)] >= 0) info.seat = p->seats[static_cast<std::size_t>(l)];
            info.players = p->wanted;
            info.pingMs = p->pingMs;
            info.ready = p->ready;
            info.playerKey = p->playerKey;
            lobby_.clients.push_back(info);
        }
}

void Server::leave(Peer& p) {
    if (!p.joined) return;
    p.joined = false;
    if (adminId_ == p.id) adminId_ = -1;  // the role falls back to whoever has been here longest
    log("INFO  Left id=" + std::to_string(p.id) + " name=\"" + p.name + "\"");
    say(p.name + " has left");
    if (phase_ == Phase::Lobby) {
        rebuildSeats();
    } else {
        // A computer player takes the seat over until the match ends. With no human
        // left in a seat the match is abandoned.
        bool anyone = false;
        for (auto& q : peers_) anyone = anyone || (q->joined && !q->gone && q->playing());
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
        case Option::Goldman: s.goldman = !s.goldman; break;
        case Option::Campaign: {
            const int n = static_cast<int>(campaigns_.size()) + 1;  // off, then each campaign
            s.campaign = (s.campaign + d + n) % n;
            s.campaignTitle = s.campaign > 0 ? campaigns_[static_cast<std::size_t>(s.campaign - 1)].first : std::string();
            break;
        }
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
    if (!config_.password.empty()) {
        const Key expected = passwordProof(config_.password, p.keys.master);
        if (!equalConstantTime(expected.data(), hello.proof.data(), expected.size())) {
            const bool none = hello.proof == Key{};
            reject(p, none ? "This server needs a password" : "Wrong password");
            log("INFO  Refused (password) from " + p.address.text());
            passwordFailed(p);
            return;
        }
    }
    // A player key: its owner must be at the other end of this very connection, and must not be banned.
    if (hello.playerKey != Key{} && config_.verifySignature) {
        if (!config_.verifySignature(hello.playerKey, playerProofMessage(p.keys.master), hello.playerSignature)) {
            reject(p, "The player key was not proved (bad signature)");
            log("WARN  Refused (player key not proved) from " + p.address.text());
            return;
        }
        if (blocked("key:" + toHex(hello.playerKey))) {
            reject(p, "Banned");
            return;
        }
        p.playerKey = hello.playerKey;
    }
    if (!config_.allowedKeys.empty() && (p.playerKey == Key{} || std::find(config_.allowedKeys.begin(), config_.allowedKeys.end(), p.playerKey) == config_.allowedKeys.end())) {
        reject(p, "This match is for its listed players only");
        log("INFO  Refused (not a listed player) from " + p.address.text());
        return;
    }
    // One connection for each listed player: a second one under the same key is not let in.
    if (!config_.allowedKeys.empty())
        for (auto& q : peers_)
            if (q.get() != &p && q->joined && !q->gone && q->playerKey == p.playerKey) {
                reject(p, "That player is already here");
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
    p.order = ++joinCounter_;
    p.joined = true;
    p.seats = {-1, -1, -1, -1};
    p.wanted = 1;
    send(p, ServerMsg::Welcome, encoded(WelcomeMsg{p.id, p.token, config_.name}));
    log("INFO  Joined id=" + std::to_string(p.id) + " name=\"" + p.name + "\" from " + p.address.text());
    if (phase_ == Phase::Lobby) {
        rebuildSeats();
    } else {
        // In the middle of a match: watch it from the present state.
        send(p, ServerMsg::RoundStart, roundStart_);
        sendSnapshot(p);
        if (phase_ == Phase::Result) send(p, ServerMsg::RoundEnd, roundEnd_);
    }
    lobbyDirty_ = true;
    if (!routerNotice_.empty()) tell(p, routerNotice_);
    if (!relayNotice_.empty() && &p == admin()) tell(p, relayNotice_);
    say(p.name + " has joined" + (!p.seated() && phase_ != Phase::Lobby ? " (watching until the match ends)" : ""));
}

void Server::applyInput(Peer& p, const InputMsg& m) {
    if (m.roundId != roundId_) return;
    if (p.ackRound != roundId_) {
        p.ackRound = roundId_;
        p.haveState = false;
        p.ackStep = 0;
    }
    const auto done = static_cast<std::uint32_t>(history_.size());
    if (m.haveState && m.haveStep <= done && (!p.haveState || m.haveStep > p.ackStep)) {
        p.haveState = true;
        p.ackStep = m.haveStep;
    }
    // Inputs for steps still to come wait for their step. Of those that come too late the
    // newest counts from now on (it is what the player is holding down).
    for (std::size_t i = 0; i < m.inputs.size() && i <= m.step; ++i) {
        const std::uint32_t step = m.step - static_cast<std::uint32_t>(i);
        if (step > done) {
            if (step <= done + 40) p.inputs[step] = m.inputs[i];
        } else {
            if (step >= p.inputStep) p.input = m.inputs[i], p.inputStep = step;
            break;
        }
    }
}

void Server::handleFrame(Peer& p, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    // Far more messages than a game client ever sends: not a game client.
    if (now_ - p.frameWindowAt >= 1000) p.frameWindowAt = now_, p.frames = 0;
    if (++p.frames > kFramesPerSecond) {
        const std::string host = p.host;
        log("WARN  Flood of messages from " + host + ": dropped and blocked for a minute");
        block(host, now_ + 60000);
        p.gone = true;
        return;
    }
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
            if (config_.lockSettings) {
                tell(p, "The settings of this match are fixed");
                break;
            }
            changeOption(static_cast<Option>(option), direction);
            break;
        }
        case ClientMsg::Start:
            if (&p == admin() && phase_ == Phase::Lobby)
                if (const std::string why = startMatch(); !why.empty()) tell(p, why);
            break;
        case ClientMsg::Ready: {
            const bool ready = r.flag();
            if (!r.ok() || phase_ != Phase::Lobby || p.ready == ready) break;
            p.ready = ready;
            lobbyDirty_ = true;
            break;
        }
        case ClientMsg::Team:
        {
            // Which of the computer's players changes team (the first if not said).
            const int local = payload.empty() ? 0 : r.u8();
            if (phase_ == Phase::Lobby && local < kMaxLocalPlayers && p.seats[static_cast<std::size_t>(local)] >= 0) {
                teams_[static_cast<std::size_t>(p.seats[static_cast<std::size_t>(local)])] ^= 1;
                lobbyDirty_ = true;
            }
            break;
        }
        case ClientMsg::Locals: {
            // Players at that computer, 1-4; each gets a seat while there are seats.
            const int count = r.u8();
            if (!r.ok() || phase_ != Phase::Lobby) break;
            p.wanted = std::clamp(count, 1, kMaxLocalPlayers);
            rebuildSeats();
            break;
        }
        case ClientMsg::Kick: {
            const std::uint8_t id = r.u8();
            Peer* target = r.ok() ? peerById(id) : nullptr;
            if (&p != admin() || target == nullptr || target == &p) break;
            remove(*target, false, "the administrator");
            break;
        }
        case ClientMsg::Ban: {
            const std::uint8_t id = r.u8();
            Peer* target = r.ok() ? peerById(id) : nullptr;
            if (&p != admin() || target == nullptr || target == &p) break;
            tell(p, target->name + "'s address is " + target->host + " (to lift the ban: /unban " + target->host + ")");
            if (target->playerKey != Key{}) tell(p, "Their player key is banned too (/unban key:" + toHex(target->playerKey) + ")");
            remove(*target, true, "the administrator");
            break;
        }
        case ClientMsg::LeaveMatch: {
            // Back to the lobby without leaving the server: the match goes on for the others
            // with the computer in this player's seats; with nobody left playing it is over.
            if (phase_ == Phase::Lobby || p.out) break;
            p.out = true;
            if (p.seated()) say(p.name + " has gone back to the lobby");
            bool anyone = false;
            for (auto& q : peers_) anyone = anyone || (q->joined && !q->gone && q->playing());
            if (!anyone) toLobby();
            lobbyDirty_ = true;
            break;
        }
        case ClientMsg::Admin: {
            // The administrator hands the role to another player.
            const std::uint8_t id = r.u8();
            Peer* target = r.ok() ? peerById(id) : nullptr;
            if (&p != admin() || target == nullptr || target == &p) break;
            makeAdmin(*target, "handed over by " + p.name);
            break;
        }
        case ClientMsg::Login: {
            // The server owner's way in: the administrator password.
            const std::string given = r.text(128);
            if (!r.ok()) break;
            const Key a = sha256(reinterpret_cast<const std::uint8_t*>(given.data()), given.size());
            const Key b = sha256(reinterpret_cast<const std::uint8_t*>(config_.adminPassword.data()), config_.adminPassword.size());
            if (config_.adminPassword.empty() || !equalConstantTime(a.data(), b.data(), a.size())) {
                tell(p, "That is not the administrator password");
                log("INFO  Refused (administrator password) from " + p.address.text());
                passwordFailed(p);
                break;
            }
            if (&p != admin()) makeAdmin(p, "administrator password");
            break;
        }
        case ClientMsg::Unban: {
            const std::string address = cleanText(r.text(128), 64);
            if (!r.ok() || &p != admin()) break;
            tell(p, unban(address) ? address + " may join again" : "No ban on " + address);
            break;
        }
        case ClientMsg::Input: {
            InputMsg m;
            if (decode(r, m)) applyInput(p, m);
            break;
        }
        case ClientMsg::Continue: p.wantsContinue = true; break;
        case ClientMsg::SeedCommit: {
            const std::uint32_t round = r.u32();
            Key commit{};
            for (std::uint8_t& b : commit) b = r.u8();
            if (!r.ok() || !seeding_.active || seeding_.stage != 1 || round != seeding_.roundId) break;
            if (std::find(seeding_.asked.begin(), seeding_.asked.end(), p.id) == seeding_.asked.end()) break;
            bool had = false;
            for (const SeedShare& s : seeding_.shares) had = had || s.client == p.id;
            if (!had) seeding_.shares.push_back({p.id, commit, {}, false});
            break;
        }
        case ClientMsg::SeedReveal: {
            const std::uint32_t round = r.u32();
            Key reveal{};
            for (std::uint8_t& b : reveal) b = r.u8();
            if (!r.ok() || !seeding_.active || seeding_.stage != 2 || round != seeding_.roundId) break;
            for (SeedShare& s : seeding_.shares)
                if (s.client == p.id && !s.revealed && commitOf(reveal) == s.commit) s.reveal = reveal, s.revealed = true;  // a number that does not fit the promise is no number
            break;
        }
        case ClientMsg::InputLog: {
            InputLogMsg m;
            if (!decode(r, m) || m.roundId != roundId_ || phase_ == Phase::Lobby) break;
            const auto it = logs_.find(p.id);
            if (it == logs_.end()) break;  // had no seat when the round began
            PlayerLog& log = it->second;
            // In order, as the link delivers them; anything else is dropped (and the record will show the gap).
            if (m.firstIndex != log.values.size() || log.values.size() + m.values.size() > 200000) break;
            log.values.insert(log.values.end(), m.values.begin(), m.values.end());
            if (m.signedCount <= log.values.size()) {
                log.signedCount = m.signedCount;
                log.signature = m.signature;
                log.haveStatement = true;
            }
            if (m.final) logFinal_[p.id] = true;
            break;
        }
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

// May this address open one more connection? Counts the attempt.
bool Server::admit(const std::string& host) {
    if (peers_.size() >= kMaxConnections || blocked(host)) return false;
    int same = 0;
    for (const auto& q : peers_) same += !q->gone && q->host == host ? 1 : 0;
    if (same >= config_.maxPerAddress) return false;
    std::deque<std::uint64_t>& recent = connects_[host];
    recent.push_back(now_);
    while (!recent.empty() && now_ - recent.front() > 10000) recent.pop_front();
    if (recent.size() > kConnectsPerTenSeconds) {
        block(host, now_ + 60000);
        log("WARN  Too many connections from " + host + ": blocked for a minute");
        return false;
    }
    return true;
}

// What a client at this address must say back before the server does any work for it: shows
// that the address is its own. Changes every ten seconds; the one before is still taken.
Key Server::cookieFor(const Address& from, std::uint64_t period) const {
    const std::string text = from.text() + "/" + std::to_string(period);
    return hmacSha256(cookieSecret_, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Server::handleKey(Peer* viaTcp, const Address& from, const std::vector<std::uint8_t>& data) {
    ByteReader r(data);
    r.u32();
    r.u8();
    const std::uint16_t version = r.u16();
    Key theirs{};
    for (std::uint8_t& b : theirs) b = r.u8();
    std::array<std::uint8_t, kCookieBytes> cookie{};
    for (std::uint8_t& b : cookie) b = r.u8();
    if (!r.ok()) {
        if (viaTcp != nullptr) viaTcp->gone = true;  // not one of ours
        return;
    }
    auto answer = [&](const Bytes& packet) {
        if (viaTcp != nullptr) viaTcp->socket.send(kPacketFrame, packet), viaTcp->socket.pump();
        else udp_.sendTo(from, packet);
    };
    Peer* p = viaTcp;
    if (p == nullptr) {
        // The same key from the same place again: our answer was lost. Say it again.
        for (auto& q : peers_)
            if (!q->viaTcp && !q->gone && q->keyed && q->clientPublic == theirs && q->address == from) {
                udp_.sendTo(from, q->keyAnswer);
                return;
            }
        const std::string host = hostOf(from);
        if (blocked(host)) return;
        if (now_ - cookieWindowAt_ >= 1000) cookieWindowAt_ = now_, cookiesSent_ = 0;
        const std::uint64_t period = now_ / 10000;
        const Key now = cookieFor(from, period), before = cookieFor(from, period - 1);
        if (!equalConstantTime(cookie.data(), now.data(), kCookieBytes) && !equalConstantTime(cookie.data(), before.data(), kCookieBytes)) {
            if (++cookiesSent_ > 200) return;  // a flood gets no more answers this second
            ByteWriter w;
            w.u32(kUdpMagic);
            w.u8(kCookie);
            for (std::size_t i = 0; i < kCookieBytes; ++i) w.u8(now[i]);
            udp_.sendTo(from, w.data());
            return;
        }
    } else if (p->keyed) {
        return;
    }
    if (version != kProtocolVersion) {
        ByteWriter w;
        w.u32(kUdpMagic);
        w.u8(kClearReject);
        w.text("Different game version (server protocol " + std::to_string(kProtocolVersion) + ", yours " + std::to_string(version) + ")");
        answer(w.data());
        if (viaTcp != nullptr) viaTcp->gone = true;
        return;
    }
    if (p == nullptr) {
        const std::string host = hostOf(from);
        if (!admit(host)) return;
        auto fresh = std::make_unique<Peer>();
        fresh->host = host;
        fresh->address = from;
        fresh->connectedAt = now_;
        p = fresh.get();
        peers_.push_back(std::move(fresh));
    }
    // Our fresh key goes back in the clear with the connection's number; from then on
    // everything in both directions is encrypted.
    Key secret{};
    randomBytes(secret.data(), secret.size());
    const Key ours = x25519Base(secret);
    if (!deriveSession(secret, theirs, theirs, ours, x25519(identitySecret_, theirs), identityPublic_, p->keys)) {
        p->gone = true;
        return;
    }
    for (bool unique = false; !unique;) {
        std::uint8_t raw[4];
        randomBytes(raw, 4);
        p->token = static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) | (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24);
        unique = true;
        for (auto* list : {&peers_, &closing_})
            for (auto& q : *list) unique = unique && (q.get() == p || q->token != p->token);
    }
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(kServerKey);
    for (std::uint8_t b : ours) w.u8(b);
    for (std::uint8_t b : identityPublic_) w.u8(b);
    w.u32(p->token);
    p->clientPublic = theirs;
    p->keyAnswer = w.data();
    p->link.start(p->keys.udpToClient, p->keys.udpToServer, p->token, now_);
    p->keyed = true;
    answer(p->keyAnswer);
    // Who answers for this server, for the client to check against the keys it trusts.
    if (!config_.certificate.empty()) {
        ByteWriter cert;
        cert.bytes(Bytes(config_.certificate.begin(), config_.certificate.end()));
        send(*p, ServerMsg::Certificate, cert.data());
    }
}

void Server::handleDatagram(const Address& from, const std::vector<std::uint8_t>& data, bool viaDiscovery) {
    // A connection's packet: sealed with that connection's key and numbered. Whatever does
    // not open, or repeats an old number, is dropped without an answer.
    std::uint32_t session = 0;
    std::uint64_t counter = 0;
    if (peekSealed(data, &session, &counter)) {
        if (viaDiscovery) return;
        for (auto* list : {&peers_, &closing_})
            for (auto& p : *list) {
                if (!p->keyed || p->viaTcp || p->token != session) continue;
                // A packet that opens may come from a new address (a changed NAT mapping): only the key holder can cause this.
                if (p->link.receive(data, now_)) p->address = from;
                return;
            }
        return;
    }
    if (!viaDiscovery && data.size() >= 5 && data[4] == kClientKey) {
        ByteReader magic(data);
        if (magic.u32() == kUdpMagic) handleKey(nullptr, from, data);
        return;
    }
    // In the clear: only a search for servers.
    ByteReader r(data);
    if (r.u32() != kUdpMagic || static_cast<UdpMsg>(r.u8()) != UdpMsg::Query || !r.ok() || !config_.discoverable) return;
    if (now_ - infoWindowAt_ >= 1000) infoWindowAt_ = now_, infoSent_ = 0;
    if (++infoSent_ > 30) return;  // a flood of searches gets no more answers this second
    ServerInfo info;
    info.port = listener_.port();
    info.name = config_.name;
    info.players = players();
    info.phase = phase_;
    info.password = !config_.password.empty();
    (viaDiscovery ? discovery_ : udp_).sendTo(from, datagram(UdpMsg::Info, info));
}

// The relay, for players who cannot reach this machine directly.
void Server::pumpRelay() {
    if (config_.relay.empty()) return;
    if (!relayResolved_) {
        if (now_ < relayRetryAt_) return;
        relayRetryAt_ = now_ + 30000;
        const auto at = resolveHostPort(config_.relay, kRelayPort);  // a name lookup: may take a moment
        if (!at) {
            log("WARN  Cannot find the relay " + config_.relay);
            return;
        }
        relayAddress_ = *at;
        relayResolved_ = true;
        relayRetryAt_ = 0;
    }
    if (!relayControl_.open()) {
        if (now_ < relayRetryAt_) return;
        relayRetryAt_ = now_ + 10000;
        relayHelloSent_ = false;
        relayCode_.clear();
        relayControl_.connect(relayAddress_);
        return;
    }
    const bool alive = relayControl_.pump();
    if (relayControl_.connected() && !relayHelloSent_) {
        ByteWriter w;
        w.u32(kRelayMagic);
        relayControl_.send(static_cast<std::uint8_t>(RelayMsg::Host), w.data());
        relayHelloSent_ = true;
        relayPingAt_ = now_;
    }
    if (relayHelloSent_ && now_ - relayPingAt_ >= 15000) {
        relayPingAt_ = now_;
        relayControl_.send(static_cast<std::uint8_t>(RelayMsg::Ping), {});
    }
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
    while (relayControl_.receive(type, payload)) {
        ByteReader r(payload);
        if (static_cast<RelayMsg>(type) == RelayMsg::Hosted) {
            relayCode_ = cleanText(r.text(16), 16);
            relayNotice_ = "Players who cannot reach you directly join with the address " + relayCode_ + "@" + config_.relay;
            log("INFO  Relay " + config_.relay + " code=" + relayCode_);
            if (Peer* a = admin()) tell(*a, relayNotice_);
        } else if (static_cast<RelayMsg>(type) == RelayMsg::Incoming) {
            Adopting a;
            a.ticket = r.u32();
            a.host = cleanText(r.text(64), 64);
            a.since = now_;
            if (!r.ok() || blocked(a.host) || adopting_.size() >= 16 || peers_.size() >= kMaxConnections) continue;
            if (a.socket.connect(relayAddress_)) adopting_.push_back(std::move(a));
        } else if (static_cast<RelayMsg>(type) == RelayMsg::Refuse) {
            log("WARN  Relay: " + cleanText(r.text(256), 200));
        }
    }
    if (!alive) {
        if (!relayCode_.empty()) log("WARN  The connection to the relay was lost; trying again");
        relayControl_.close();
        relayCode_.clear();
        relayNotice_.clear();
    }
    // A connection out to the relay for each announced player: once it is linked it is a
    // player's connection like any other (the player's key exchange comes through it next).
    for (std::size_t i = 0; i < adopting_.size();) {
        Adopting& a = adopting_[i];
        const bool up = a.socket.pump();
        if (up && a.socket.connected()) {
            ByteWriter w;
            w.u32(kRelayMagic);
            w.u32(a.ticket);
            a.socket.send(static_cast<std::uint8_t>(RelayMsg::Accept), w.data());
            a.socket.pump();
            auto p = std::make_unique<Peer>();
            p->host = a.host;
            p->viaTcp = true;
            p->address = relayAddress_;
            p->socket = std::move(a.socket);
            p->connectedAt = now_;
            peers_.push_back(std::move(p));
            adopting_.erase(adopting_.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (!up || now_ - a.since > 5000) {
            adopting_.erase(adopting_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

std::string Server::startMatch() {
    if (seeding_.active) return {};  // a round is already being set up
    // Everybody who will play has to have said they are ready. The administrator says it by starting.
    std::string waiting;
    const Peer* a = admin();
    for (const auto& p : peers_)
        if (p->joined && !p->gone && p->seated() && p.get() != a && !p->ready) waiting += (waiting.empty() ? "" : ", ") + p->name;
    if (!waiting.empty()) return "Not ready yet: " + waiting;
    int occupied = 0;
    std::array<int, 2> perTeam{};
    for (int s = 0; s < kMaxPlayers; ++s)
        if (seatKind_[static_cast<std::size_t>(s)] != SeatKind::Empty) {
            ++occupied;
            ++perTeam[static_cast<std::size_t>(teams_[static_cast<std::size_t>(s)])];
        }
    if (settings_.campaign > 0) {
        // A campaign: stages against enemies, played together. One human is enough.
        int humans = 0;
        for (SeatKind k : seatKind_) humans += k == SeatKind::Human ? 1 : 0;
        if (humans < 1) return "A campaign needs at least one player!";
        const auto loaded = loadCampaignFile(campaigns_[static_cast<std::size_t>(settings_.campaign - 1)].second);
        if (!loaded || loaded->empty()) return "This campaign file has no stages!";
        stages_ = *loaded;
        stageIndex_ = 0;
        campaignMode_ = true;
        score_ = {};
        ai_.clear();
        for (int i = 0; i < kMaxPlayers; ++i) ai_.emplace_back(static_cast<std::uint32_t>(rng_.next()) * 31u + static_cast<std::uint32_t>(i) * 977u + 5u);
        log("INFO  Campaign started name=\"" + settings_.campaignTitle + "\" stages=" + std::to_string(stages_.size()));
        startRound();
        return {};
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
    // The roulette: the winner of the last match (every member of the winning team) gets one
    // more of a powerup in every round of this one. Nobody watches the wheel over the network:
    // the server spins it, as the game does for itself when no human is there to press the key.
    prizeType_ = -1;
    log("INFO  Match started level=" + std::to_string(level_) + " scheme=\"" + settings_.schemeTitle + "\" players=" + std::to_string(occupied));
    if (settings_.goldman && lastWinner_ >= 0) {
        RouletteMsg show;
        show.seed = (static_cast<std::uint32_t>(rng_.next()) << 16) ^ static_cast<std::uint32_t>(rng_.next());
        Roulette wheel(values_, show.seed);
        int frame = 0;
        for (; frame < 5000 && wheel.prize() < 0; ++frame) {
            wheel.step();
            if (frame == 50 || wheel.state() == Roulette::State::Stopped) wheel.press();  // the key is pressed after two seconds
        }
        prizeType_ = wheel.prize();
        prizeWinner_ = lastWinner_;
        static const char* const kPrizeText[kPowTypeCount] = {
            "an extra bomb", "longer flame length", "a disease", "the ability to kick bombs", "extra speed",
            "the ability to punch bombs", "the ability to grab bombs", "the spooger", "goldflame", "a trigger mechanism",
            "jelly (bouncy) bombs", "super bad disease", "random", "a speed brake (slowness)", ""};
        if (prizeType_ >= 0) {
            const std::string who = settings_.teamPlay ? "team " + std::to_string(prizeWinner_ + 1) : lobby_.seatName(prizeWinner_);
            rouletteSay_ = "The Gold Player (" + who + ") has " + kPrizeText[prizeType_] + " for this match!!";
            // Everybody watches the wheel (25 frames a second) and then the prize for a few seconds.
            show.winner = prizeWinner_;
            show.teamPlay = settings_.teamPlay;
            show.prize = prizeType_;
            show.frames = frame;
            broadcast(ServerMsg::Roulette, encoded(show));
            phase_ = Phase::Roulette;
            rouletteUntil_ = now_ + static_cast<std::uint64_t>(frame) * 40 + 4000;
            lobbyDirty_ = true;
            return {};
        }
    }
    startRound();
    return {};
}

// A round begins. If it is to be recorded, its seed is first agreed on: the server and
// every player promise a random number, then show it (stepSeeding() goes on from here).
void Server::startRound() {
    sealRecord();  // the round before, if its record is still open
    if (!config_.signRecord) {
        beginRound((static_cast<std::uint32_t>(rng_.next()) << 16) ^ static_cast<std::uint32_t>(rng_.next()), {});
        return;
    }
    seeding_ = Seeding{};
    seeding_.active = true;
    seeding_.stage = 1;
    seeding_.deadline = now_ + kSeedStageMs;
    seeding_.roundId = roundId_ + 1;
    randomBytes(seeding_.serverReveal.data(), seeding_.serverReveal.size());
    seeding_.shares.push_back({kServerSender, commitOf(seeding_.serverReveal), {}, false});
    ByteWriter w;
    w.u32(seeding_.roundId);
    for (auto& p : peers_)
        if (p->joined && !p->gone && p->playing()) {
            seeding_.asked.push_back(p->id);
            send(*p, ServerMsg::SeedAsk, w.data());
        }
    if (seeding_.asked.empty()) stepSeeding();  // nobody to wait for
}

void Server::stepSeeding() {
    auto present = [&](std::uint8_t id) { return peerById(id) != nullptr; };
    if (seeding_.stage == 1) {
        bool all = true;
        for (std::uint8_t id : seeding_.asked) {
            bool had = false;
            for (const SeedShare& s : seeding_.shares) had = had || s.client == id;
            all = all && (had || !present(id));
        }
        if (!all && now_ < seeding_.deadline) return;
        // Everybody's promise is told to everybody (so nobody's can be changed afterwards), the server's among them.
        SeedCommitsMsg m;
        m.roundId = seeding_.roundId;
        for (const SeedShare& s : seeding_.shares) m.commits.emplace_back(s.client, s.commit);
        const Bytes bytes = encoded(m);
        for (const SeedShare& s : seeding_.shares)
            if (Peer* p = s.client != kServerSender ? peerById(s.client) : nullptr) send(*p, ServerMsg::SeedCommits, bytes);
        seeding_.stage = 2;
        seeding_.deadline = now_ + kSeedStageMs;
    }
    bool all = true;
    for (const SeedShare& s : seeding_.shares)
        if (s.client != kServerSender) all = all && (s.revealed || !present(s.client));
    if (!all && now_ < seeding_.deadline) return;
    // The server's own number is shown only in the record: until the round is over the seed stays its secret.
    bool anyone = false;
    for (auto& p : peers_) anyone = anyone || (p->joined && !p->gone && p->playing());
    if (!anyone) {
        // Everybody left while the round was being set up: there is no round.
        seeding_ = Seeding{};
        if (phase_ != Phase::Lobby) toLobby();
        return;
    }
    seeding_.shares[0].reveal = seeding_.serverReveal;
    seeding_.shares[0].revealed = true;
    const std::vector<SeedShare> shares = seeding_.shares;
    const std::uint32_t seed = seedOf(seeding_.roundId, shares);
    seeding_ = Seeding{};
    beginRound(seed, shares);
}

void Server::beginRound(std::uint32_t roundSeed, const std::vector<SeedShare>& shares) {
    ++roundId_;
    RoundStartMsg start;
    start.roundId = roundId_;
    start.seed = roundSeed;
    start.recorded = !shares.empty();
    if (start.recorded) start.recordId = recordIdOf(roundId_, shares);
    start.values = values_.entries();
    start.winsNeeded = settings_.winsNeeded;
    start.score = score_;
    RoundSetup& setup = start.setup;
    roundKind_ = seatKind_;
    SchemeFile arena = schemeFile_;
    arena.scheme.start = startCells_;
    if (campaignMode_) {
        // The stage chooses level, arena, enemies and how many computer players join in.
        const CampaignStage& st = stages_[static_cast<std::size_t>(stageIndex_)];
        level_ = std::clamp(st.level, 0, 10);
        for (const SchemeEntry& entry : schemes_)
            if (entry.file == st.scheme)
                if (auto file = loadSchemeFile(entry.path)) arena = *file;
        int computers = st.computerPlayers;
        for (int s = 0; s < kMaxPlayers; ++s) {
            SeatKind& kind = roundKind_[static_cast<std::size_t>(s)];
            if (kind == SeatKind::Computer) kind = SeatKind::Empty;
            if (kind == SeatKind::Empty && computers > 0) kind = SeatKind::Computer, --computers;
        }
        setup.campaign = true;
        setup.ghosts = std::clamp(st.ghosts, 0, 100), setup.ghostSpeed = std::clamp(st.ghostSpeed, 0, 5000);
        setup.rovers = std::clamp(st.rovers, 0, 100), setup.roverSpeed = std::clamp(st.roverSpeed, 0, 5000);
        start.stageName = st.name;
        start.stage = stageIndex_ + 1;
        start.stages = static_cast<int>(stages_.size());
    }
    setup.level = level_;
    setup.scheme = arena.scheme;
    setup.powers = arena.powers;
    if (!config_.gameDir.empty()) setup.extras = loadExtrasFile(config_.gameDir + "/data/res/extra" + std::to_string(level_) + ".res");
    setup.conveyorSpeed = settings_.conveyorSpeed;
    setup.teamPlay = settings_.teamPlay && !campaignMode_;
    setup.teams = teams_;
    setup.enclosementDepth = settings_.enclosementDepth;
    setup.stompedBombsDetonate = settings_.stompedBombsDetonate;
    setup.diseasesDestroyable = settings_.diseasesDestroyable;
    setup.playTime = settings_.playTime;
    setup.winByKills = settings_.winByKills;
    for (int s = 0; s < kMaxPlayers; ++s) {
        setup.present[static_cast<std::size_t>(s)] = roundKind_[static_cast<std::size_t>(s)] != SeatKind::Empty;
        setup.human[static_cast<std::size_t>(s)] = roundKind_[static_cast<std::size_t>(s)] == SeatKind::Human;
        if (!campaignMode_ && prizeType_ >= 0 && setup.present[static_cast<std::size_t>(s)] &&
            (settings_.teamPlay ? teams_[static_cast<std::size_t>(s)] : s) == prizeWinner_)
            setup.prize[static_cast<std::size_t>(s)] = prizeType_;
    }
    const std::uint32_t seed = start.seed;
    start.seed = 0;  // the clients are not told: they get the round's start as a snapshot without its secrets
    world_ = std::make_unique<World>(values_, seed);
    applyRoundSetup(*world_, values_, setup);
    world_->takeEvents();
    history_.clear();
    states_.clear();
    stepEvents_.clear();
    states_.push_back({0, world_->snapshotFields()});
    // What the record of this round starts from.
    roundShares_ = shares;
    recordStart_ = start;
    recordStart_.seed = seed;
    recordStartedUnix_ = static_cast<std::uint64_t>(std::time(nullptr));
    logs_.clear();
    logFinal_.clear();
    computerMask_.clear();
    recordHashes_.clear();
    recordPending_ = false;
    for (int s = 0; s < kMaxPlayers; ++s) {
        RecordSeat& seat = recordSeats_[static_cast<std::size_t>(s)];
        seat = RecordSeat{};
        seat.kind = roundKind_[static_cast<std::size_t>(s)];
        int local = 0;
        if (const Peer* p = seat.kind == SeatKind::Human ? peerAtSeat(s, &local) : nullptr) {
            seat.client = p->id;
            seat.local = static_cast<std::uint8_t>(local);
            PlayerLog& log = logs_[p->id];
            log.client = p->id;
            log.name = p->name;
            log.playerKey = p->playerKey;
        } else if (seat.kind == SeatKind::Human) {
            seat.kind = SeatKind::Computer;  // nobody sits there any more
        }
    }
    decided_ = false;
    winner_ = -1;
    afterSteps_ = 0;
    roundStartAt_ = now_ + kRoundLeadMs;
    roundStart_ = encoded(start);
    roundEnd_.clear();
    for (auto& p : peers_) {
        p->ackRound = roundId_;
        p->haveState = false;
        p->ackStep = 0;
        p->input = {};
        p->inputStep = 0;
        p->inputs.clear();
        p->wantsContinue = false;
    }
    phase_ = Phase::Round;
    lobbyDirty_ = true;
    broadcast(ServerMsg::RoundStart, roundStart_);
    for (auto& p : peers_)
        if (p->joined && !p->gone && !p->out) sendSnapshot(*p);  // where the round begins, as far as a player may know
    if (campaignMode_) say("Stage " + std::to_string(start.stage) + " of " + std::to_string(start.stages) + ": " + start.stageName);
    log("INFO  Round " + std::to_string(roundId_) + " started seed=" + std::to_string(seed) + (campaignMode_ ? " stage=\"" + start.stageName + "\"" : ""));
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
        // Each player's input meant for this step if it has come, otherwise what they held last.
        const auto step = static_cast<std::uint32_t>(history_.size()) + 1;
        for (auto& p : peers_) {
            if (const auto it = p->inputs.find(step); it != p->inputs.end()) p->input = it->second, p->inputStep = step;
            p->inputs.erase(p->inputs.begin(), p->inputs.upper_bound(step));
        }
        StepInputs bytes{};
        std::array<PlayerInput, kMaxPlayers> input{};
        std::uint16_t computer = 0;  // the seats the computer plays in this step (for the record)
        for (int s = 0; s < kMaxPlayers; ++s) {
            const auto i = static_cast<std::size_t>(s);
            if (!world_->player(s).present) continue;
            int local = 0;
            const Peer* human = roundKind_[i] == SeatKind::Human ? peerAtSeat(s, &local) : nullptr;
            if (human != nullptr && human->out) human = nullptr;  // back in the lobby: the computer plays the seat
            if (human == nullptr) computer = static_cast<std::uint16_t>(computer | (1u << s));
            bytes[i] = human != nullptr ? human->input[static_cast<std::size_t>(local)] : packInput(ai_[i].decide(*world_, s, kStepMs));
            input[i] = unpackInput(bytes[i]);
        }
        world_->tick(kStepMs, input);
        history_.push_back(bytes);
        computerMask_.push_back(computer);
        if (!roundShares_.empty() && history_.size() % kRecordHashEvery == 0) recordHashes_.push_back(world_->stateHash(false));
        // What clients are told: the public state after the step, and what happened in it.
        for (const Event& e : world_->takeEvents()) stepEvents_.push_back({step, e});
        states_.push_back({step, world_->snapshotFields()});
        while (states_.size() > kStatesKept) states_.pop_front();
        while (!stepEvents_.empty() && stepEvents_.front().step + kStatesKept <= step) stepEvents_.pop_front();
        ++ran;
        if (world_->roundOver()) {
            if (!decided_) {
                decided_ = true;
                winner_ = campaignMode_ ? -1 : score_.roundDecided(*world_, settings_.winsNeeded, settings_.winByKills);
            }
            if (++afterSteps_ > kStepsAfterDecision) endRound();
        }
    }
    if (ran > 0)
        for (auto& p : peers_)
            if (p->joined && !p->gone && !p->out) sendState(*p);
}

// The newest state, as its difference from the newest one the client has said it holds.
void Server::sendState(Peer& p) {
    if (!world_ || states_.empty()) return;
    const StateAt& now = states_.back();
    if (p.ackRound != roundId_ || !p.haveState) {
        // It holds nothing of this round yet: the whole state is on its way as a message.
        // Said again only if that has plainly not worked.
        if (now_ - p.lastSnapshot > 2000 && p.link.unacknowledged() == 0) sendSnapshot(p);
        return;
    }
    if (p.ackStep >= now.step) return;
    const Fields* base = nullptr;
    for (const StateAt& s : states_)
        if (s.step == p.ackStep) base = &s.fields;
    if (base == nullptr) {
        // Too far behind for a difference: the whole state again (not while one is still under way).
        if (now_ - p.lastSnapshot > 500 && p.link.unacknowledged() == 0) sendSnapshot(p);
        return;
    }
    DeltaMsg m;
    m.roundId = roundId_;
    m.step = now.step;
    m.baseStep = p.ackStep;
    for (const StepEvent& e : stepEvents_)
        if (e.step > p.ackStep) m.events.push_back(e);
    m.delta = encodeDelta(*base, now.fields);
    const Bytes bytes = encoded(m);
    if (p.link.sendDatagram(static_cast<std::uint8_t>(ServerMsg::Delta), bytes)) return;
    // Too large for a datagram: as a message, unless earlier ones are still under way.
    if (p.link.unacknowledged() < 4) send(p, ServerMsg::Delta, bytes);
}

void Server::sendSnapshot(Peer& p) {
    if (!world_ || states_.empty()) return;
    SnapshotMsg m;
    m.roundId = roundId_;
    m.step = states_.back().step;
    m.state = encodeDelta({}, states_.back().fields);  // without the hidden powerups and the random generator
    send(p, ServerMsg::Snapshot, encoded(m));
    p.lastSnapshot = now_;
}

void Server::endRound() {
    RoundEndMsg end;
    end.roundId = roundId_;
    end.steps = static_cast<std::uint32_t>(history_.size());
    end.winner = winner_;
    end.teamPlay = settings_.teamPlay;
    end.score = score_;
    if (campaignMode_) {
        // Cleared: on to the next stage. Failed by the clock: also on. Failed because no
        // human was left: the same stage again (as the original's campaign code does).
        end.campaign = world_->campaignResult() == 1 ? 1 : 2;
        campaignNext_ = stageIndex_ + (end.campaign == 2 && world_->campaignRetry() ? 0 : 1);
        end.campaignOver = campaignNext_ >= static_cast<int>(stages_.size());
    }
    roundEnd_ = encoded(end);
    broadcast(ServerMsg::RoundEnd, roundEnd_);
    // The record is sealed a moment later, when the players' last statements are in.
    recordEnd_ = end;
    recordPending_ = !roundShares_.empty();
    recordDueAt_ = now_ + kRecordWaitMs;
    phase_ = Phase::Result;
    resultAt_ = now_;
    lobbyDirty_ = true;
    const std::string who = campaignMode_ ? (end.campaign == 1 ? "stage cleared" : "stage failed") : winner_ < 0 ? "draw" : settings_.teamPlay ? "team " + std::to_string(winner_ + 1) : lobby_.seatName(winner_);
    log("INFO  Round " + std::to_string(roundId_) + " over steps=" + std::to_string(end.steps) + " winner=" + who +
        (score_.matchWinner >= 0 ? " (match decided)" : ""));
}

// The record of the round just played: put together, signed, kept, and handed to everybody who was in it.
void Server::sealRecord() {
    if (!recordPending_) return;
    recordPending_ = false;
    if (!config_.signRecord || !world_) return;
    MatchRecord record;
    record.gameVersion = config_.gameVersion;
    record.serverName = config_.name;
    record.startedUnix = recordStartedUnix_;
    record.start = recordStart_;
    record.shares = roundShares_;
    record.seats = recordSeats_;
    for (const auto& [id, log] : logs_) record.players.push_back(log);
    record.inputs = history_;
    record.computer = computerMask_;
    record.hashEvery = kRecordHashEvery;
    record.hashes = recordHashes_;
    record.finalHash = world_->stateHash(false);
    record.end = recordEnd_;
    record.certificate = config_.certificate;
    const Bytes bytes = encodeRecord(record);
    const std::string document = config_.signRecord(bytes);
    if (document.empty()) {
        log("WARN  The record of round " + std::to_string(roundId_) + " could not be signed");
        return;
    }
    if (config_.recordSink) config_.recordSink(document, record);
    ByteWriter w;
    w.u32(roundId_);
    w.bytes(Bytes(document.begin(), document.end()));
    for (auto& p : peers_)
        if (p->joined && !p->gone && !p->out) send(*p, ServerMsg::Record, w.data());
    log("INFO  Record of round " + std::to_string(roundId_) + " sealed bytes=" + std::to_string(bytes.size()) + " players=" + std::to_string(record.players.size()));
}

void Server::toLobby() {
    sealRecord();
    seeding_ = Seeding{};
    phase_ = Phase::Lobby;
    for (auto& p : peers_) p->out = false, p->ready = false;
    campaignMode_ = false;
    world_.reset();
    history_.clear();
    states_.clear();
    stepEvents_.clear();
    rebuildSeats();
    log("INFO  Back in the lobby");
}

void Server::update(std::uint64_t nowMs) {
    if (!running_) return;
    now_ = nowMs;

    while (auto socket = listener_.accept()) {
        const std::string host = hostOf(socket->peer());
        if (!admit(host)) continue;  // dropped: the socket closes here
        auto p = std::make_unique<Peer>();
        p->host = host;
        p->viaTcp = true;
        p->address = socket->peer();
        p->socket = std::move(*socket);
        p->connectedAt = now_;
        peers_.push_back(std::move(p));
    }
    pumpRelay();
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
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        Peer& p = *peers_[i];
        if (p.gone) continue;
        if (p.viaTcp) {
            // Inside a TCP connection each frame is one packet.
            const bool alive = p.socket.pump();
            std::uint8_t type = 0;
            Bytes payload;
            while (!p.gone && p.socket.receive(type, payload)) {
                if (type != kPacketFrame || payload.size() < 5) {
                    p.gone = true;  // not one of ours
                } else if (payload[4] == kClientKey) {
                    handleKey(&p, p.address, payload);
                } else if (p.keyed) {
                    p.link.receive(payload, now_);
                }
            }
            if (!alive) p.gone = true;
        }
        std::uint8_t type = 0;
        Bytes payload;
        while (!p.gone && p.link.nextMessage(type, payload)) handleFrame(p, type, payload);
        while (!p.gone && p.link.nextDatagram(type, payload)) {
            if (static_cast<ClientMsg>(type) != ClientMsg::Input || !p.joined) continue;
            ByteReader r(payload);
            InputMsg m;
            if (decode(r, m)) applyInput(p, m);
        }
        if (p.gone) continue;
        if (p.link.closedByPeer() || p.link.broken()) p.gone = true;
        if (!p.joined && now_ - p.connectedAt > kHelloTimeoutMs) p.gone = true;
        if (p.joined && !p.gone && now_ - p.link.lastHeard() > kSilenceTimeoutMs) {
            log("INFO  Timed out id=" + std::to_string(p.id));
            reject(p, "Connection timed out");
        }
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
        // What it was last told (why it has to go, for one) is still delivered.
        if (p->keyed && !p->link.closedByPeer() && (!p->viaTcp || p->socket.open())) {
            p->link.close();
            p->closeUntil = now_ + 2000;
            closing_.push_back(std::move(p));
        }
    }

    // A finished round's record waits a moment for the players' last statements.
    if (recordPending_) {
        bool all = true;
        for (const auto& [id, log] : logs_) {
            const Peer* p = peerById(id);
            all = all && (p == nullptr || p->out || logFinal_.count(id) != 0 || log.playerKey == Key{});
        }
        if (all || now_ >= recordDueAt_) sealRecord();
    }
    if (seeding_.active) {
        stepSeeding();
    } else if (phase_ == Phase::Round) {
        stepRound();
    } else if (phase_ == Phase::Roulette) {
        if (now_ >= rouletteUntil_) {
            say(rouletteSay_);
            startRound();
        }
    } else if (phase_ == Phase::Result) {
        // Clients may still miss the last states.
        if (now_ - resendAt_ >= 100) {
            resendAt_ = now_;
            for (auto& p : peers_)
                if (p->joined && !p->gone && !p->out && (!p->haveState || p->ackRound != roundId_ || p->ackStep < history_.size())) sendState(*p);
        }
        bool everyone = true, anyone = false;
        for (auto& p : peers_)
            if (p->joined && p->playing()) {
                anyone = true;
                everyone = everyone && p->wantsContinue;
            }
        const std::uint64_t waited = now_ - resultAt_;
        if (!anyone) {
            toLobby();
        } else if ((everyone && waited >= kResultMinMs) || waited >= kResultMaxMs) {
            if (campaignMode_) {
                if (campaignNext_ >= static_cast<int>(stages_.size())) {
                    toLobby();
                    say("Congratulations! You made it through the whole campaign!");
                } else {
                    stageIndex_ = campaignNext_;
                    startRound();
                }
            } else if (score_.matchWinner >= 0) {
                lastWinner_ = settings_.goldman ? score_.matchWinner : -1;  // remembered for the roulette
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
    // Out with everything that is due.
    for (auto* list : {&peers_, &closing_})
        for (auto& p : *list) {
            if (p->gone && list == &peers_) continue;
            if (p->keyed) p->link.update(now_, [&](const Bytes& packet) { transmit(*p, packet); });
            if (p->viaTcp && !p->socket.pump() && list == &peers_) p->gone = true;
        }
    std::erase_if(closing_, [&](const std::unique_ptr<Peer>& p) { return p->link.idle() || now_ >= p->closeUntil || (p->viaTcp && !p->socket.open()); });
}

}  // namespace ab::net
