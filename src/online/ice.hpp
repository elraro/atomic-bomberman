// Finding a way between two computers that both sit behind routers
// (docs/specifications/online-play.md, "Connectivity"): ICE, with STUN to learn
// one's public address and TURN as the relay when no direct way exists. All of
// that is libjuice's; this file only wraps one of its agents as a carrier for
// the game's packets (net/carrier.hpp).
//
// The two sides exchange a "description" each (a few lines of text: a name and
// a password for the checks, and the addresses each can be tried at) by way of
// the master server. Then the agents try every pair of addresses and settle on
// the best that works.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "net/carrier.hpp"

struct juice_agent;
struct juice_server;

namespace ab::ice {

struct TurnServer {
    std::string host;
    int port = 3478;
    std::string username, password;
};

struct Config {
    std::string stunHost;  // empty: none (only the computer's own addresses and the relays are offered)
    int stunPort = 3478;
    std::vector<TurnServer> turn;  // reached over UDP
    // "Hide my address": only relayed ways are offered to the other side, and only its relayed
    // ways are tried, so that neither end learns the other's address. Needs a relay on both sides.
    bool relayOnly = false;
};

class Agent : public net::Carrier {
public:
    enum class State { New, Gathering, Connecting, Connected, Failed };

    Agent();
    ~Agent() override;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // Begins to collect the addresses this side can be reached at.
    bool start(const Config& config);
    bool gathered() const { return gathered_.load(); }  // the description is complete
    // This side's description, for the other side. Empty until gathered().
    std::string localDescription() const;
    // The other side's. The checks begin once both sides have each other's.
    bool setRemoteDescription(const std::string& description);
    State state() const;

    // The carrier, once Connected.
    void send(const std::vector<std::uint8_t>& packet) override;
    bool receive(std::vector<std::uint8_t>& packet) override;
    bool alive() const override;
    std::string remote() const override;
    std::string kind() const override;  // "Direct" or "Relay"

    // Called by libjuice, on its own thread.
    void onState(int state);
    void onGathered() { gathered_.store(true); }
    void onPacket(const char* data, std::size_t size);

private:
    juice_agent* agent_ = nullptr;
    Config config_;
    std::atomic<int> state_{0};
    std::atomic<bool> gathered_{false};
    std::atomic<bool> wasConnected_{false};
    mutable std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> in_;
};

// Only the lines of a description that name relayed addresses (and the name and password).
std::string relayOnly(const std::string& description);

// A STUN and TURN server in this process, for tests (libjuice's own).
class TestServer {
public:
    TestServer(const std::string& username, const std::string& password);
    ~TestServer();
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    int port() const { return port_; }

private:
    juice_server* server_ = nullptr;
    int port_ = 0;
};

}  // namespace ab::ice
