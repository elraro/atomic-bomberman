#include "net/snapshot.hpp"

namespace ab::net {

namespace {
void putVarint(Bytes& out, std::uint64_t u) {
    while (u >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(u | 0x80));
        u >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(u));
}

bool getVarint(const Bytes& in, std::size_t& pos, std::uint64_t& u) {
    u = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) return false;
        const std::uint8_t b = in[pos++];
        u |= static_cast<std::uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) return true;
    }
    return false;
}
}  // namespace

Bytes encodeDelta(const Fields& base, const Fields& now) {
    Bytes out;
    putVarint(out, now.size());
    std::size_t last = 0;  // one past the last place written
    for (std::size_t i = 0; i < now.size(); ++i) {
        const std::int64_t before = i < base.size() ? base[i] : 0;
        if (now[i] == before) continue;
        putVarint(out, i - last);
        // The difference, small numbers for small changes either way (zigzag).
        const auto d = static_cast<std::uint64_t>(now[i]) - static_cast<std::uint64_t>(before);
        putVarint(out, (d << 1) ^ (static_cast<std::uint64_t>(0) - (d >> 63)));
        last = i + 1;
    }
    return out;
}

bool decodeDelta(const Fields& base, const Bytes& delta, Fields& now) {
    std::size_t pos = 0;
    std::uint64_t size = 0;
    if (!getVarint(delta, pos, size) || size > kMaxSnapshotFields) return false;
    Fields out(static_cast<std::size_t>(size), 0);
    for (std::size_t i = 0; i < out.size() && i < base.size(); ++i) out[i] = base[i];
    std::size_t at = 0;
    while (pos < delta.size()) {
        std::uint64_t skip = 0, z = 0;
        if (!getVarint(delta, pos, skip) || !getVarint(delta, pos, z)) return false;
        if (skip >= out.size() || at + skip >= out.size()) return false;
        at += static_cast<std::size_t>(skip);
        const std::uint64_t d = (z >> 1) ^ (static_cast<std::uint64_t>(0) - (z & 1));
        out[at] = static_cast<std::int64_t>(static_cast<std::uint64_t>(out[at]) + d);
        ++at;
    }
    now = std::move(out);
    return true;
}

}  // namespace ab::net
