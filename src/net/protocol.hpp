// Messages of the client-server network mode (docs/specifications/networking.md)
// and their byte encoding. No sockets here: this part is plain data, so it is
// tested without a network.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "game/match.hpp"
#include "game/world.hpp"

namespace ab::net {

inline constexpr std::uint32_t kTcpMagic = 0x4E4D4241u;  // "ABMN"
inline constexpr std::uint32_t kUdpMagic = 0x554D4241u;  // "ABMU"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 27410;
// Every server also listens here for searches on the local network, whatever its own port.
inline constexpr std::uint16_t kDiscoveryPort = 27409;
inline constexpr int kStepMs = 50;
inline constexpr int kMaxStepsPerMessage = 40;
inline constexpr std::size_t kMaxName = 16;
inline constexpr std::size_t kMaxChat = 120;
inline constexpr std::uint8_t kServerSender = 255;

enum class ClientMsg : std::uint8_t { Hello = 1, Chat, Option, Start, Team, Kick, Input, UdpState, NeedState, Continue, Pong };
enum class ServerMsg : std::uint8_t { Welcome = 1, Reject, Lobby, Chat, RoundStart, Steps, RoundEnd, Snapshot, Ping };
enum class UdpMsg : std::uint8_t { Probe = 1, ProbeAck, Input, Steps, Query, Info };

enum class Phase : std::uint8_t { Lobby = 0, Round = 1, Result = 2 };
enum class SeatKind : std::uint8_t { Empty = 0, Human = 1, Computer = 2 };

// Match settings the administrator can change, in the order the lobby lists them.
enum class Option : std::uint8_t {
    Level = 0,
    Scheme,
    Wins,
    TeamPlay,
    PlayTime,
    Enclosement,
    Computers,
    RandomStart,
    ConveyorSpeed,
    StompedBombs,
    WinByKills,
    DiseasesDestroyable,
    Count
};

class ByteWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void i8(int v) { u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(v))); }
    void i16(int v) { u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(v))); }
    void i32(int v) { u32(static_cast<std::uint32_t>(v)); }
    void flag(bool v) { u8(v ? 1 : 0); }
    void text(const std::string& s);                     // u16 length + bytes
    void bytes(const std::vector<std::uint8_t>& data);  // u32 length + bytes
    std::vector<std::uint8_t>& data() { return out_; }
    const std::vector<std::uint8_t>& data() const { return out_; }

private:
    std::vector<std::uint8_t> out_;
};

