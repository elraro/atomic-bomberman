// Thin non-blocking TCP and UDP sockets (BSD sockets / Winsock), IPv4.
// Nothing here blocks except resolve(); everything is polled by its owner.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ab::net {

// Once per process before any socket is used (Winsock start-up; ignores SIGPIPE elsewhere).
bool startup();

// Milliseconds of a monotonic clock.
std::uint64_t clockMs();

struct Address {
    std::uint32_t ip = 0;    // host byte order
    std::uint16_t port = 0;
    friend bool operator==(const Address&, const Address&) = default;
    std::string text() const;  // "a.b.c.d:port"
};

inline constexpr std::uint32_t kLoopback = 0x7F000001u;
inline constexpr std::uint32_t kBroadcast = 0xFFFFFFFFu;

// Name or dotted address to an IPv4 address. May block on a name lookup.
std::optional<Address> resolve(const std::string& host, std::uint16_t port);
// "host" or "host:port".
std::optional<Address> resolveHostPort(const std::string& text, std::uint16_t defaultPort);

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

    bool listen(std::uint16_t port);  // 0: any free port
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

    bool open(std::uint16_t port, bool broadcast = false);  // 0: any free port
    void close();
    bool isOpen() const { return fd_ != -1; }
    std::uint16_t port() const { return port_; }
    bool sendTo(const Address& to, const std::vector<std::uint8_t>& data);
    bool receiveFrom(Address& from, std::vector<std::uint8_t>& data);

private:
    std::intptr_t fd_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace ab::net
