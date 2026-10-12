// The relay reached over TCP or TLS, for players whose network lets no UDP out
// or stops carrying it after a few kilobytes (docs/specifications/online-play.md,
// "Connectivity"). TURN's messages are the same whatever carries them; only how
// they follow one another in a stream differs (RFC 8656, 12.5: a STUN message
// says its own length, channel data is filled up to a multiple of four bytes).
// So nothing of TURN is done here: libjuice speaks it, to a UDP port on this
// machine, and this bridge passes each datagram on into a stream to the real
// relay and cuts the stream that comes back into datagrams again.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ab::ice {

// One datagram as it goes into the stream. False: neither a STUN message nor channel data.
bool turnToStream(const std::uint8_t* datagram, std::size_t size, std::vector<std::uint8_t>& stream);
// The next whole message at the front of what has arrived, taken off it.
// 1: one taken, 0: not all of it is there yet, -1: this is not a TURN stream.
int turnFromStream(std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& datagram);

class TurnStream {
public:
    TurnStream() = default;
    ~TurnStream();
    TurnStream(const TurnStream&) = delete;
    TurnStream& operator=(const TurnStream&) = delete;

    // Opens the UDP port on this machine and begins to connect to the relay, on a thread of
    // its own. With `tls` the stream is wrapped in TLS; the relay's certificate is not judged
    // (what TURN protects, it protects itself with the credentials, and the game's packets are
    // sealed from end to end: TLS is here to pass where only HTTPS does).
    bool start(const std::string& host, int port, bool tls);
    int localPort() const { return port_; }  // on 127.0.0.1
    bool connected() const;
    bool failed() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    int port_ = 0;
};

}  // namespace ab::ice
