#include "net/record.hpp"

#include <algorithm>
#include <exception>
#include <memory>

#include "game/match.hpp"

namespace ab::net {

namespace {

constexpr char kMagic[] = "ABREC";
constexpr std::uint8_t kFormat = 1;

void put(ByteWriter& w, const Key& k) {
    for (std::uint8_t b : k) w.u8(b);
}
void get(ByteReader& r, Key& k) {
    for (std::uint8_t& b : k) b = r.u8();
}

std::vector<SeedShare> ordered(std::vector<SeedShare> shares) {
    std::sort(shares.begin(), shares.end(), [](const SeedShare& a, const SeedShare& b) { return a.client < b.client; });
    return shares;
}

}  // namespace

Key commitOf(const Key& reveal) {
    static const char kWhat[] = "atomic bomberman seed share v1";
    Bytes all(kWhat, kWhat + sizeof kWhat - 1);
    all.insert(all.end(), reveal.begin(), reveal.end());
    return sha256(all);
}

Key recordIdOf(std::uint32_t roundId, const std::vector<SeedShare>& shares) {
    ByteWriter w;
    static const char kWhat[] = "atomic bomberman record v1";
    for (const char* c = kWhat; *c != 0; ++c) w.u8(static_cast<std::uint8_t>(*c));
    w.u32(roundId);
    for (const SeedShare& s : ordered(shares)) {
        w.u8(s.client);
        put(w, s.commit);
    }
    return sha256(w.data());
}

std::uint32_t seedOf(std::uint32_t roundId, const std::vector<SeedShare>& shares) {
    ByteWriter w;
    static const char kWhat[] = "atomic bomberman seed v1";
    for (const char* c = kWhat; *c != 0; ++c) w.u8(static_cast<std::uint8_t>(*c));
    w.u32(roundId);
    for (const SeedShare& s : ordered(shares)) {
        if (!s.revealed) continue;
        w.u8(s.client);
        put(w, s.reveal);
    }
    const Key h = sha256(w.data());
    const std::uint32_t seed = static_cast<std::uint32_t>(h[0]) | (static_cast<std::uint32_t>(h[1]) << 8) | (static_cast<std::uint32_t>(h[2]) << 16) |
                               (static_cast<std::uint32_t>(h[3]) << 24);
    return seed != 0 ? seed : 1;  // (a generator seeded with zero is as good as any other; kept away from the "no seed" value)
}

Key logChain(const std::vector<InputTuple>& values, std::size_t count) {
    Key chain{};
    for (std::size_t i = 0; i < count && i < values.size(); ++i) {
        Bytes all(chain.begin(), chain.end());
        all.insert(all.end(), values[i].begin(), values[i].end());
        chain = sha256(all);
    }
    return chain;
}

std::vector<std::uint8_t> logStatement(const Key& recordId, std::uint32_t count, const Key& chain) {
    ByteWriter w;
    static const char kWhat[] = "atomic bomberman input log v1";
    for (const char* c = kWhat; *c != 0; ++c) w.u8(static_cast<std::uint8_t>(*c));
    put(w, recordId);
    w.u32(count);
    put(w, chain);
    return w.data();
}

std::vector<std::uint8_t> encodeRecord(const MatchRecord& r) {
    ByteWriter w;
    for (const char* c = kMagic; *c != 0; ++c) w.u8(static_cast<std::uint8_t>(*c));
    w.u8(kFormat);
    w.text(r.gameVersion);
    w.u16(r.protocol);
    w.text(r.serverName);
    w.u64(r.startedUnix);
    w.bytes(encoded(r.start));
    w.u8(static_cast<std::uint8_t>(r.shares.size()));
    for (const SeedShare& s : r.shares) {
        w.u8(s.client);
        put(w, s.commit);
        w.flag(s.revealed);
        put(w, s.reveal);
    }
    for (const RecordSeat& s : r.seats) {
        w.u8(static_cast<std::uint8_t>(s.kind));
        w.u8(s.client);
        w.u8(s.local);
    }
    w.u8(static_cast<std::uint8_t>(r.players.size()));
    for (const PlayerLog& p : r.players) {
        w.u8(p.client);
        w.text(p.name);
        put(w, p.playerKey);
        w.flag(p.haveStatement);
        w.u32(p.signedCount);
        for (std::uint8_t b : p.signature) w.u8(b);
        w.u32(static_cast<std::uint32_t>(p.values.size()));
        for (const InputTuple& v : p.values)
            for (std::uint8_t b : v) w.u8(b);
    }
    w.u32(static_cast<std::uint32_t>(r.inputs.size()));
    for (std::size_t i = 0; i < r.inputs.size(); ++i) {
        for (std::uint8_t b : r.inputs[i]) w.u8(b);
        w.u16(i < r.computer.size() ? r.computer[i] : 0);
    }
    w.u32(r.hashEvery);
    w.u32(static_cast<std::uint32_t>(r.hashes.size()));
    for (std::uint32_t h : r.hashes) w.u32(h);
    w.u32(r.finalHash);
    w.bytes(encoded(r.end));
    w.bytes(Bytes(r.certificate.begin(), r.certificate.end()));
    return std::move(w.data());
}

bool decodeRecord(const std::vector<std::uint8_t>& bytes, MatchRecord& out) {
    ByteReader r(bytes);
    for (const char* c = kMagic; *c != 0; ++c)
        if (r.u8() != static_cast<std::uint8_t>(*c)) return false;
    if (r.u8() != kFormat) return false;
    MatchRecord m;
    m.gameVersion = r.text(64);
    m.protocol = r.u16();
    m.serverName = r.text(128);
    m.startedUnix = r.u64();
    {
        const Bytes start = r.bytes(1u << 18);
        ByteReader sr(start);
        if (!r.ok() || !decode(sr, m.start) || !sr.atEnd()) return false;
    }
    const int shares = r.u8();
    for (int i = 0; i < shares && r.ok(); ++i) {
        SeedShare s;
        s.client = r.u8();
        get(r, s.commit);
        s.revealed = r.flag();
        get(r, s.reveal);
        m.shares.push_back(s);
    }
    for (RecordSeat& s : m.seats) {
        const int kind = r.u8();
        s.client = r.u8();
        s.local = r.u8();
        if (kind > 2 || s.local >= kMaxLocalPlayers) return false;
        s.kind = static_cast<SeatKind>(kind);
    }
    const int players = r.u8();
    for (int i = 0; i < players && r.ok(); ++i) {
        PlayerLog p;
        p.client = r.u8();
        p.name = r.text(64);
        get(r, p.playerKey);
        p.haveStatement = r.flag();
        p.signedCount = r.u32();
        for (std::uint8_t& b : p.signature) b = r.u8();
        const std::uint32_t n = r.u32();
        if (!r.ok() || n > 200000) return false;
        p.values.resize(n);
        for (InputTuple& v : p.values)
            for (std::uint8_t& b : v) b = r.u8();
        if (p.signedCount > n) return false;
        m.players.push_back(std::move(p));
    }
    const std::uint32_t steps = r.u32();
    if (!r.ok() || steps > 2000000) return false;
    m.inputs.resize(steps);
    m.computer.resize(steps);
    for (std::uint32_t i = 0; i < steps && r.ok(); ++i) {
        for (std::uint8_t& b : m.inputs[i]) b = r.u8();
        m.computer[i] = r.u16();
    }
    m.hashEvery = r.u32();
    const std::uint32_t hashes = r.u32();
    if (!r.ok() || m.hashEvery == 0 || hashes > steps + 1) return false;
    m.hashes.resize(hashes);
    for (std::uint32_t& h : m.hashes) h = r.u32();
    m.finalHash = r.u32();
    {
        const Bytes end = r.bytes(4096);
        ByteReader er(end);
        if (!r.ok() || !decode(er, m.end) || !er.atEnd()) return false;
    }
    const Bytes cert = r.bytes(8192);
    m.certificate.assign(cert.begin(), cert.end());
    if (!r.ok() || !r.atEnd()) return false;
    out = std::move(m);
    return true;
}

bool inputsFollowLog(const MatchRecord& r, const PlayerLog& log, std::size_t* uncovered) {
    // Which of this client's players sit where.
    std::array<int, kMaxLocalPlayers> seatOf{-1, -1, -1, -1};
    for (int s = 0; s < kMaxPlayers; ++s) {
        const RecordSeat& seat = r.seats[static_cast<std::size_t>(s)];
        if (seat.kind == SeatKind::Human && seat.client == log.client) seatOf[seat.local] = s;
    }
    std::size_t beyond = 0;
    // Where the walk stands: before the first value nothing is held down.
    std::size_t at = 0;
    bool started = false;
    for (std::size_t step = 0; step < r.inputs.size(); ++step) {
        const std::uint16_t computer = step < r.computer.size() ? r.computer[step] : 0;
        // The same as a value, as far as this client's own seats (not played by the computer) go?
        auto same = [&](const InputTuple& value) {
            for (int l = 0; l < kMaxLocalPlayers; ++l) {
                const int s = seatOf[static_cast<std::size_t>(l)];
                if (s < 0 || (computer & (1u << s))) continue;
                if (r.inputs[step][static_cast<std::size_t>(s)] != value[static_cast<std::size_t>(l)]) return false;
            }
            return true;
        };
        if (started ? same(log.values[at]) : same(InputTuple{})) continue;  // still holding the same
        std::size_t next = started ? at + 1 : 0;
        while (next < log.values.size() && !same(log.values[next])) ++next;
        if (next >= log.values.size()) return false;  // something the player never held down, or held only earlier
        at = next;
        started = true;
        if (at >= log.signedCount) ++beyond;
    }
    if (uncovered != nullptr) *uncovered = beyond;
    return true;
}

RecordVerdict checkRecord(const MatchRecord& r, const std::string& gameVersion, const VerifyFn& verify) {
    RecordVerdict v;
    auto problem = [&](const std::string& text) {
        v.ok = false;
        v.problems.push_back(text);
    };
    if (r.gameVersion != gameVersion || r.protocol != kProtocolVersion) {
        problem("made by game version " + r.gameVersion + " (protocol " + std::to_string(r.protocol) + "); this is " + gameVersion + " (protocol " +
                std::to_string(kProtocolVersion) + "): a round can only be played again by the version that played it");
        return v;
    }
    // The seed: every number shown fits its promise, and the seed is what follows from them.
    bool serverShare = false;
    int shown = 0;
    for (const SeedShare& s : r.shares) {
        if (s.client == kServerSender) serverShare = s.revealed;
        if (!s.revealed) {
            v.notes.push_back("client " + std::to_string(s.client) + " promised a share of the seed and did not show it; it has no part in the seed");
            continue;
        }
        ++shown;
        if (commitOf(s.reveal) != s.commit) problem("a share of the seed does not fit what was promised (client " + std::to_string(s.client) + ")");
    }
    if (!serverShare) problem("the server's share of the seed is missing");
    if (shown < 2) v.notes.push_back("no player had a share in the seed: it was the server's alone");
    if (r.start.seed != seedOf(r.start.roundId, r.shares)) problem("the seed is not the one that follows from the shares");

    // The round again.
    if (r.inputs.size() != r.end.steps) problem("the record holds " + std::to_string(r.inputs.size()) + " steps, the result names " + std::to_string(r.end.steps));
    Values values = Values::defaults();
    for (const auto& [id, value] : r.start.values) values.set(id, value);
    std::unique_ptr<World> world;
    try {
        world = std::make_unique<World>(values, r.start.seed);
        applyRoundSetup(*world, values, r.start.setup);
        world->takeEvents();
        MatchScore score = r.start.score;
        bool decided = false;
        int winner = -1;
        std::size_t hashAt = 0;
        for (std::size_t step = 0; step < r.inputs.size(); ++step) {
            std::array<PlayerInput, kMaxPlayers> input{};
            for (std::size_t i = 0; i < input.size(); ++i) input[i] = unpackInput(r.inputs[step][i]);
            world->tick(kStepMs, input);
            world->takeEvents();
            if (world->roundOver() && !decided) {
                decided = true;
                winner = r.start.setup.campaign ? -1 : score.roundDecided(*world, r.start.winsNeeded, r.start.setup.winByKills);
            }
            if ((step + 1) % r.hashEvery == 0) {
                if (hashAt >= r.hashes.size() || r.hashes[hashAt] != world->stateHash(false)) {
                    problem("played again, the state after step " + std::to_string(step + 1) + " is not the recorded one");
                    break;
                }
                ++hashAt;
            }
        }
        if (v.ok) {
            if (world->stateHash(false) != r.finalHash) problem("played again, the round ends in another state than recorded");
            if (!decided) problem("played again, the round is not over where the record ends");
            if (decided && winner != r.end.winner) problem("played again, the winner is " + std::to_string(winner) + ", the record says " + std::to_string(r.end.winner));
            if (decided && (score.wins != r.end.score.wins || score.kills != r.end.score.kills || score.matchWinner != r.end.score.matchWinner))
                problem("played again, the score is not the recorded one");
        }
    } catch (const std::exception& e) {
        problem(std::string("the round cannot be played again (") + e.what() + ")");
    }

    // The players' own word on what they pressed.
    const Key recordId = recordIdOf(r.start.roundId, r.shares);
    for (const PlayerLog& p : r.players) {
        const std::string who = "\"" + p.name + "\"";
        const bool hasKey = p.playerKey != Key{};
        if (!hasKey) {
            v.notes.push_back(who + " showed no player key: nothing vouches for the inputs recorded for them");
            continue;
        }
        if (!p.haveStatement) {
            v.notes.push_back(who + " left no signed statement of their inputs");
            continue;
        }
        if (verify && !verify(p.playerKey, logStatement(recordId, p.signedCount, logChain(p.values, p.signedCount)), p.signature)) {
            problem("the signature on the inputs of " + who + " is not theirs");
            continue;
        }
        std::size_t uncovered = 0;
        if (!inputsFollowLog(r, p, &uncovered)) {
            problem("the inputs applied for " + who + " are not the ones in their own log");
            continue;
        }
        if (uncovered > 0) v.notes.push_back(who + ": " + std::to_string(uncovered) + " input(s) near the end are not covered by their last signature");
        if (!verify) v.notes.push_back("the signature of " + who + " was not checked (no signature code at hand)");
    }
    return v;
}

}  // namespace ab::net
