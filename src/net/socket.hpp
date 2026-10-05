// Thin non-blocking TCP and UDP sockets (BSD sockets / Winsock), IPv4 and IPv6.
// Nothing here blocks except resolve(); everything is polled by its owner.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ab::net {

// Once per process before any socket is used (Winsock start-up; ignores SIGPIPE elsewhere).
bool startup();

// For tests: this share (0-100) of all UDP datagrams sent by this process is thrown away.
void setTestUdpLoss(int percent);

// Milliseconds of a monotonic clock.
std::uint64_t clockMs();

struct Address {
    std::uint32_t ip = 0;    // IPv4, host byte order (unused for an IPv6 address)
    std::uint16_t port = 0;
    bool v6 = false;
    std::array<std::uint8_t, 16> ip6{};  // IPv6, network byte order
    friend bool operator==(const Address&, const Address&) = default;
    std::string text() const;  // "a.b.c.d:port" or "[x:x::x]:port"
    bool loopback() const;
};

inline constexpr std::uint32_t kLoopback = 0x7F000001u;
inline constexpr std::uint32_t kBroadcast = 0xFFFFFFFFu;

// Name or literal address to an address (the first the system offers, IPv4 or IPv6). May block on a name lookup.
std::optional<Address> resolve(const std::string& host, std::uint16_t port);
// "host", "host:port", an IPv6 literal, or "[IPv6 literal]:port".
std::optional<Address> resolveHostPort(const std::string& text, std::uint16_t defaultPort);

// Two blocking helpers for the conversation with a home router (upnp.hpp); not used by
// the game protocol. Both give up after timeoutMs.
// Sends a datagram to a multicast group and collects the answers that arrive in time.
std::vector<std::string> multicastAsk(const std::string& group, std::uint16_t port, const std::string& message, int timeoutMs);
// Connects, sends the request, reads until the other side closes. *localIp receives the
// address this machine used for the connection.
std::optional<std::string> blockingExchange(const std::string& host, int port, const std::string& request, int timeoutMs, std::string* localIp);

// A TCP connection carrying frames: u32 length, u8 type, payload.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Starts connecting; connected() turns true later, or pump() returns false.
    bool connect(const Address& to);
    void close();
    bool open() const { return fd_ != kNone; }
    bool connected() const { return connected_; }
    const Address& peer() const { return peer_; }

    // Queues a frame. Sent by pump().
    void send(std::uint8_t type, const std::vector<std::uint8_t>& payload);
    // Reads what has arrived and writes what is queued. False once the connection is gone
    // (closed by the peer, error, or a malformed frame); the socket is then closed.
    bool pump();
    // The next complete frame, if any.
    bool receive(std::uint8_t& type, std::vector<std::uint8_t>& payload);

    static constexpr std::size_t kMaxFrame = 1u << 20;

private:
    friend class TcpListener;
    using Handle = std::intptr_t;
    static constexpr Handle kNone = -1;
    Handle fd_ = kNone;
    bool connected_ = false;
    Address peer_{};
    std::vector<std::uint8_t> in_;
    std::vector<std::uint8_t> out_;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // 0: any free port. Listens for IPv6 and IPv4 together where the system allows it,
    // for IPv4 alone otherwise.
    bool listen(std::uint16_t port);
    void close();
    std::uint16_t port() const { return port_; }
    std::optional<TcpSocket> accept();

private:
    std::intptr_t fd_ = -1;
    std::uint16_t port_ = 0;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // 0: any free port. `shared`: several sockets on this machine may bind the port (each
    // gets a copy of a broadcast), used for the discovery port.
    // `both`: one socket for IPv6 and IPv4 (falls back to IPv4 alone where that is not possible).
    bool open(std::uint16_t port, bool broadcast = false, bool shared = false, bool both = false);
    void close();
    bool isOpen() const { return fd_ != -1; }
    std::uint16_t port() const { return port_; }
    bool sendTo(const Address& to, const std::vector<std::uint8_t>& data);
    bool receiveFrom(Address& from, std::vector<std::uint8_t>& data);

private:
    std::intptr_t fd_ = -1;
    std::uint16_t port_ = 0;
    bool v6_ = false;
};

}  // namespace ab::net
