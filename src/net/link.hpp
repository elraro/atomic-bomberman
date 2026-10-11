// A link: what a client and the server talk through once they share keys
// (docs/specifications/online-play.md, "Game transport"). It offers two services
// over packets that may be lost, repeated or arrive out of order:
//   datagrams - unreliable, unordered, at most kMaxDatagram bytes each;
//   messages  - reliable and ordered, of any size.
// The link knows nothing of sockets: its owner hands it the packets that arrive
// and sends the packets it produces, over UDP or anything else that carries
// packets. Every packet is encrypted and authenticated with the link's keys and
// is accepted once only.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "net/crypto.hpp"

namespace ab::net {

class Link {
public:
    using Transmit = std::function<void(const Bytes&)>;

    static constexpr std::size_t kMaxDatagram = 1100;    // payload of one datagram
    static constexpr std::size_t kMaxPacket = 1200;      // what goes on the wire, at most
    static constexpr std::size_t kMaxMessage = 1u << 20;

    // `token` names the connection in every packet (in the clear; it is no secret).
    void start(const Key& sendKey, const Key& receiveKey, std::uint32_t token, std::uint64_t nowMs);
    void reset();
    bool started() const { return started_; }
    std::uint32_t token() const { return token_; }

    // Queued; sent by update().
    void sendMessage(std::uint8_t type, const Bytes& payload);
    // False (and nothing is sent) if the payload is larger than kMaxDatagram.
    bool sendDatagram(std::uint8_t type, const Bytes& payload);
    // The other side is told that this end is going away (best effort).
    void close() { closing_ = true; }

    // A packet from the wire. False if it is not a valid, new packet of this link.
    bool receive(const Bytes& packet, std::uint64_t nowMs);
    // Sends what is due: new data, repeats of what was not acknowledged, acknowledgements,
    // and a packet every second so that the other side knows this one is alive.
    void update(std::uint64_t nowMs, const Transmit& transmit);
    // Sends everything unacknowledged at once, whatever the timers say (before going away).
    void sendNow(std::uint64_t nowMs, const Transmit& transmit);

    bool nextMessage(std::uint8_t& type, Bytes& payload);
    bool nextDatagram(std::uint8_t& type, Bytes& payload);

    bool closedByPeer() const { return peerClosed_; }
    bool broken() const { return broken_; }            // the other side sent nonsense
    bool idle() const { return segments_.empty() && (!closing_ || closeSent_); }  // nothing waits to be delivered
    std::uint64_t lastHeard() const { return lastHeard_; }
    int rttMs() const { return static_cast<int>(srtt_); }
    std::size_t unacknowledged() const { return segments_.size(); }

private:
    struct Segment {
        std::uint32_t seq = 0;
        Bytes data;
        std::uint64_t sentAt = 0;
        int sends = 0;
        int passed = 0;      // acknowledgements seen for later segments while this one was missing
        bool acked = false;  // acknowledged out of order
    };
    struct Incoming {
        std::uint8_t type = 0;
        Bytes payload;
    };
    void buildPackets(std::uint64_t nowMs, const Transmit& transmit, bool everything);
    void acknowledged(std::uint32_t ack, std::uint32_t mask, std::uint64_t nowMs);
    void takeSegment(std::uint32_t seq, const std::uint8_t* data, std::size_t size);
    void parseStream();
    std::uint64_t timeoutOf(const Segment& s) const;

    bool started_ = false;
    Key sendKey_{}, receiveKey_{};
    std::uint32_t token_ = 0;
    std::uint64_t sent_ = 0;          // packets sent: the number of the last one
    std::uint64_t highest_ = 0;       // highest packet number accepted
    std::uint64_t seen_ = 0;          // bit i: packet highest_ - 1 - i was accepted

    std::deque<Segment> segments_;    // sent or waiting to be sent, not yet acknowledged in order
    std::uint32_t nextSeq_ = 0;
    std::deque<Incoming> outDatagrams_;

    std::uint32_t expected_ = 0;      // next segment wanted
    std::map<std::uint32_t, Bytes> early_;  // segments that arrived before their turn
    Bytes stream_;                    // segments in order, not yet a whole message
    std::deque<Incoming> messages_, datagrams_;

    bool ackDue_ = false;
    bool closing_ = false, closeSent_ = false;
    bool peerClosed_ = false, broken_ = false;
    std::uint64_t lastHeard_ = 0, lastSent_ = 0;
    double srtt_ = 100.0;
    bool measured_ = false;
};

}  // namespace ab::net