// Reading past the end gives zeros and clears ok().
class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& data) : data_(data) {}
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    int i8() { return static_cast<std::int8_t>(u8()); }
    int i16() { return static_cast<std::int16_t>(u16()); }
    int i32() { return static_cast<std::int32_t>(u32()); }
    bool flag() { return u8() != 0; }
    std::string text(std::size_t maxLength = 1024);
    std::vector<std::uint8_t> bytes(std::size_t maxLength);
    bool ok() const { return ok_; }
    bool atEnd() const { return pos_ == data_.size(); }
    void fail() { ok_ = false; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// One step's input of one seat, in a byte: bits 0-3 north, east, south, west; 4 bomb; 5 action.
std::uint8_t packInput(const PlayerInput& in);
PlayerInput unpackInput(std::uint8_t bits);
using StepInputs = std::array<std::uint8_t, kMaxPlayers>;

struct MatchSettings {
    int level = 0;               // -1: random each match
    int schemeIndex = 0;
    std::string schemeTitle = "Basic";
    int winsNeeded = 2;
    bool teamPlay = false;
    int playTime = 150;
    int enclosementDepth = 1;
    int computers = 1;           // computer players wanted
    bool randomStart = false;
    int conveyorSpeed = 1;
    bool stompedBombsDetonate = true;
    bool winByKills = false;
    bool diseasesDestroyable = true;
};

struct Seat {
    SeatKind kind = SeatKind::Empty;
    std::uint8_t client = 0;  // for a human seat
    int team = 0;
};

struct ClientInfo {
    std::uint8_t id = 0;
    std::string name;
    int seat = -1;   // -1: watching
    int pingMs = 0;
};

struct LobbyState {
    Phase phase = Phase::Lobby;
    std::uint8_t admin = 0;
    MatchSettings settings;
    std::array<Seat, kMaxPlayers> seats{};
    std::vector<ClientInfo> clients;

    const ClientInfo* client(std::uint8_t id) const;
    // "Name", or "Computer" for a computer player's seat; empty for an empty one.
    std::string seatName(int seat) const;
};

struct HelloMsg {
    std::uint16_t version = kProtocolVersion;
    std::string name;
    std::string password;
};

struct WelcomeMsg {
    std::uint8_t id = 0;
    std::uint32_t token = 0;
    std::string serverName;
};

struct ChatMsg {
    std::uint8_t sender = kServerSender;
    std::string name;
    std::string text;
};

struct RoundStartMsg {
    std::uint32_t roundId = 0;
    std::uint32_t seed = 0;
    std::vector<std::pair<int, int>> values;  // the server's whole tuning table
    RoundSetup setup;
    int winsNeeded = 2;
    MatchScore score;  // before this round
};

struct StepsMsg {
    std::uint32_t roundId = 0;
    std::uint32_t firstStep = 0;      // steps before the first one in this message
    std::vector<StepInputs> steps;
    std::uint32_t hash = 0;           // of the state after the last step here
};

struct InputMsg {
    std::uint32_t token = 0;
    std::uint32_t roundId = 0;
    std::uint32_t haveStep = 0;       // steps received so far, without gaps
    std::uint8_t input = 0;
};

struct RoundEndMsg {
    std::uint32_t roundId = 0;
    std::uint32_t steps = 0;          // length of the round
    int winner = -1;                  // player, or team in team play; -1 draw
    bool teamPlay = false;
    MatchScore score;                 // after this round (matchWinner >= 0: match decided)
};

struct SnapshotMsg {
    std::uint32_t roundId = 0;
    std::uint32_t step = 0;
    std::vector<std::uint8_t> state;
};

struct ServerInfo {
    std::uint16_t version = kProtocolVersion;
    std::uint16_t port = kDefaultPort;
    std::string name;
    int players = 0;
    int seats = kMaxPlayers;
    Phase phase = Phase::Lobby;
    bool password = false;
};

// encode() appends to the writer; decode() returns false on anything malformed
// or out of range, so decoded data is safe to hand to the game core.
void encode(ByteWriter& w, const HelloMsg& m);
bool decode(ByteReader& r, HelloMsg& m);
void encode(ByteWriter& w, const WelcomeMsg& m);
bool decode(ByteReader& r, WelcomeMsg& m);
void encode(ByteWriter& w, const ChatMsg& m);
bool decode(ByteReader& r, ChatMsg& m);
void encode(ByteWriter& w, const LobbyState& m);
bool decode(ByteReader& r, LobbyState& m);
void encode(ByteWriter& w, const RoundSetup& m);
bool decode(ByteReader& r, RoundSetup& m);
void encode(ByteWriter& w, const RoundStartMsg& m);
bool decode(ByteReader& r, RoundStartMsg& m);
void encode(ByteWriter& w, const StepsMsg& m);
bool decode(ByteReader& r, StepsMsg& m);
void encode(ByteWriter& w, const InputMsg& m);
bool decode(ByteReader& r, InputMsg& m);
void encode(ByteWriter& w, const RoundEndMsg& m);
bool decode(ByteReader& r, RoundEndMsg& m);
void encode(ByteWriter& w, const SnapshotMsg& m);
bool decode(ByteReader& r, SnapshotMsg& m);
void encode(ByteWriter& w, const ServerInfo& m);
bool decode(ByteReader& r, ServerInfo& m);

template <class M>
std::vector<std::uint8_t> encoded(const M& m) {
    ByteWriter w;
    encode(w, m);
    return std::move(w.data());
}

// A UDP datagram: magic, type, then the message.
template <class M>
std::vector<std::uint8_t> datagram(UdpMsg type, const M& m) {
    ByteWriter w;
    w.u32(kUdpMagic);
    w.u8(static_cast<std::uint8_t>(type));
    encode(w, m);
    return std::move(w.data());
}
std::vector<std::uint8_t> datagram(UdpMsg type);

// Printable text of at most maxLength bytes: control characters removed, cut at a character boundary.
std::string cleanText(const std::string& s, std::size_t maxLength);

}  // namespace ab::net
