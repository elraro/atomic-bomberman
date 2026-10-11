// Snapshots (docs/specifications/online-play.md, "Game model"): the public state
// of a round as a row of numbers (World::snapshotFields), sent as the difference
// from a row the receiver is known to hold. A full state is the difference from
// an empty row.
#pragma once

#include <cstdint>
#include <vector>

#include "net/crypto.hpp"

namespace ab::net {

using Fields = std::vector<std::int64_t>;

inline constexpr std::size_t kMaxSnapshotFields = 60000;

// For every place where `now` differs from `base`: how many places were skipped since the
// last one, and the difference. A place `base` does not have counts as zero there.
Bytes encodeDelta(const Fields& base, const Fields& now);
// False if the bytes are malformed or describe a row longer than kMaxSnapshotFields.
bool decodeDelta(const Fields& base, const Bytes& delta, Fields& now);

}  // namespace ab::net
