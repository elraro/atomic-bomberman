// A small HTTP/1.1 client for the control side of online play (the configuration
// document, the master server). TLS is mbedTLS's. It does not use the system's
// list of certificate authorities: a server is recognised by the pin of its key or
// by roots that the signed configuration names (tls_trust.hpp); the configuration
// itself needs no trust in where it came from, because it is signed. A request
// blocks; HttpJob runs one on a thread of its own.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ab::online {

struct HttpRequest {
    std::string method = "GET";
    std::string url;  // http://host[:port]/path or https://...
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // sent as JSON when not empty
    // https: SHA-256 (hex) of the server key's SubjectPublicKeyInfo, and roots (PEM) its chain
    // may lead to (made out to the host name of the URL). A matching pin or a chain to one of
    // the roots is enough. Both empty: any server is taken (only for fetching what is
    // verified by its own signature).
    std::vector<std::string> pins;
    std::vector<std::string> roots;
    int timeoutMs = 10000;  // for each read; a long poll needs more
};

struct HttpResponse {
    int status = 0;     // 0: the request did not get through; see error
    std::string body;
    std::string error;
    std::string pin;    // https: the pin of the key the server showed
    bool ok() const { return status >= 200 && status < 300; }
};

HttpResponse httpFetch(const HttpRequest& request);

// "http://a.b:8080/x?y" -> parts; false if it is not such a URL.
bool splitUrl(const std::string& url, bool* tls, std::string* host, std::string* port, std::string* path);

class HttpJob {
public:
    HttpJob() = default;
    ~HttpJob();
    HttpJob(const HttpJob&) = delete;
    HttpJob& operator=(const HttpJob&) = delete;
    void start(HttpRequest request);
    bool running() const { return state_ && !state_->done.load(); }
    bool finished() const { return state_ && state_->done.load(); }
    // The answer of a finished job; the job is then empty again.
    HttpResponse take();
    // Gives the job up: its thread ends by itself and its answer is dropped.
    void abandon();

private:
    struct State {
        std::atomic<bool> done{false};
        HttpResponse response;
    };
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace ab::online
