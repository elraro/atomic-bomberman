#include "net/link.hpp"

#include <algorithm>

#include "net/protocol.hpp"

namespace ab::net {

namespace {
constexpr std::size_t kSegmentBytes = 1000;
constexpr std::size_t kWindow = 64;          // segments in flight at most
constexpr std::size_t kEarlyKept = 256;      // segments held that came before their turn
constexpr std::size_t kQueued = 1024;        // datagrams waiting to be read
constexpr std::uint64_t kKeepAliveMs = 1000;
constexpr std::size_t kPlainLimit = Link::kMaxPacket - 17 - 16;  // header and tag of a sealed packet
constexpr int kPacketsPerUpdate = 48;

enum Chunk : std::uint8_t { kSegment = 1, kDatagram = 2, kClose = 3 };
}  // namespace

void Link::reset() { *this = Link{}; }

void Link::start(const Key& sendKey, const Key& receiveKey, std::uint32_t token, std::uint64_t nowMs) {
    reset();
    sendKey_ = sendKey;
    receiveKey_ = receiveKey;
    token_ = token;
    lastHeard_ = nowMs;
    lastSent_ = 0;
    started_ = true;
}

// A message travels as: u32 length of the payload, u8 type, payload; cut into segments.
void Link::sendMessage(std::uint8_t type, const Bytes& payload) {
    if (payload.size() > kMaxMessage) return;
    Bytes frame;
    frame.reserve(payload.size() + 5);
    const auto n = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    frame.push_back(type);
    frame.insert(frame.end(), payload.begin(), payload.end());
    // Small messages share a segment with what is still unsent before them.
    std::size_t at = 0;
    if (!segments_.empty() && segments_.back().sends == 0 && segments_.back().data.size() < kSegmentBytes) {
        Bytes& last = segments_.back().data;
        at = std::min(frame.size(), kSegmentBytes - last.size());
        last.insert(last.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(at));
    }
    while (at < frame.size()) {
        const std::size_t size = std::min(kSegmentBytes, frame.size() - at);
        Segment s;
        s.seq = nextSeq_++;
        s.data.assign(frame.begin() + static_cast<std::ptrdiff_t>(at), frame.begin() + static_cast<std::ptrdiff_t>(at + size));
        segments_.push_back(std::move(s));
        at += size;
    }
}

bool Link::sendDatagram(std::uint8_t type, const Bytes& payload) {
    if (payload.size() > kMaxDatagram) return false;
    if (outDatagrams_.size() >= kQueued) outDatagrams_.pop_front();  // nobody is sending: the oldest goes
    outDatagrams_.push_back({type, payload});
    return true;
}

std::uint64_t Link::timeoutOf(const Segment& s) const {
    double t = std::clamp(srtt_ * 2.0 + 20.0, 60.0, 1000.0);
    for (int i = 1; i < s.sends && t < 3000.0; ++i) t *= 1.5;
    return static_cast<std::uint64_t>(std::min(t, 3000.0));
}

void Link::acknowledged(std::uint32_t ack, std::uint32_t mask, std::uint64_t nowMs) {
    // Everything before `ack` has arrived.
    while (!segments_.empty() && static_cast<std::int32_t>(ack - segments_.front().seq) > 0) {
        const Segment& s = segments_.front();
        if (s.sends == 1 && !s.acked && s.sentAt != 0 && nowMs >= s.sentAt) {
            // Measured only on what was sent once: a repeat's answer cannot be told from the first's.
            const double sample = static_cast<double>(nowMs - s.sentAt);
            srtt_ = measured_ ? srtt_ * 0.875 + sample * 0.125 : sample;
            measured_ = true;
        }
        segments_.pop_front();
    }
    // Bit i: segment ack + 1 + i has arrived although `ack` itself has not. Those need no
    // repeat; the ones passed over twice are repeated without waiting for their timer.
    if (mask == 0) return;
    int highest = 31;
    while (highest > 0 && (mask & (1u << highest)) == 0) --highest;
    const auto hurry = static_cast<std::uint64_t>(srtt_ / 2);
    for (Segment& s : segments_) {
        const std::uint32_t offset = s.seq - ack;  // 0: the one the other side is waiting for
        if (offset > static_cast<std::uint32_t>(highest) + 1) break;
        if (offset >= 1 && (mask & (1u << (offset - 1)))) {
            s.acked = true;
            continue;
        }
        if (s.acked || s.sends == 0 || s.sentAt == 0) continue;
        if (++s.passed >= 2 && nowMs - s.sentAt > hurry) s.sentAt = 0, s.passed = 0;
    }
}

void Link::parseStream() {
    std::size_t at = 0;
    while (stream_.size() - at >= 5) {
        const std::size_t size = static_cast<std::size_t>(stream_[at]) | (static_cast<std::size_t>(stream_[at + 1]) << 8) |
                                 (static_cast<std::size_t>(stream_[at + 2]) << 16) | (static_cast<std::size_t>(stream_[at + 3]) << 24);
        if (size > kMaxMessage) {
            broken_ = true;
            stream_.clear();
            return;
        }
        if (stream_.size() - at < 5 + size) break;
        Incoming m;
        m.type = stream_[at + 4];
        m.payload.assign(stream_.begin() + static_cast<std::ptrdiff_t>(at + 5), stream_.begin() + static_cast<std::ptrdiff_t>(at + 5 + size));
        messages_.push_back(std::move(m));
        at += 5 + size;
    }
    stream_.erase(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(at));
}

void Link::takeSegment(std::uint32_t seq, const std::uint8_t* data, std::size_t size) {
    ackDue_ = true;
    const auto ahead = static_cast<std::int32_t>(seq - expected_);
    if (ahead < 0) return;  // had it already
    if (ahead > 0) {
        if (static_cast<std::size_t>(ahead) < kEarlyKept && early_.size() < kEarlyKept) early_.emplace(seq, Bytes(data, data + size));
        return;
    }
    stream_.insert(stream_.end(), data, data + size);
    ++expected_;
    for (auto it = early_.find(expected_); it != early_.end(); it = early_.find(expected_)) {
        stream_.insert(stream_.end(), it->second.begin(), it->second.end());
        early_.erase(it);
        ++expected_;
    }
    parseStream();
}

bool Link::receive(const Bytes& packet, std::uint64_t nowMs) {
    if (!started_) return false;
    std::uint32_t token = 0;
    std::uint64_t number = 0;
    if (!peekSealed(packet, &token, &number) || token != token_ || number == 0) return false;
    // Each packet number once: newer than all before, or within the last 64 and not seen.
    if (number <= highest_) {
        const std::uint64_t back = highest_ - number;
        if (back == 0 || back > 64 || (seen_ & (1ull << (back - 1)))) return false;
    }
    Bytes plain;
    if (!openSealed(packet, receiveKey_, plain)) return false;
    if (number > highest_) {
        const std::uint64_t shift = number - highest_;
        seen_ = shift >= 64 ? 0 : (seen_ << shift);
        if (highest_ != 0 && shift <= 64) seen_ |= 1ull << (shift - 1);
        highest_ = number;
    } else {
        seen_ |= 1ull << (highest_ - number - 1);
    }
    // Heard again after a silence: what was waiting through it, with ever longer pauses
    // between tries, is sent again at once.
    if (nowMs - lastHeard_ > 1000)
        for (Segment& s : segments_)
            if (s.sends > 0) s.sentAt = 0, s.sends = 1;
    lastHeard_ = nowMs;

    ByteReader r(plain);
    const std::uint32_t ack = r.u32();
    const std::uint32_t mask = r.u32();
    if (!r.ok()) return false;
    acknowledged(ack, mask, nowMs);
    std::size_t at = 8;
    while (at < plain.size() && !broken_) {
        const std::uint8_t kind = plain[at++];
        if (kind == kClose) {
            peerClosed_ = true;
            continue;
        }
        if (kind == kSegment) {
            if (plain.size() - at < 6) break;
            const std::uint32_t seq = static_cast<std::uint32_t>(plain[at]) | (static_cast<std::uint32_t>(plain[at + 1]) << 8) |
                                      (static_cast<std::uint32_t>(plain[at + 2]) << 16) | (static_cast<std::uint32_t>(plain[at + 3]) << 24);
            const std::size_t size = static_cast<std::size_t>(plain[at + 4]) | (static_cast<std::size_t>(plain[at + 5]) << 8);
            at += 6;
            if (plain.size() - at < size) break;
            takeSegment(seq, plain.data() + at, size);
            at += size;
        } else if (kind == kDatagram) {
            if (plain.size() - at < 3) break;
            const std::uint8_t type = plain[at];
            const std::size_t size = static_cast<std::size_t>(plain[at + 1]) | (static_cast<std::size_t>(plain[at + 2]) << 8);
            at += 3;
            if (plain.size() - at < size) break;
            if (datagrams_.size() >= kQueued) datagrams_.pop_front();
            datagrams_.push_back({type, Bytes(plain.begin() + static_cast<std::ptrdiff_t>(at), plain.begin() + static_cast<std::ptrdiff_t>(at + size))});
            at += size;
        } else {
            break;  // a kind this version does not know: the rest of the packet is left alone
        }
    }
    return true;
}

void Link::buildPackets(std::uint64_t nowMs, const Transmit& transmit, bool everything) {
    if (!started_ || !transmit) return;
    // What is acknowledged with every packet.
    std::uint32_t mask = 0;
    for (const auto& [seq, data] : early_) {
        const std::uint32_t offset = seq - expected_;
        if (offset >= 1 && offset <= 32) mask |= 1u << (offset - 1);
    }
    Bytes plain;
    int packets = 0;
    auto begin = [&]() {
        plain.clear();
        ByteWriter w;
        w.u32(expected_);
        w.u32(mask);
        plain = std::move(w.data());
    };
    auto flush = [&]() {
        transmit(sealDatagram(token_, ++sent_, sendKey_, plain));
        lastSent_ = nowMs;
        ackDue_ = false;
        ++packets;
        begin();
    };
    begin();
    // Datagrams first: they are worth most when fresh.
    while (!outDatagrams_.empty() && packets < kPacketsPerUpdate) {
        const Incoming& d = outDatagrams_.front();
        if (plain.size() + 4 + d.payload.size() > kPlainLimit && plain.size() > 8) flush();
        plain.push_back(kDatagram);
        plain.push_back(d.type);
        plain.push_back(static_cast<std::uint8_t>(d.payload.size()));
        plain.push_back(static_cast<std::uint8_t>(d.payload.size() >> 8));
        plain.insert(plain.end(), d.payload.begin(), d.payload.end());
        outDatagrams_.pop_front();
    }
    std::size_t index = 0;
    for (Segment& s : segments_) {
        if (index++ >= kWindow || packets >= kPacketsPerUpdate) break;
        if (s.acked) continue;
        const bool due = s.sends == 0 || s.sentAt == 0 || nowMs - s.sentAt >= timeoutOf(s);
        if (!due && !everything) continue;
        if (plain.size() + 7 + s.data.size() > kPlainLimit && plain.size() > 8) flush();
        plain.push_back(kSegment);
        for (int i = 0; i < 4; ++i) plain.push_back(static_cast<std::uint8_t>(s.seq >> (8 * i)));
        plain.push_back(static_cast<std::uint8_t>(s.data.size()));
        plain.push_back(static_cast<std::uint8_t>(s.data.size() >> 8));
        plain.insert(plain.end(), s.data.begin(), s.data.end());
        s.sentAt = nowMs;
        ++s.sends;
        s.passed = 0;
    }
    // "Going away" is said once everything before it has been delivered, or at once when asked to hurry.
    bool closeNow = closing_ && (everything || (segments_.empty() && !closeSent_));
    if (closeNow) {
        plain.push_back(kClose);
        closeSent_ = true;
    }
    if (plain.size() > 8 || ackDue_ || nowMs - lastSent_ >= kKeepAliveMs) flush();
}

void Link::update(std::uint64_t nowMs, const Transmit& transmit) { buildPackets(nowMs, transmit, false); }
void Link::sendNow(std::uint64_t nowMs, const Transmit& transmit) { buildPackets(nowMs, transmit, true); }

bool Link::nextMessage(std::uint8_t& type, Bytes& payload) {
    if (messages_.empty()) return false;
    type = messages_.front().type;
    payload = std::move(messages_.front().payload);
    messages_.pop_front();
    return true;
}

bool Link::nextDatagram(std::uint8_t& type, Bytes& payload) {
    if (datagrams_.empty()) return false;
    type = datagrams_.front().type;
    payload = std::move(datagrams_.front().payload);
    datagrams_.pop_front();
    return true;
}

}  // namespace ab::net
