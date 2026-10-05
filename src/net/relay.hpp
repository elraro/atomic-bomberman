// A relay: a meeting point for a host whose router lets nothing in. The host
// connects out to the relay and gets a short code; a player who gives the code
// is joined to the host through the relay, which copies the frames between the
// two connections. What it copies is already encrypted end to end, so the relay
// cannot read or alter the game; it can only carry it or drop it. Everything
// then goes over TCP (no UDP through a relay).
//
// Somebody has to run one on a machine that can be reached: `atomic_server
// --relay-server`. Like the game server it is polled, never blocks, and needs
// no SDL.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/socket.hpp"

namespace ab::net {

inline constexpr std::uint16_t kRelayPort = 27408;
inline constexpr std::uint32_t kRelayMagic = 0x524D4241u;  // "ABMR"

// Frames spoken with the relay itself, before it starts copying (all in the clear:
// they carry nothing but a code).
enum class RelayMsg : std::uint8_t {
    Host = 200,      // host: magic. Answered by Hosted.
    Hosted = 201,    // relay: the code players must give
    Join = 202,      // player: magic, code
    Incoming = 203,  // relay to host: ticket, the player's address
    Accept = 204,    // host, on a new connection: magic, ticket
    Refuse = 205,    // relay: why not (text)
    Ping = 206,      // host: still here
};

// "CODE@relay.host[:port]" -> the two parts; false for an ordinary address.
bool splitRelayAddress(const std::string& address, std::string* code, std::string* relay);

class Relay {
public:
    Relay();
    ~Relay();
    bool start(std::uint16_t port, std::function<void(const std::string&)> log = {});
    void stop();
    std::uint16_t port() const { return listener_.port(); }
    void update(std::uint64_t nowMs);
    int hosts() const { return static_cast<int>(hosts_.size()); }
    int links() const { return static_cast<int>(links_.size()); }

private:
    struct Fresh {  // connected, has not said yet what it is
        TcpSocket socket;
        std::uint64_t since = 0;
    };
    struct HostEntry {
        TcpSocket control;
        std::uint64_t lastHeard = 0;
    };
    struct Waiting {  // a player whose host has been told
        TcpSocket socket;
        std::uint64_t since = 0;
    };
    struct Link {
        TcpSocket a, b;
    };
    void log(const std::string& line) const;
    void refuse(TcpSocket& socket, const std::string& why);

    TcpListener listener_;
    std::function<void(const std::string&)> log_;
    std::vector<Fresh> fresh_;
    std::map<std::string, HostEntry> hosts_;            // by code
    std::map<std::uint32_t, Waiting> waiting_;          // by ticket
    std::vector<std::unique_ptr<Link>> links_;
    std::map<std::string, std::deque<std::uint64_t>> connects_;  // recent connections per address
};

}  // namespace ab::net
