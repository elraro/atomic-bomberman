#include "net/socket.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
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

// An address as the socket layer wants it. On a socket for both families an IPv4
// address is written in its IPv6 form (::ffff:a.b.c.d).
struct Native {
    sockaddr_storage storage{};
    SockLen length = 0;
    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&storage); }
};

Native toNative(const Address& a, bool socketIsV6) {
    Native n;
    if (!a.v6 && !socketIsV6) {
        auto* sa = reinterpret_cast<sockaddr_in*>(&n.storage);
        sa->sin_family = AF_INET;
        sa->sin_addr.s_addr = htonl(a.ip);
        sa->sin_port = htons(a.port);
        n.length = sizeof(sockaddr_in);
        return n;
    }
    auto* sa = reinterpret_cast<sockaddr_in6*>(&n.storage);
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(a.port);
    if (a.v6) {
        std::memcpy(&sa->sin6_addr, a.ip6.data(), 16);
    } else {
        std::uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, static_cast<std::uint8_t>(a.ip >> 24), static_cast<std::uint8_t>(a.ip >> 16),
                                   static_cast<std::uint8_t>(a.ip >> 8), static_cast<std::uint8_t>(a.ip)};
        std::memcpy(&sa->sin6_addr, mapped, 16);
    }
    n.length = sizeof(sockaddr_in6);
    return n;
}

Address fromNative(const sockaddr_storage& storage) {
    Address a;
    if (storage.ss_family == AF_INET) {
        const auto* sa = reinterpret_cast<const sockaddr_in*>(&storage);
        a.ip = ntohl(sa->sin_addr.s_addr);
        a.port = ntohs(sa->sin_port);
        return a;
    }
    const auto* sa = reinterpret_cast<const sockaddr_in6*>(&storage);
    a.port = ntohs(sa->sin6_port);
    std::uint8_t bytes[16];
    std::memcpy(bytes, &sa->sin6_addr, 16);
    static const std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(bytes, kMapped, 12) == 0) {  // an IPv4 peer seen through a socket for both
        a.ip = (static_cast<std::uint32_t>(bytes[12]) << 24) | (static_cast<std::uint32_t>(bytes[13]) << 16) |
               (static_cast<std::uint32_t>(bytes[14]) << 8) | bytes[15];
        return a;
    }
    a.v6 = true;
    std::memcpy(a.ip6.data(), bytes, 16);
    return a;
}

