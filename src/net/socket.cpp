#include "net/socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
using NativeSocket = int;
#endif

namespace ab::net {

namespace {

NativeSocket native(std::intptr_t h) { return static_cast<NativeSocket>(h); }

void closeNative(std::intptr_t h) {
#ifdef _WIN32
    closesocket(native(h));
#else
    ::close(native(h));
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
#endif
}

bool setNonBlocking(std::intptr_t h) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(native(h), FIONBIO, &on) == 0;
#else
    const int flags = fcntl(native(h), F_GETFL, 0);
    return flags >= 0 && fcntl(native(h), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void setOption(std::intptr_t h, int level, int name, int value) {
    setsockopt(native(h), level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

sockaddr_in toSockaddr(const Address& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(a.ip);
    sa.sin_port = htons(a.port);
    return sa;
}

Address fromSockaddr(const sockaddr_in& sa) { return {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)}; }

std::uint16_t boundPort(std::intptr_t h) {
    sockaddr_in sa{};
    SockLen len = sizeof sa;
    if (getsockname(native(h), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

std::intptr_t newSocket(int type) {
    const NativeSocket s = ::socket(AF_INET, type, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    return static_cast<std::intptr_t>(s);
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

bool startup() {
#ifdef _WIN32
    static bool done = false;
    if (done) return true;
    WSADATA data;
    done = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return done;
#else
    std::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

std::uint64_t clockMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string Address::text() const {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
           std::to_string(ip & 255) + ":" + std::to_string(port);
}

std::optional<Address> resolve(const std::string& host, std::uint16_t port) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) return std::nullopt;
    Address a = fromSockaddr(*reinterpret_cast<sockaddr_in*>(list->ai_addr));
    a.port = port;
    freeaddrinfo(list);
    return a;
}

std::optional<Address> resolveHostPort(const std::string& text, std::uint16_t defaultPort) {
    std::string host = text;
    std::uint16_t port = defaultPort;
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.pop_back();
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.erase(host.begin());
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        const std::string digits = host.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        const int value = std::stoi(digits);
        if (value < 1 || value > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(value);
        host.resize(colon);
    }
    if (host.empty()) return std::nullopt;
    return resolve(host, port);
}

// --- TCP ---------------------------------------------------------------------

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept
    : fd_(o.fd_), connected_(o.connected_), peer_(o.peer_), in_(std::move(o.in_)), out_(std::move(o.out_)) {
    o.fd_ = kNone;
    o.connected_ = false;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        connected_ = o.connected_;
        peer_ = o.peer_;
        in_ = std::move(o.in_);
        out_ = std::move(o.out_);
        o.fd_ = kNone;
        o.connected_ = false;
    }
    return *this;
}

void TcpSocket::close() {
    if (fd_ != kNone) closeNative(fd_);
    fd_ = kNone;
    connected_ = false;
    in_.clear();
    out_.clear();
}

bool TcpSocket::connect(const Address& to) {
    startup();
    close();
    fd_ = newSocket(SOCK_STREAM);
    if (fd_ == kNone) return false;
    setNonBlocking(fd_);
    setOption(fd_, IPPROTO_TCP, TCP_NODELAY, 1);
    peer_ = to;
    const sockaddr_in sa = toSockaddr(to);
    if (::connect(native(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0) {
        connected_ = true;
        return true;
    }
    if (wouldBlock()) return true;
    close();
    return false;
}

void TcpSocket::send(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (fd_ == kNone) return;
    const auto length = static_cast<std::uint32_t>(payload.size() + 1);
    for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    out_.push_back(type);
    out_.insert(out_.end(), payload.begin(), payload.end());
}

bool TcpSocket::pump() {
    if (fd_ == kNone) return false;
    if (!connected_) {
        // A non-blocking connect is finished when the socket turns writable (or fails).
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(native(fd_), &writable);
        FD_SET(native(fd_), &failed);
        timeval none{0, 0};
        const int n = select(static_cast<int>(native(fd_)) + 1, nullptr, &writable, &failed, &none);
        if (n < 0) {
            close();
            return false;
        }
        if (n == 0) return true;
        int error = 0;
        SockLen len = sizeof error;
        getsockopt(native(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
        if (error != 0 || FD_ISSET(native(fd_), &failed)) {
            close();
            return false;
        }
        connected_ = true;
    }
    // Write.
    while (!out_.empty()) {
        const auto n = ::send(native(fd_), reinterpret_cast<const char*>(out_.data()),
#ifdef _WIN32
                              static_cast<int>(std::min<std::size_t>(out_.size(), 1u << 16)),
#else
                              std::min<std::size_t>(out_.size(), 1u << 16),
#endif
                              kSendFlags);
        if (n > 0) {
            out_.erase(out_.begin(), out_.begin() + n);
        } else if (wouldBlock()) {
            break;
        } else {
            close();
            return false;
        }
    }
    if (out_.size() > 4 * kMaxFrame) {  // the peer is not reading
        close();
        return false;
    }
    // Read.
    for (;;) {
        char buffer[8192];
        const auto n = ::recv(native(fd_), buffer, sizeof buffer, 0);
        if (n > 0) {
            in_.insert(in_.end(), buffer, buffer + n);
            if (in_.size() > 4 * kMaxFrame) {
                close();
                return false;
            }
        } else if (n == 0) {
            // Closed by the peer. Frames already received stay readable until close().
            connected_ = false;
            closeNative(fd_);
            fd_ = kNone;
            out_.clear();
            return false;
        } else if (wouldBlock()) {
            break;
        } else {
            close();
            return false;
        }
    }
    return true;
}

bool TcpSocket::receive(std::uint8_t& type, std::vector<std::uint8_t>& payload) {
    if (in_.size() < 5) return false;
    const std::uint32_t length = static_cast<std::uint32_t>(in_[0]) | (static_cast<std::uint32_t>(in_[1]) << 8) |
                                 (static_cast<std::uint32_t>(in_[2]) << 16) | (static_cast<std::uint32_t>(in_[3]) << 24);
    if (length == 0 || length > kMaxFrame) {  // not our protocol
        close();
        return false;
    }
    if (in_.size() < 4 + static_cast<std::size_t>(length)) return false;
    type = in_[4];
    payload.assign(in_.begin() + 5, in_.begin() + 4 + static_cast<std::ptrdiff_t>(length));
    in_.erase(in_.begin(), in_.begin() + 4 + static_cast<std::ptrdiff_t>(length));
    return true;
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
    if (fd_ != -1) closeNative(fd_);
    fd_ = -1;
    port_ = 0;
}

bool TcpListener::listen(std::uint16_t port) {
    startup();
    close();
    fd_ = newSocket(SOCK_STREAM);
    if (fd_ == -1) return false;
#ifndef _WIN32
    setOption(fd_, SOL_SOCKET, SO_REUSEADDR, 1);  // restart without waiting out old connections
#endif
    setNonBlocking(fd_);
    const sockaddr_in sa = toSockaddr({0, port});
    if (::bind(native(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0 || ::listen(native(fd_), 16) != 0) {
        close();
        return false;
    }
    port_ = boundPort(fd_);
    return true;
}

std::optional<TcpSocket> TcpListener::accept() {
    if (fd_ == -1) return std::nullopt;
    sockaddr_in sa{};
    SockLen len = sizeof sa;
    const NativeSocket s = ::accept(native(fd_), reinterpret_cast<sockaddr*>(&sa), &len);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return std::nullopt;
#else
    if (s < 0) return std::nullopt;
#endif
    TcpSocket socket;
    socket.fd_ = static_cast<std::intptr_t>(s);
    socket.connected_ = true;
    socket.peer_ = fromSockaddr(sa);
    setNonBlocking(socket.fd_);
    setOption(socket.fd_, IPPROTO_TCP, TCP_NODELAY, 1);
    return socket;
}

// --- UDP ---------------------------------------------------------------------

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (fd_ != -1) closeNative(fd_);
    fd_ = -1;
    port_ = 0;
}

bool UdpSocket::open(std::uint16_t port, bool broadcast) {
    startup();
    close();
    fd_ = newSocket(SOCK_DGRAM);
    if (fd_ == -1) return false;
    setNonBlocking(fd_);
    if (broadcast) setOption(fd_, SOL_SOCKET, SO_BROADCAST, 1);
    const sockaddr_in sa = toSockaddr({0, port});
    if (::bind(native(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0) {
        close();
        return false;
    }
    port_ = boundPort(fd_);
    return true;
}

bool UdpSocket::sendTo(const Address& to, const std::vector<std::uint8_t>& data) {
    if (fd_ == -1) return false;
    const sockaddr_in sa = toSockaddr(to);
    const auto n = ::sendto(native(fd_), reinterpret_cast<const char*>(data.data()),
#ifdef _WIN32
                            static_cast<int>(data.size()),
#else
                            data.size(),
#endif
                            kSendFlags, reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    return static_cast<long long>(n) == static_cast<long long>(data.size());
}

bool UdpSocket::receiveFrom(Address& from, std::vector<std::uint8_t>& data) {
    if (fd_ == -1) return false;
    char buffer[2048];
    sockaddr_in sa{};
    SockLen len = sizeof sa;
    const auto n = ::recvfrom(native(fd_), buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return false;  // nothing waiting (or an ICMP error from an earlier send: ignored)
    from = fromSockaddr(sa);
    data.assign(buffer, buffer + n);
    return true;
}

}  // namespace ab::net
