// The record of a round (docs/specifications/online-play.md, section 7): what a
// server with a signing key leaves of every round, so that anybody can play the
// round again with the gameplay core and see that the result follows. It holds
// the round's setup, where its seed came from, the input applied for every seat
// in every step, state hashes on the way, the result, and each player's own
// signed log of what they pressed.
//
// This file is the format and the checks that need no signature code: encoding,
// the seed, the replay, and whether the inputs applied are a walk through each
// player's log. Signatures (the server's over the record, the players' over
// their logs, the certificate) are checked by whoever has that code; a function
// for it is handed in.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/protocol.hpp"

namespace ab::net {

using InputTuple = std::array<std::uint8_t, kMaxLocalPlayers>;  // what the players at one computer hold down
using SignatureBytes = std::array<std::uint8_t, 64>;
using VerifyFn = std::function<bool(const Key& key, const std::vector<std::uint8_t>& message, const SignatureBytes& signature)>;

// One party's share in the seed: promised (the hash) before anybody's number was known,
// then shown. `client` is the client id, kServerSender for the server.
struct SeedShare {
    std::uint8_t client = kServerSender;
    Key commit{};
    Key reveal{};
    bool revealed = false;  // a client that promised and then went silent has no part in the seed
};
Key commitOf(const Key& reveal);
// What names a round to its players before it is played: the promises, and whose they are.
Key recordIdOf(std::uint32_t roundId, const std::vector<SeedShare>& shares);
// The seed that follows from the numbers shown.
std::uint32_t seedOf(std::uint32_t roundId, const std::vector<SeedShare>& shares);

// A player's log: every different thing held down, in order, and a signature over the
// first `signedCount` of them.
struct PlayerLog {
    std::uint8_t client = 0;
    std::string name;
    Key playerKey{};                 // zeros: the player showed no key
    std::vector<InputTuple> values;
    std::uint32_t signedCount = 0;   // how many of them the signature covers
    SignatureBytes signature{};
    bool haveStatement = false;
};
// The running hash over the first `count` values of a log.
Key logChain(const std::vector<InputTuple>& values, std::size_t count);
// What a player signs: the round (its id), how many values, and their running hash.
std::vector<std::uint8_t> logStatement(const Key& recordId, std::uint32_t count, const Key& chain);

struct RecordSeat {
    SeatKind kind = SeatKind::Empty;
    std::uint8_t client = 0;  // for a human seat: the client id...
    std::uint8_t local = 0;   // ...and which of its players
};

struct MatchRecord {
    std::string gameVersion;
    std::uint16_t protocol = kProtocolVersion;
    std::string serverName;
    std::uint64_t startedUnix = 0;
    RoundStartMsg start;                 // with the seed, which the players were not told
    std::vector<SeedShare> shares;
    std::array<RecordSeat, kMaxPlayers> seats{};
    std::vector<PlayerLog> players;
    std::vector<StepInputs> inputs;      // the input applied for every seat, step by step
    std::vector<std::uint16_t> computer; // per step, bit s: seat s was played by the computer (also a person's seat they had left)
    std::uint32_t hashEvery = 20;
    std::vector<std::uint32_t> hashes;   // the whole state's hash after every `hashEvery` steps
    std::uint32_t finalHash = 0;
    RoundEndMsg end;
    std::string certificate;             // the server's, if it has one
};

std::vector<std::uint8_t> encodeRecord(const MatchRecord& r);
bool decodeRecord(const std::vector<std::uint8_t>& bytes, MatchRecord& r);

// Is every input applied for `client` one that the player's log holds, taken in the log's
// order (values may be passed over, never gone back to)? Steps the computer played are
// not looked at. `uncovered`: how many of those steps used a value beyond the signed part.
bool inputsFollowLog(const MatchRecord& r, const PlayerLog& log, std::size_t* uncovered = nullptr);

struct RecordVerdict {
    bool ok = true;                      // nothing below contradicts the record
    std::vector<std::string> problems;   // what does contradict it
    std::vector<std::string> notes;      // what is worth knowing and contradicts nothing
};
// Plays the round again and checks everything that needs no key: the seed, every hash,
// the result, the inputs against the logs. With `verify`, the players' signatures too.
// `gameVersion`: this program's; a record of another version cannot be replayed.
RecordVerdict checkRecord(const MatchRecord& r, const std::string& gameVersion, const VerifyFn& verify = {});

}  // namespace ab::net
