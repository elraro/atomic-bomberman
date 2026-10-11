// A stand-in HTTP server for the tests: answers each request with what a handler
// says. Plain HTTP on 127.0.0.1; one request per connection, one at a time.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <mbedtls/net_sockets.h>

namespace fake {

struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers;  // names in lower case
};
struct Reply {
    int status = 200;
    std::string body;
    bool chunked = false;
    bool noLength = false;
    bool hangUp = false;  // close without a word
};

class FakeServer {
public:
    explicit FakeServer(std::function<Reply(const Request&)> handler) : handler_(std::move(handler)) {
        mbedtls_net_init(&listen_);
        // Any free port: bind to 0 is not offered, so a few are tried.
        for (int p = 39100; p < 39200 && port_ == 0; ++p)
            if (mbedtls_net_bind(&listen_, "127.0.0.1", std::to_string(p).c_str(), MBEDTLS_NET_PROTO_TCP) == 0) port_ = p;
        thread_ = std::thread([this] { run(); });
    }
    ~FakeServer() {
        stop_ = true;
        // One last connection wakes the accept.
        mbedtls_net_context poke;
        mbedtls_net_init(&poke);
        mbedtls_net_connect(&poke, "127.0.0.1", std::to_string(port_).c_str(), MBEDTLS_NET_PROTO_TCP);
        mbedtls_net_free(&poke);
        thread_.join();
        mbedtls_net_free(&listen_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    int requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (!stop_) {
            mbedtls_net_context client;
            mbedtls_net_init(&client);
            if (mbedtls_net_accept(&listen_, &client, nullptr, 0, nullptr) != 0 || stop_) {
                mbedtls_net_free(&client);
                continue;
            }
            std::string raw;
            unsigned char buf[4096];
            std::size_t headEnd = std::string::npos, length = 0;
            while (true) {
                const int n = mbedtls_net_recv_timeout(&client, buf, sizeof buf, 2000);
                if (n <= 0) break;
                raw.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n));
                if (headEnd == std::string::npos) {
                    headEnd = raw.find("\r\n\r\n");
                    if (headEnd != std::string::npos) {
                        const std::size_t at = raw.find("Content-Length: ");
                        if (at != std::string::npos && at < headEnd) length = static_cast<std::size_t>(std::atoi(raw.c_str() + at + 16));
                    }
                }
                if (headEnd != std::string::npos && raw.size() >= headEnd + 4 + length) break;
            }
            if (headEnd != std::string::npos) {
                Request q;
                const std::size_t sp1 = raw.find(' '), sp2 = raw.find(' ', sp1 + 1);
                q.method = raw.substr(0, sp1);
                q.path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
                q.body = raw.substr(headEnd + 4);
                for (std::size_t at = raw.find("\r\n") + 2; at < headEnd;) {
                    const std::size_t eol = raw.find("\r\n", at), colon = raw.find(": ", at);
                    if (colon < eol) {
                        std::string name = raw.substr(at, colon - at);
                        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        q.headers[name] = raw.substr(colon + 2, eol - colon - 2);
                    }
                    at = eol + 2;
                }
                Reply r;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++requests_;
                    r = handler_(q);
                }
                if (!r.hangUp) {
                    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Type: application/json\r\n";
                    if (r.chunked) {
                        out += "Transfer-Encoding: chunked\r\n\r\n";
                        // In pieces of seven bytes, to be put together again.
                        for (std::size_t at = 0; at < r.body.size(); at += 7) {
                            const std::string piece = r.body.substr(at, 7);
                            char size[16];
                            std::snprintf(size, sizeof size, "%zx\r\n", piece.size());
                            out += size + piece + "\r\n";
                        }
                        out += "0\r\n\r\n";
                    } else if (r.noLength) {
                        out += "\r\n" + r.body;
                    } else {
                        out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n\r\n" + r.body;
                    }
                    mbedtls_net_send(&client, reinterpret_cast<const unsigned char*>(out.data()), out.size());
                }
            }
            mbedtls_net_free(&client);
        }
    }

    std::function<Reply(const Request&)> handler_;
    mbedtls_net_context listen_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    int requests_ = 0;
    bool stop_ = false;
};


}  // namespace fake
