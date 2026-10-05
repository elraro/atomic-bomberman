// Asking the home router to let the game's port through (UPnP Internet Gateway
// Device), so that a player who hosts need not set up port forwarding by hand.
// The conversation with the router is: find it (SSDP search on the local
// network), read its description, and send it two "AddPortMapping" requests
// (TCP and UDP). The logic takes its network access as two functions, so it is
// tested against a router made of strings; the real ones use blocking sockets
// with timeouts and run on a thread of their own.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ab::net {

struct UpnpTransport {
    // Sends the search message to the SSDP multicast address and returns the answers.
    std::function<std::vector<std::string>(const std::string& message)> search;
    // One HTTP exchange with host:port. Returns the whole response (headers and body),
    // and in *localIp this machine's address as the router sees it.
    std::function<std::optional<std::string>(const std::string& host, int port, const std::string& request, std::string* localIp)> http;
};

struct UpnpResult {
    bool ok = false;
    std::string externalIp;   // the router's public address, if it told us
    std::string message;      // what happened, for the log
    // What is needed to take the mapping away again.
    std::string host;
    int hostPort = 0;
    std::string controlPath;
    std::string service;
};

// Pieces, exposed for the tests.
std::string httpHeader(const std::string& response, const std::string& name);   // "" if absent
bool splitUrl(const std::string& url, std::string* host, int* port, std::string* path);
// The control URL path and service type of the WAN connection service in a device description.
bool findWanService(const std::string& xml, std::string* controlUrl, std::string* serviceType);
std::string xmlValue(const std::string& xml, const std::string& tag);

UpnpResult upnpMapPort(const UpnpTransport& transport, std::uint16_t port, const std::string& description);
void upnpUnmapPort(const UpnpTransport& transport, const UpnpResult& mapping, std::uint16_t port);
// The transport over real sockets (timeouts of a few seconds).
UpnpTransport realUpnpTransport();

// Runs the mapping in the background. Poll finished(); the destructor takes the mapping away.
class PortMapper {
public:
    ~PortMapper();
    void start(std::uint16_t port, const std::string& description);
    void stop();  // waits for the thread, removes the mapping
    bool started() const { return started_; }
    bool finished() const { return finished_; }
    UpnpResult result();

private:
    std::thread thread_;
    std::atomic<bool> finished_{false};
    bool started_ = false;
    std::uint16_t port_ = 0;
    std::mutex mutex_;
    UpnpResult result_;
};

}  // namespace ab::net
