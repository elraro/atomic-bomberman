// A carrier: something that takes a connection's packets to the other side and
// brings the other side's back, when that is neither the game's own UDP socket
// nor a TCP connection: a path found by ICE through routers, or by way of a
// TURN relay (docs/specifications/online-play.md, "Connectivity"). The network
// library knows nothing of how it does it; the link on top is the same.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ab::net {

class Carrier {
public:
    virtual ~Carrier() = default;
    // One packet out. May be dropped: the link above copes with loss.
    virtual void send(const std::vector<std::uint8_t>& packet) = 0;
    // The next packet that has arrived, if any.
    virtual bool receive(std::vector<std::uint8_t>& packet) = 0;
    // False once the path is gone for good.
    virtual bool alive() const = 0;
    // The other end as far as it is known ("a.b.c.d:port"), for logs, bans and limits.
    virtual std::string remote() const = 0;
    // What kind of path it is, for the player: "Direct", "Relay".
    virtual std::string kind() const = 0;
};

}  // namespace ab::net