std::uint16_t boundPort(std::intptr_t h) {
    sockaddr_storage sa{};
    SockLen len = sizeof sa;
    if (getsockname(native(h), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return fromNative(sa).port;
}

std::intptr_t newSocket(int type, int family = AF_INET);

// A socket bound to the port for IPv6 and IPv4 together; -1 if the system will not do that.
std::intptr_t boundDual(int type, std::uint16_t port, bool reuse) {
    const std::intptr_t h = newSocket(type, AF_INET6);
    if (h == -1) return -1;
    setOption(h, IPPROTO_IPV6, IPV6_V6ONLY, 0);
    if (reuse) setOption(h, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    if (::bind(native(h), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0) {
        closeNative(h);
        return -1;
    }
    return h;
}

std::intptr_t newSocket(int type, int family) {
    const NativeSocket s = ::socket(family, type, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    return static_cast<std::intptr_t>(s);
}

int g_udpLossPercent = 0;
std::uint32_t g_udpLossState = 12345;

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

void setTestUdpLoss(int percent) { g_udpLossPercent = percent; }

std::uint64_t clockMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool Address::loopback() const {
    static const std::uint8_t kOne[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return v6 ? std::memcmp(ip6.data(), kOne, 16) == 0 : (ip >> 24) == 127;
}

std::string Address::text() const {
    if (v6) {
        char buffer[64] = "";
        in6_addr raw;
        std::memcpy(&raw, ip6.data(), 16);
        inet_ntop(AF_INET6, &raw, buffer, sizeof buffer);
        return "[" + std::string(buffer) + "]:" + std::to_string(port);
    }
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
           std::to_string(ip & 255) + ":" + std::to_string(port);
}

std::optional<Address> resolve(const std::string& host, std::uint16_t port) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) return std::nullopt;
    const addrinfo* pick = nullptr;
    for (const addrinfo* it = list; it != nullptr && pick == nullptr; it = it->ai_next)
        if (it->ai_family == AF_INET || it->ai_family == AF_INET6) pick = it;
    if (pick == nullptr) {
        freeaddrinfo(list);
        return std::nullopt;
    }
    sockaddr_storage storage{};
    std::memcpy(&storage, pick->ai_addr, std::min<std::size_t>(sizeof storage, static_cast<std::size_t>(pick->ai_addrlen)));
    Address a = fromNative(storage);
    a.port = port;
    freeaddrinfo(list);
    return a;
}

std::optional<Address> resolveHostPort(const std::string& text, std::uint16_t defaultPort) {
    std::string host = text;
    std::uint16_t port = defaultPort;
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.pop_back();
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.erase(host.begin());
    std::string portText;
    bool havePort = false;
    if (!host.empty() && host.front() == '[') {  // [IPv6 literal] or [IPv6 literal]:port
        const auto close = host.find(']');
        if (close == std::string::npos) return std::nullopt;
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') return std::nullopt;
            portText = host.substr(close + 2);
            havePort = true;
        }
        host = host.substr(1, close - 1);
    } else if (const auto colon = host.rfind(':'); colon != std::string::npos && host.find(':') == colon) {  // one colon: host:port
        portText = host.substr(colon + 1);
        havePort = true;
        host.resize(colon);
    }
    if (havePort) {
        const std::string digits = portText;
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        const int value = std::stoi(digits);
        if (value < 1 || value > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) return std::nullopt;
    return resolve(host, port);
}

// --- blocking helpers (router conversation only) -------------------------------

namespace {
bool waitFor(std::intptr_t h, bool writing, std::uint64_t deadline) {
    const std::uint64_t now = clockMs();
    if (now >= deadline) return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(native(h), &set);
    const std::uint64_t left = deadline - now;
    timeval tv{static_cast<decltype(tv.tv_sec)>(left / 1000), static_cast<decltype(tv.tv_usec)>((left % 1000) * 1000)};
    return select(static_cast<int>(native(h)) + 1, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &tv) > 0;
}
}  // namespace

std::vector<std::string> multicastAsk(const std::string& group, std::uint16_t port, const std::string& message, int timeoutMs) {
    std::vector<std::string> answers;
    startup();
    const auto to = resolve(group, port);
    const std::intptr_t h = newSocket(SOCK_DGRAM);
    if (!to || to->v6 || h == -1) {
        if (h != -1) closeNative(h);
        return answers;
    }
    const Native sa = toNative(*to, false);
    ::sendto(native(h), message.data(),
#ifdef _WIN32
             static_cast<int>(message.size()),
#else
             message.size(),
#endif
             kSendFlags, sa.get(), sa.length);
    const std::uint64_t deadline = clockMs() + static_cast<std::uint64_t>(timeoutMs);
    while (waitFor(h, false, deadline)) {
        char buffer[4096];
        const auto n = ::recv(native(h), buffer, sizeof buffer, 0);
        if (n <= 0) break;
        answers.emplace_back(buffer, buffer + n);
        if (answers.size() >= 16) break;
    }
    closeNative(h);
    return answers;
}

std::optional<std::string> blockingExchange(const std::string& host, int port, const std::string& request, int timeoutMs, std::string* localIp) {
    startup();
    const auto to = resolve(host, static_cast<std::uint16_t>(port));
    if (!to) return std::nullopt;
    const std::intptr_t h = newSocket(SOCK_STREAM, to->v6 ? AF_INET6 : AF_INET);
    if (h == -1) return std::nullopt;
    setNonBlocking(h);
    const std::uint64_t deadline = clockMs() + static_cast<std::uint64_t>(timeoutMs);
    const Native sa = toNative(*to, to->v6);
    bool ok = ::connect(native(h), sa.get(), sa.length) == 0 || (wouldBlock() && waitFor(h, true, deadline));
    if (ok) {
        int error = 0;
        SockLen len = sizeof error;
        getsockopt(native(h), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
        ok = error == 0;
    }
    if (ok && localIp != nullptr) {
        sockaddr_storage own{};
        SockLen len = sizeof own;
        if (getsockname(native(h), reinterpret_cast<sockaddr*>(&own), &len) == 0) {
            const std::string text = fromNative(own).text();  // "a.b.c.d:port"
            *localIp = text.substr(0, text.rfind(':'));
        }
    }
    std::size_t sent = 0;
    while (ok && sent < request.size()) {
        const auto n = ::send(native(h), request.data() + sent,
#ifdef _WIN32
                              static_cast<int>(request.size() - sent),
#else
                              request.size() - sent,
#endif
                              kSendFlags);
        if (n > 0) sent += static_cast<std::size_t>(n);
        else if (!(wouldBlock() && waitFor(h, true, deadline))) ok = false;
    }
    std::string response;
    while (ok && response.size() < (1u << 20)) {
        char buffer[4096];
        const auto n = ::recv(native(h), buffer, sizeof buffer, 0);
        if (n > 0) {
            response.append(buffer, buffer + n);
            // A router that keeps the connection open: stop once the announced length is in.
            if (const auto head = response.find("\r\n\r\n"); head != std::string::npos) {
                std::string headers = response.substr(0, head);
                for (char& ch : headers) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (const auto at = headers.find("content-length:"); at != std::string::npos &&
                    response.size() - head - 4 >= static_cast<std::size_t>(std::atoll(headers.c_str() + at + 15)))
                    break;
            }
        } else if (n == 0) break;
        else if (!(wouldBlock() && waitFor(h, false, deadline))) break;
    }
    closeNative(h);
    if (!ok || response.empty()) return std::nullopt;
    return response;
}

// --- TCP ---------------------------------------------------------------------

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept
    : fd_(o.fd_), connected_(o.connected_), peer_(o.peer_), in_(std::move(o.in_)), out_(std::move(o.out_)), encrypted_(o.encrypted_),
      sendKey_(o.sendKey_), receiveKey_(o.receiveKey_), sent_(o.sent_), received_(o.received_) {
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
        encrypted_ = o.encrypted_;
        sendKey_ = o.sendKey_;
        receiveKey_ = o.receiveKey_;
        sent_ = o.sent_;
        received_ = o.received_;
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
    encrypted_ = false;
    sent_ = received_ = 0;
}

void TcpSocket::setKeys(const Key& sendKey, const Key& receiveKey) {
    sendKey_ = sendKey;
    receiveKey_ = receiveKey;
    encrypted_ = true;
    sent_ = received_ = 0;
}

bool TcpSocket::connect(const Address& to) {
    startup();
    close();
    fd_ = newSocket(SOCK_STREAM, to.v6 ? AF_INET6 : AF_INET);
    if (fd_ == kNone) return false;
    setNonBlocking(fd_);
    setOption(fd_, IPPROTO_TCP, TCP_NODELAY, 1);
    peer_ = to;
    const Native sa = toNative(to, to.v6);
    if (::connect(native(fd_), sa.get(), sa.length) == 0) {
        connected_ = true;
        return true;
    }
    if (wouldBlock()) return true;
    close();
    return false;
}

void TcpSocket::send(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (fd_ == kNone) return;
    if (encrypted_) {
        std::vector<std::uint8_t> plain;
        plain.reserve(payload.size() + 1);
        plain.push_back(type);
        plain.insert(plain.end(), payload.begin(), payload.end());
        const std::vector<std::uint8_t> sealed = seal(sendKey_, counterNonce(++sent_), nullptr, 0, plain.data(), plain.size());
        const auto length = static_cast<std::uint32_t>(sealed.size());
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
        out_.insert(out_.end(), sealed.begin(), sealed.end());
        return;
    }
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
    if (length == 0 || length > kMaxFrame + 64) {  // not our protocol
        close();
        return false;
    }
    if (in_.size() < 4 + static_cast<std::size_t>(length)) return false;
    if (encrypted_) {
        std::vector<std::uint8_t> plain;
        const bool genuine = ab::net::open(receiveKey_, counterNonce(++received_), nullptr, 0, in_.data() + 4, length, plain) && !plain.empty();
        if (!genuine) {  // altered, replayed, out of order or not encrypted with our key
            close();
            return false;
        }
        in_.erase(in_.begin(), in_.begin() + 4 + static_cast<std::ptrdiff_t>(length));
        type = plain[0];
        payload.assign(plain.begin() + 1, plain.end());
        return true;
    }
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
#ifndef _WIN32
    const bool reuse = true;  // restart without waiting out old connections
#else
    const bool reuse = false;
#endif
    fd_ = boundDual(SOCK_STREAM, port, reuse);
    if (fd_ == -1) {
        fd_ = newSocket(SOCK_STREAM);
        if (fd_ == -1) return false;
        if (reuse) setOption(fd_, SOL_SOCKET, SO_REUSEADDR, 1);
        const Native sa = toNative({0, port}, false);
        if (::bind(native(fd_), sa.get(), sa.length) != 0) {
            close();
            return false;
        }
    }
    setNonBlocking(fd_);
    if (::listen(native(fd_), 16) != 0) {
        close();
        return false;
    }
    port_ = boundPort(fd_);
    return true;
}

std::optional<TcpSocket> TcpListener::accept() {
    if (fd_ == -1) return std::nullopt;
    sockaddr_storage sa{};
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
    socket.peer_ = fromNative(sa);
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

bool UdpSocket::open(std::uint16_t port, bool broadcast, bool shared, bool both) {
    startup();
    close();
    v6_ = false;
    if (both) {
        fd_ = boundDual(SOCK_DGRAM, port, false);
        if (fd_ != -1) {
            v6_ = true;
            setNonBlocking(fd_);
            port_ = boundPort(fd_);
            return true;
        }
    }
    fd_ = newSocket(SOCK_DGRAM);
    if (fd_ == -1) return false;
    setNonBlocking(fd_);
    if (broadcast) setOption(fd_, SOL_SOCKET, SO_BROADCAST, 1);
    if (shared) {
        setOption(fd_, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
        setOption(fd_, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    }
    const Native sa = toNative({0, port}, false);
    if (::bind(native(fd_), sa.get(), sa.length) != 0) {
        close();
        return false;
    }
    port_ = boundPort(fd_);
    return true;
}

bool UdpSocket::sendTo(const Address& to, const std::vector<std::uint8_t>& data) {
    if (fd_ == -1) return false;
    if (g_udpLossPercent > 0) {
        g_udpLossState = g_udpLossState * 1103515245u + 12345u;
        if (static_cast<int>((g_udpLossState >> 16) % 100u) < g_udpLossPercent) return true;  // "sent", and lost
    }
    if (to.v6 && !v6_) return false;  // an IPv6 address through an IPv4-only socket
    const Native sa = toNative(to, v6_);
    const auto n = ::sendto(native(fd_), reinterpret_cast<const char*>(data.data()),
#ifdef _WIN32
                            static_cast<int>(data.size()),
#else
                            data.size(),
#endif
                            kSendFlags, sa.get(), sa.length);
    return static_cast<long long>(n) == static_cast<long long>(data.size());
}

bool UdpSocket::receiveFrom(Address& from, std::vector<std::uint8_t>& data) {
    if (fd_ == -1) return false;
    char buffer[2048];
    sockaddr_storage sa{};
    SockLen len = sizeof sa;
    const auto n = ::recvfrom(native(fd_), buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return false;  // nothing waiting (or an ICMP error from an earlier send: ignored)
    from = fromNative(sa);
    data.assign(buffer, buffer + n);
    return true;
}

}  // namespace ab::net
