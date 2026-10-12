#include "online/turn_stream.hpp"

#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

namespace ab::ice {

namespace {

constexpr std::size_t kMaxDatagram = 2048;
constexpr std::size_t kMaxStream = 1u << 16;

#ifdef _WIN32
using SockLen = int;
using Fd = SOCKET;
#else
using SockLen = socklen_t;
using Fd = int;
#endif

std::size_t be16(const std::uint8_t* p) { return (static_cast<std::size_t>(p[0]) << 8) | p[1]; }

// Until one of the two sockets has something to read, or the time is up.
void waitForEither(Fd a, Fd b, int ms) {
#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(a, &readable);
    FD_SET(b, &readable);
    timeval limit{0, ms * 1000};
    select(0, &readable, nullptr, nullptr, &limit);
#else
    pollfd both[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
    poll(both, 2, ms);
#endif
}

}  // namespace

bool turnToStream(const std::uint8_t* datagram, std::size_t size, std::vector<std::uint8_t>& stream) {
    if (size < 4) return false;
    const int kind = datagram[0] & 0xC0;
    if (kind == 0x00) {  // a STUN message: a header of 20 bytes and what it says follows
        if (size != 20 + be16(datagram + 2)) return false;
        stream.insert(stream.end(), datagram, datagram + size);
        return true;
    }
    if (kind != 0x40) return false;
    const std::size_t whole = 4 + be16(datagram + 2);  // channel data: number, length, bytes
    if (whole > size) return false;
    stream.insert(stream.end(), datagram, datagram + whole);
    stream.resize(stream.size() + (4 - whole % 4) % 4, 0);
    return true;
}

int turnFromStream(std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& datagram) {
    if (stream.size() < 4) return 0;
    const int kind = stream[0] & 0xC0;
    std::size_t whole = 0, taken = 0;
    if (kind == 0x00) {
        whole = taken = 20 + be16(stream.data() + 2);
    } else if (kind == 0x40) {
        whole = 4 + be16(stream.data() + 2);
        taken = whole + (4 - whole % 4) % 4;
    } else {
        return -1;
    }
    if (stream.size() < taken) return 0;
    datagram.assign(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(whole));
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(taken));
    return 1;
}

struct TurnStream::Shared {
    std::atomic<bool> stop{false}, connected{false}, failed{false};
    std::string host;
    int port = 0;
    bool tls = false;
    mbedtls_net_context udp, tcp;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;

    Shared() {
        mbedtls_net_init(&udp);
        mbedtls_net_init(&tcp);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
    }
    ~Shared() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&tcp);
        mbedtls_net_free(&udp);
    }

    bool open() {
        if (mbedtls_net_connect(&tcp, host.c_str(), std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP) != 0) return false;
        if (tls) {
            psa_crypto_init();
            static const char kPurpose[] = "atomic bomberman relay";
            if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char*>(kPurpose), sizeof kPurpose) != 0) return false;
            if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            mbedtls_ssl_conf_read_timeout(&conf, 10000);
            if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, host.c_str()) != 0) return false;
            mbedtls_ssl_set_bio(&ssl, &tcp, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
            int r = 0;
            while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
                if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || stop.load()) return false;
            mbedtls_ssl_set_bio(&ssl, &tcp, mbedtls_net_send, mbedtls_net_recv, nullptr);
        }
        return mbedtls_net_set_nonblock(&tcp) == 0;
    }

    // All of it, or false: the connection is gone (or this bridge is being closed).
    bool write(const std::vector<std::uint8_t>& data) {
        std::size_t at = 0;
        while (at < data.size()) {
            if (stop.load()) return false;
            const int n = tls ? mbedtls_ssl_write(&ssl, data.data() + at, data.size() - at) : mbedtls_net_send(&tcp, data.data() + at, data.size() - at);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
                mbedtls_net_poll(&tcp, MBEDTLS_NET_POLL_WRITE, 50);
                continue;
            }
            if (n <= 0) return false;
            at += static_cast<std::size_t>(n);
        }
        return true;
    }

    void run() {
        if (!open()) {
            failed.store(true);
            return;
        }
        connected.store(true);
        const Fd fd = static_cast<Fd>(udp.fd);
        const Fd stream = static_cast<Fd>(tcp.fd);
        sockaddr_storage local{};  // where libjuice speaks from: the answers go there
        SockLen localLen = 0;
        std::vector<std::uint8_t> in, out, datagram;
        unsigned char buf[4096];
        while (!stop.load()) {
            bool busy = false;
            for (int i = 0; i < 64; ++i) {
                sockaddr_storage from{};
                SockLen fromLen = sizeof from;
                const auto n = recvfrom(fd, reinterpret_cast<char*>(buf), static_cast<int>(kMaxDatagram), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;
                local = from;
                localLen = fromLen;
                turnToStream(buf, static_cast<std::size_t>(n), out);
            }
            if (!out.empty()) {
                busy = true;
                if (!write(out)) break;
                out.clear();
            }
            const int n = tls ? mbedtls_ssl_read(&ssl, buf, sizeof buf) : mbedtls_net_recv(&tcp, buf, sizeof buf);
            if (n > 0) {
                busy = true;
                in.insert(in.end(), buf, buf + n);
                int got = 0;
                while ((got = turnFromStream(in, datagram)) == 1)
                    if (localLen != 0) sendto(fd, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0, reinterpret_cast<const sockaddr*>(&local), localLen);
                if (got < 0 || in.size() > kMaxStream) break;
            } else if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
                       && n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
            ) {
                break;  // closed by the relay, or an error
            }
            // Nothing either way: until there is (looking now and then whether this is to end).
            if (!busy && !(tls && mbedtls_ssl_get_bytes_avail(&ssl) > 0)) waitForEither(fd, stream, 20);
        }
        if (!stop.load()) failed.store(true);
        connected.store(false);
    }
};

TurnStream::~TurnStream() {
    // The thread may be inside a connect that the system takes its time over: it is left to
    // end by itself, and owns what it uses.
    if (shared_) shared_->stop.store(true);
}

bool TurnStream::start(const std::string& host, int port, bool tls) {
    if (shared_) return false;
    auto shared = std::make_shared<Shared>();
    shared->host = host;
    shared->port = port;
    shared->tls = tls;
    if (mbedtls_net_bind(&shared->udp, "127.0.0.1", "0", MBEDTLS_NET_PROTO_UDP) != 0 || mbedtls_net_set_nonblock(&shared->udp) != 0) return false;
    // Room for a burst: what does not fit while the stream is being written to is lost.
    const int room = 1 << 20;
    setsockopt(static_cast<Fd>(shared->udp.fd), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&room), sizeof room);
    sockaddr_storage bound{};
    SockLen len = sizeof bound;
    if (getsockname(static_cast<Fd>(shared->udp.fd), reinterpret_cast<sockaddr*>(&bound), &len) != 0 || bound.ss_family != AF_INET) return false;
    port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
    shared_ = shared;
    std::thread([shared] { shared->run(); }).detach();
    return true;
}

bool TurnStream::connected() const { return shared_ && shared_->connected.load(); }
bool TurnStream::failed() const { return shared_ && shared_->failed.load(); }

}  // namespace ab::ice
