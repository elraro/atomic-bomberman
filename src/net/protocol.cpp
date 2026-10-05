#include "net/protocol.hpp"

#include <algorithm>

namespace ab::net {

void ByteWriter::u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
}

void ByteWriter::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
}

void ByteWriter::text(const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
    u16(static_cast<std::uint16_t>(n));
    out_.insert(out_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void ByteWriter::bytes(const std::vector<std::uint8_t>& data) {
    u32(static_cast<std::uint32_t>(data.size()));
    out_.insert(out_.end(), data.begin(), data.end());
}

std::uint8_t ByteReader::u8() {
    if (pos_ >= data_.size()) {
        ok_ = false;
        return 0;
    }
    return data_[pos_++];
}

std::uint16_t ByteReader::u16() {
    const std::uint16_t lo = u8();
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(u8()) << 8));
}

std::uint32_t ByteReader::u32() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i);
    return v;
}

std::string ByteReader::text(std::size_t maxLength) {
    const std::size_t n = u16();
    if (!ok_ || n > maxLength || data_.size() - pos_ < n) {
        ok_ = false;
        return {};
    }
    std::string s(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return s;
}

std::vector<std::uint8_t> ByteReader::bytes(std::size_t maxLength) {
    const std::size_t n = u32();
    if (!ok_ || n > maxLength || data_.size() - pos_ < n) {
        ok_ = false;
        return {};
    }
    std::vector<std::uint8_t> out(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return out;
}

std::uint8_t packInput(const PlayerInput& in) {
    std::uint8_t bits = 0;
    for (std::size_t d = 0; d < 4; ++d)
        if (in.dir[d]) bits = static_cast<std::uint8_t>(bits | (1u << d));
    if (in.button1) bits |= 16;
    if (in.button2) bits |= 32;
    return bits;
}

PlayerInput unpackInput(std::uint8_t bits) {
    PlayerInput in;
    for (std::size_t d = 0; d < 4; ++d) in.dir[d] = (bits & (1u << d)) != 0;
    in.button1 = (bits & 16) != 0;
    in.button2 = (bits & 32) != 0;
    return in;
}

const ClientInfo* LobbyState::client(std::uint8_t id) const {
    for (const ClientInfo& c : clients)
        if (c.id == id) return &c;
    return nullptr;
}

std::string LobbyState::seatName(int seat) const {
    if (seat < 0 || seat >= kMaxPlayers) return {};
    const Seat& s = seats[static_cast<std::size_t>(seat)];
    if (s.kind == SeatKind::Computer) return "Computer";
    if (s.kind == SeatKind::Human)
        if (const ClientInfo* c = client(s.client)) return s.local == 0 ? c->name : c->name + " (" + std::to_string(s.local + 1) + ")";
    return {};
}

std::string cleanText(const std::string& s, std::size_t maxLength) {
    std::string out;
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 32 || u == 127) continue;
        out += ch;
    }
    if (out.size() > maxLength) {
        std::size_t n = maxLength;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 character
        out.resize(n);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

std::vector<std::uint8_t> datagram(UdpMsg type) {
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(static_cast<std::uint8_t>(type));
    return std::move(w.data());
}

namespace {
constexpr std::size_t kSealedHeader = 4 + 1 + 4 + 8;
}

std::vector<std::uint8_t> sealDatagram(std::uint32_t session, std::uint64_t counter, const Key& key, const std::vector<std::uint8_t>& inner) {
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(kSealedDatagram);
    w.u32(session);
    w.u64(counter);
    const std::vector<std::uint8_t> sealed = seal(key, counterNonce(counter), w.data().data(), kSealedHeader, inner.data(), inner.size());
    w.data().insert(w.data().end(), sealed.begin(), sealed.end());
    return std::move(w.data());
}

bool peekSealed(const std::vector<std::uint8_t>& data, std::uint32_t* session, std::uint64_t* counter) {
    if (data.size() < kSealedHeader + 16) return false;
    ByteReader r(data);
    if (r.u32() != kUdpMagic || r.u8() != kSealedDatagram) return false;
    *session = r.u32();
    *counter = r.u64();
    return true;
}

bool openSealed(const std::vector<std::uint8_t>& data, const Key& key, std::vector<std::uint8_t>& inner) {
    std::uint32_t session = 0;
    std::uint64_t counter = 0;
    if (!peekSealed(data, &session, &counter)) return false;
    return open(key, counterNonce(counter), data.data(), kSealedHeader, data.data() + kSealedHeader, data.size() - kSealedHeader, inner);
}

// --- messages ----------------------------------------------------------------

void encode(ByteWriter& w, const HelloMsg& m) {
    w.u32(kTcpMagic);
    w.u16(m.version);
    w.text(m.name);
    for (std::uint8_t b : m.proof) w.u8(b);
}

bool decode(ByteReader& r, HelloMsg& m) {
    if (r.u32() != kTcpMagic) return false;
    m.version = r.u16();
    m.name = r.text(64);
    for (std::uint8_t& b : m.proof) b = r.u8();
    return r.ok();
}

void encode(ByteWriter& w, const WelcomeMsg& m) {
    w.u8(m.id);
    w.u32(m.token);
    w.text(m.serverName);
}

bool decode(ByteReader& r, WelcomeMsg& m) {
    m.id = r.u8();
    m.token = r.u32();
    m.serverName = r.text(64);
    return r.ok();
}

void encode(ByteWriter& w, const ChatMsg& m) {
    w.u8(m.sender);
    w.text(m.name);
    w.text(m.text);
}

bool decode(ByteReader& r, ChatMsg& m) {
    m.sender = r.u8();
    m.name = r.text(64);
    m.text = r.text(512);
    return r.ok();
}

namespace {

void encodeScore(ByteWriter& w, const MatchScore& s) {
    for (int v : s.wins) w.i16(v);
    for (int v : s.kills) w.i16(v);
    w.i8(s.matchWinner);
}

bool decodeScore(ByteReader& r, MatchScore& s) {
    for (int& v : s.wins) v = r.i16();
    for (int& v : s.kills) v = r.i16();
    s.matchWinner = r.i8();
    return r.ok() && s.matchWinner >= -1 && s.matchWinner < kMaxPlayers;
}

}  // namespace

void encode(ByteWriter& w, const LobbyState& m) {
    w.u8(static_cast<std::uint8_t>(m.phase));
    w.u8(m.admin);
    const MatchSettings& s = m.settings;
    w.i8(s.level);
    w.u16(static_cast<std::uint16_t>(s.schemeIndex));
    w.text(s.schemeTitle);
    w.u8(static_cast<std::uint8_t>(s.winsNeeded));
    w.flag(s.teamPlay);
    w.u16(static_cast<std::uint16_t>(s.playTime));
    w.u8(static_cast<std::uint8_t>(s.enclosementDepth));
    w.u8(static_cast<std::uint8_t>(s.computers));
    w.flag(s.randomStart);
    w.u8(static_cast<std::uint8_t>(s.conveyorSpeed));
    w.flag(s.stompedBombsDetonate);
    w.flag(s.winByKills);
    w.flag(s.diseasesDestroyable);
    w.flag(s.goldman);
    w.u8(static_cast<std::uint8_t>(s.campaign));
    w.text(s.campaignTitle);
    for (const Seat& seat : m.seats) {
        w.u8(static_cast<std::uint8_t>(seat.kind));
        w.u8(seat.client);
        w.u8(seat.local);
        w.u8(static_cast<std::uint8_t>(seat.team));
    }
    w.u8(static_cast<std::uint8_t>(m.clients.size()));
    for (const ClientInfo& c : m.clients) {
        w.u8(c.id);
        w.text(c.name);
        w.i8(c.seat);
        w.u8(static_cast<std::uint8_t>(c.players));
        w.u16(static_cast<std::uint16_t>(std::clamp(c.pingMs, 0, 65535)));
    }
}

bool decode(ByteReader& r, LobbyState& m) {
    const int phase = r.u8();
    m.admin = r.u8();
    MatchSettings& s = m.settings;
    s.level = r.i8();
    s.schemeIndex = r.u16();
    s.schemeTitle = r.text(128);
    s.winsNeeded = r.u8();
    s.teamPlay = r.flag();
    s.playTime = r.u16();
    s.enclosementDepth = r.u8();
    s.computers = r.u8();
    s.randomStart = r.flag();
    s.conveyorSpeed = r.u8();
    s.stompedBombsDetonate = r.flag();
    s.winByKills = r.flag();
    s.diseasesDestroyable = r.flag();
    s.goldman = r.flag();
    s.campaign = r.u8();
    s.campaignTitle = r.text(64);
    for (Seat& seat : m.seats) {
        const int kind = r.u8();
        seat.client = r.u8();
        seat.local = r.u8();
        if (seat.local >= kMaxLocalPlayers) return false;
        seat.team = r.u8();
        if (kind > 2 || seat.team > 1) return false;
        seat.kind = static_cast<SeatKind>(kind);
    }
    const int n = r.u8();
    m.clients.clear();
    for (int i = 0; i < n && r.ok(); ++i) {
        ClientInfo c;
        c.id = r.u8();
        c.name = r.text(64);
        c.seat = r.i8();
        c.players = r.u8();
        c.pingMs = r.u16();
        if (c.seat < -1 || c.seat >= kMaxPlayers) return false;
        m.clients.push_back(std::move(c));
    }
    if (!r.ok() || phase > 2 || s.level < -1 || s.level > 10 || s.enclosementDepth > 3 || s.conveyorSpeed > 2) return false;
    m.phase = static_cast<Phase>(phase);
    return true;
}

void encode(ByteWriter& w, const RoundSetup& m) {
    w.u8(static_cast<std::uint8_t>(m.level));
    for (const auto& row : m.scheme.tiles)
        for (Tile t : row) w.u8(static_cast<std::uint8_t>(t));
    w.u8(static_cast<std::uint8_t>(std::clamp(m.scheme.brickDensity, 0, 100)));
    for (const Cell& c : m.scheme.start) {
        w.u8(static_cast<std::uint8_t>(c.x));
        w.u8(static_cast<std::uint8_t>(c.y));
    }
    for (const SchemePower& p : m.powers) {
        w.i16(p.bornWith);
        w.flag(p.hasOverride);
        w.i16(p.overrideValue);
        w.flag(p.forbidden);
    }
    w.u16(static_cast<std::uint16_t>(m.extras.size()));
    for (const Extra& e : m.extras) {
        w.u8(static_cast<std::uint8_t>(e.type));
        w.i8(e.cell.x);
        w.i8(e.cell.y);
        w.i8(e.dir);
        w.i16(e.id);
        w.i16(e.linkTo);
    }
    w.u8(static_cast<std::uint8_t>(m.conveyorSpeed));
    w.flag(m.teamPlay);
    for (int t : m.teams) w.u8(static_cast<std::uint8_t>(t));
    w.u8(static_cast<std::uint8_t>(m.enclosementDepth));
    w.flag(m.stompedBombsDetonate);
    w.flag(m.diseasesDestroyable);
    w.u16(static_cast<std::uint16_t>(m.playTime));
    w.flag(m.winByKills);
    for (int i = 0; i < kMaxPlayers; ++i)
        w.u8(static_cast<std::uint8_t>((m.present[static_cast<std::size_t>(i)] ? 1 : 0) | (m.human[static_cast<std::size_t>(i)] ? 2 : 0)));
    w.flag(m.campaign);
    w.u8(static_cast<std::uint8_t>(m.ghosts));
    w.u16(static_cast<std::uint16_t>(m.ghostSpeed));
    w.u8(static_cast<std::uint8_t>(m.rovers));
    w.u16(static_cast<std::uint16_t>(m.roverSpeed));
    for (int p : m.prize) w.i8(p);
}

bool decode(ByteReader& r, RoundSetup& m) {
    m = RoundSetup{};
    m.level = r.u8();
    for (auto& row : m.scheme.tiles)
        for (Tile& t : row) {
            const int v = r.u8();
            if (v > 2) return false;
            t = static_cast<Tile>(v);
        }
    m.scheme.brickDensity = r.u8();
    for (Cell& c : m.scheme.start) {
        c.x = r.u8();
        c.y = r.u8();
        if (!inGrid(c)) return false;
    }
    for (SchemePower& p : m.powers) {
        p.bornWith = r.i16();
        p.hasOverride = r.flag();
        p.overrideValue = r.i16();
        p.forbidden = r.flag();
        if (p.bornWith < 0 || p.bornWith > 99 || p.overrideValue < -200 || p.overrideValue > 200) return false;
    }
    const int extras = r.u16();
    if (extras > 200) return false;
    for (int i = 0; i < extras && r.ok(); ++i) {
        Extra e;
        const int type = r.u8();
        e.cell.x = r.i8();
        e.cell.y = r.i8();
        e.dir = r.i8();
        e.id = r.i16();
        e.linkTo = r.i16();
        // A cell outside the grid means "place at random"; the core does that.
        if (type > 3 || e.dir < 0 || e.dir > 3) return false;
        e.type = static_cast<ExtraType>(type);
        m.extras.push_back(e);
    }
    m.conveyorSpeed = r.u8();
    m.teamPlay = r.flag();
    for (int& t : m.teams) {
        t = r.u8();
        if (t > 1) return false;
    }
    m.enclosementDepth = r.u8();
    m.stompedBombsDetonate = r.flag();
    m.diseasesDestroyable = r.flag();
    m.playTime = r.u16();
    m.winByKills = r.flag();
    for (int i = 0; i < kMaxPlayers; ++i) {
        const int bits = r.u8();
        m.present[static_cast<std::size_t>(i)] = (bits & 1) != 0;
        m.human[static_cast<std::size_t>(i)] = (bits & 2) != 0;
    }
    m.campaign = r.flag();
    m.ghosts = r.u8();
    m.ghostSpeed = r.u16();
    m.rovers = r.u8();
    m.roverSpeed = r.u16();
    for (int& p : m.prize) {
        p = r.i8();
        if (p < -1 || p >= kPowTypeCount) return false;
    }
    if (m.ghosts > 100 || m.rovers > 100 || m.ghostSpeed > 5000 || m.roverSpeed > 5000) return false;
    return r.ok() && m.level <= 10 && m.scheme.brickDensity <= 100 && m.conveyorSpeed <= 2 && m.enclosementDepth <= 3 &&
           m.playTime >= 1;
}

void encode(ByteWriter& w, const RoundStartMsg& m) {
    w.u32(m.roundId);
    w.u32(m.seed);
    w.u16(static_cast<std::uint16_t>(m.values.size()));
    for (const auto& [id, value] : m.values) {
        w.i32(id);
        w.i32(value);
    }
    encode(w, m.setup);
    w.u8(static_cast<std::uint8_t>(m.winsNeeded));
    encodeScore(w, m.score);
    w.text(m.stageName);
    w.u8(static_cast<std::uint8_t>(m.stage));
    w.u8(static_cast<std::uint8_t>(m.stages));
}

bool decode(ByteReader& r, RoundStartMsg& m) {
    m.roundId = r.u32();
    m.seed = r.u32();
    const int n = r.u16();
    m.values.clear();
    for (int i = 0; i < n && r.ok(); ++i) {
        const int id = r.i32();
        const int value = r.i32();
        m.values.emplace_back(id, value);
    }
    if (!r.ok() || !decode(r, m.setup)) return false;
    m.winsNeeded = r.u8();
    if (!decodeScore(r, m.score)) return false;
    m.stageName = r.text(64);
    m.stage = r.u8();
    m.stages = r.u8();
    return r.ok();
}

void encode(ByteWriter& w, const StepsMsg& m) {
    w.u32(m.roundId);
    w.u32(m.firstStep);
    w.u8(static_cast<std::uint8_t>(m.steps.size()));
    for (const StepInputs& s : m.steps)
        for (std::uint8_t b : s) w.u8(b);
    w.u32(m.hash);
}

bool decode(ByteReader& r, StepsMsg& m) {
    m.roundId = r.u32();
    m.firstStep = r.u32();
    const int n = r.u8();
    m.steps.assign(static_cast<std::size_t>(n), StepInputs{});
    for (StepInputs& s : m.steps)
        for (std::uint8_t& b : s) b = r.u8();
    m.hash = r.u32();
    return r.ok();
}

void encode(ByteWriter& w, const InputMsg& m) {
    w.u32(m.token);
    w.u32(m.roundId);
    w.u32(m.haveStep);
    for (std::uint8_t b : m.input) w.u8(b);
}

bool decode(ByteReader& r, InputMsg& m) {
    m.token = r.u32();
    m.roundId = r.u32();
    m.haveStep = r.u32();
    for (std::uint8_t& b : m.input) b = r.u8();
    return r.ok();
}

void encode(ByteWriter& w, const RoundEndMsg& m) {
    w.u32(m.roundId);
    w.u32(m.steps);
    w.i8(m.winner);
    w.flag(m.teamPlay);
    encodeScore(w, m.score);
    w.u8(static_cast<std::uint8_t>(m.campaign));
    w.flag(m.campaignOver);
}

bool decode(ByteReader& r, RoundEndMsg& m) {
    m.roundId = r.u32();
    m.steps = r.u32();
    m.winner = r.i8();
    m.teamPlay = r.flag();
    if (!decodeScore(r, m.score)) return false;
    m.campaign = r.u8();
    m.campaignOver = r.flag();
    return r.ok() && m.campaign <= 2 && m.winner >= -1 && m.winner < kMaxPlayers;
}

void encode(ByteWriter& w, const SnapshotMsg& m) {
    w.u32(m.roundId);
    w.u32(m.step);
    w.bytes(m.state);
}

bool decode(ByteReader& r, SnapshotMsg& m) {
    m.roundId = r.u32();
    m.step = r.u32();
    m.state = r.bytes(1u << 20);
    return r.ok();
}

void encode(ByteWriter& w, const ServerInfo& m) {
    w.u16(m.version);
    w.u16(m.port);
    w.text(m.name);
    w.u8(static_cast<std::uint8_t>(m.players));
    w.u8(static_cast<std::uint8_t>(m.seats));
    w.u8(static_cast<std::uint8_t>(m.phase));
    w.flag(m.password);
}

bool decode(ByteReader& r, ServerInfo& m) {
    m.version = r.u16();
    m.port = r.u16();
    m.name = r.text(64);
    m.players = r.u8();
    m.seats = r.u8();
    const int phase = r.u8();
    m.password = r.flag();
    if (!r.ok() || phase > 2) return false;
    m.phase = static_cast<Phase>(phase);
    return true;
}

}  // namespace ab::net
