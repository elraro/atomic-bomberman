#include "online/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "online/sign.hpp"

namespace ab::online {

namespace {

constexpr std::size_t kMaxResponse = 1u << 20;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string tlsError(const char* what, int code) {
    char buf[160];
    mbedtls_strerror(code, buf, sizeof buf);
    return std::string(what) + ": " + buf;
}

// The connection: a socket, with TLS on top when asked for.
struct Connection {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    bool tls = false;
    int timeoutMs = 10000;

    Connection() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
    }
    ~Connection() {
        if (tls) mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&net);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool write(const std::string& data, std::string& error) {
        std::size_t at = 0;
        while (at < data.size()) {
            const auto* p = reinterpret_cast<const unsigned char*>(data.data()) + at;
            const int n = tls ? mbedtls_ssl_write(&ssl, p, data.size() - at) : mbedtls_net_send(&net, p, data.size() - at);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) {
                error = tlsError("cannot send", n);
                return false;
            }
            at += static_cast<std::size_t>(n);
        }
        return true;
    }
    // More bytes onto `into`. 0: the other side has closed. -1: error.
    int read(std::string& into, std::string& error) {
        unsigned char buf[4096];
        while (true) {
            const int n = tls ? mbedtls_ssl_read(&ssl, buf, sizeof buf) : mbedtls_net_recv_timeout(&net, buf, sizeof buf, static_cast<std::uint32_t>(timeoutMs));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_NET_CONN_RESET) return 0;
            if (n < 0) {
                error = n == MBEDTLS_ERR_SSL_TIMEOUT ? std::string("no answer in time") : tlsError("cannot receive", n);
                return -1;
            }
            into.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
            return n;
        }
    }
};

// A body sent in chunks: "size in hex, CRLF, bytes, CRLF" until a chunk of size zero.
// 1: complete, 0: more to come, -1: malformed.
int unchunk(const std::string& raw, std::string& body) {
    std::size_t at = 0;
    std::string out;
    while (true) {
        const std::size_t eol = raw.find("\r\n", at);
        if (eol == std::string::npos) return 0;
        char* end = nullptr;
        const unsigned long size = std::strtoul(raw.c_str() + at, &end, 16);
        if (end == raw.c_str() + at || size > kMaxResponse) return -1;
        at = eol + 2;
        if (size == 0) {
            body = std::move(out);
            return 1;
        }
        if (raw.size() < at + size + 2) return 0;
        out.append(raw, at, size);
        at += size + 2;
    }
}

}  // namespace

bool splitUrl(const std::string& url, bool* tls, std::string* host, std::string* port, std::string* path) {
    std::size_t at = 0;
    if (url.rfind("https://", 0) == 0) {
        *tls = true;
        at = 8;
    } else if (url.rfind("http://", 0) == 0) {
        *tls = false;
        at = 7;
    } else {
        return false;
    }
    const std::size_t slash = url.find('/', at);
    std::string authority = url.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    *path = slash == std::string::npos ? "/" : url.substr(slash);
    *port = *tls ? "443" : "80";
    if (!authority.empty() && authority[0] == '[') {  // an IPv6 literal
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        *host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            *port = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        *host = authority.substr(0, colon);
        if (colon != std::string::npos) *port = authority.substr(colon + 1);
    }
    if (host->empty() || port->empty() || port->size() > 5) return false;
    for (const char c : *port)
        if (c < '0' || c > '9') return false;
    for (const char c : *host + *path)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return false;  // nothing that could start a second header line
    return true;
}

HttpResponse httpFetch(const HttpRequest& request) {
    HttpResponse response;
    bool tls = false;
    std::string host, port, path;
    if (!splitUrl(request.url, &tls, &host, &port, &path)) {
        response.error = "not an address: " + request.url;
        return response;
    }
    Connection c;
    c.tls = false;  // set once the handshake is done, so that an early exit does not say goodbye over nothing
    c.timeoutMs = request.timeoutMs;
    if (const int r = mbedtls_net_connect(&c.net, host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP); r != 0) {
        response.error = "cannot reach " + host + ":" + port;
        return response;
    }
    if (tls) {
        psa_crypto_init();
        static const char kPurpose[] = "atomic bomberman online";
        int r = mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy, reinterpret_cast<const unsigned char*>(kPurpose), sizeof kPurpose);
        if (r == 0) r = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (r != 0) {
            response.error = tlsError("cannot set up TLS", r);
            return response;
        }
        // No list of authorities is consulted: the key is checked against the pins below.
        mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
        mbedtls_ssl_conf_read_timeout(&c.conf, static_cast<std::uint32_t>(request.timeoutMs));
        if ((r = mbedtls_ssl_setup(&c.ssl, &c.conf)) != 0 || (r = mbedtls_ssl_set_hostname(&c.ssl, host.c_str())) != 0) {
            response.error = tlsError("cannot set up TLS", r);
            return response;
        }
        mbedtls_ssl_set_bio(&c.ssl, &c.net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        while ((r = mbedtls_ssl_handshake(&c.ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            response.error = tlsError("TLS handshake failed", r);
            return response;
        }
        c.tls = true;
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&c.ssl);
        if (peer == nullptr || peer->pk_raw.p == nullptr) {
            response.error = "the server showed no certificate";
            return response;
        }
        response.pin = sha256Hex(peer->pk_raw.p, peer->pk_raw.len);
        if (!request.pins.empty()) {
            bool known = false;
            for (const std::string& pin : request.pins) known = known || lower(pin) == response.pin;
            if (!known) {
                response.error = "this is not the server the configuration names (its key does not match)";
                return response;
            }
        }
    }

    std::string head = request.method + " " + path + " HTTP/1.1\r\nHost: " + host + (port == (tls ? "443" : "80") ? "" : ":" + port) +
                       "\r\nConnection: close\r\nAccept: application/json\r\nUser-Agent: atomic-bomberman\r\n";
    for (const auto& [name, value] : request.headers) {
        if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos) continue;
        head += name + ": " + value + "\r\n";
    }
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT")
        head += "Content-Type: application/json\r\nContent-Length: " + std::to_string(request.body.size()) + "\r\n";
    head += "\r\n";
    if (!c.write(head + request.body, response.error)) return response;

    // The answer: status line and headers, then a body of a stated length, in chunks, or until the end.
    std::string raw;
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        const int n = c.read(raw, response.error);
        if (n < 0) return response;
        headEnd = raw.find("\r\n\r\n");
        if (n == 0 && headEnd == std::string::npos) {
            response.error = "the server closed the connection";
            return response;
        }
        if (raw.size() > kMaxResponse) {
            response.error = "the answer is too long";
            return response;
        }
    }
    int status = 0;
    if (raw.compare(0, 5, "HTTP/") != 0 || raw.size() < 12 || (status = std::atoi(raw.c_str() + 9)) < 100 || status > 599) {
        response.error = "not an HTTP answer";
        return response;
    }
    long long length = -1;
    bool chunked = false;
    for (std::size_t at = raw.find("\r\n") + 2; at < headEnd;) {
        const std::size_t eol = raw.find("\r\n", at);
        const std::string line = raw.substr(at, eol - at);
        at = eol + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (name == "content-length") length = std::atoll(value.c_str());
        if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
    }
    std::string rest = raw.substr(headEnd + 4);
    if (length > static_cast<long long>(kMaxResponse)) {
        response.error = "the answer is too long";
        return response;
    }
    bool closed = false;
    while (true) {
        if (chunked) {
            const int done = unchunk(rest, response.body);
            if (done < 0) {
                response.error = "malformed answer";
                return response;
            }
            if (done == 1) break;
        } else if (length >= 0 && rest.size() >= static_cast<std::size_t>(length)) {
            response.body = rest.substr(0, static_cast<std::size_t>(length));
            break;
        } else if (closed) {
            if (length >= 0) {
                response.error = "the answer was cut short";
                return response;
            }
            response.body = rest;  // no length given: the body is what came before the end
            break;
        }
        if (closed) {
            response.error = "the answer was cut short";
            return response;
        }
        const int n = c.read(rest, response.error);
        if (n < 0) return response;
        closed = n == 0;
        if (rest.size() > kMaxResponse + 4096) {
            response.error = "the answer is too long";
            return response;
        }
    }
    response.status = status;
    return response;
}

HttpJob::~HttpJob() { abandon(); }

void HttpJob::start(HttpRequest request) {
    abandon();
    state_ = std::make_shared<State>();
    thread_ = std::thread([state = state_, request = std::move(request)]() {
        state->response = httpFetch(request);
        state->done.store(true);
    });
}

HttpResponse HttpJob::take() {
    HttpResponse out;
    if (!finished()) return out;
    if (thread_.joinable()) thread_.join();
    out = std::move(state_->response);
    state_.reset();
    return out;
}

void HttpJob::abandon() {
    if (thread_.joinable()) {
        // A finished thread is joined; one still waiting for its answer is let go and
        // ends by itself (it holds its own share of the state).
        if (state_ && state_->done.load()) thread_.join();
        else thread_.detach();
    }
    state_.reset();
}

}  // namespace ab::online
