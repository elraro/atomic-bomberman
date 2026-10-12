#include "online/ice.hpp"

#include <cstdlib>
#include <cstring>

#include <juice/juice.h>

namespace ab::ice {

namespace {

constexpr std::size_t kQueued = 2048;  // packets waiting to be read; more than that and the oldest go

void quietOnce() {
    static const bool done = [] {
        // libjuice's own log: only on request (AB_ICE_LOG=1), it is chatty.
        const char* want = std::getenv("AB_ICE_LOG");
        juice_set_log_level(want != nullptr && want[0] == '1' ? JUICE_LOG_LEVEL_DEBUG : JUICE_LOG_LEVEL_NONE);
        return true;
    }();
    (void)done;
}

void cbState(juice_agent_t*, juice_state_t state, void* user) { static_cast<Agent*>(user)->onState(static_cast<int>(state)); }
void cbGathered(juice_agent_t*, void* user) { static_cast<Agent*>(user)->onGathered(); }
void cbRecv(juice_agent_t*, const char* data, size_t size, void* user) { static_cast<Agent*>(user)->onPacket(data, size); }

}  // namespace

std::string relayOnly(const std::string& description) {
    std::string out;
    std::size_t at = 0;
    while (at < description.size()) {
        std::size_t end = description.find('\n', at);
        if (end == std::string::npos) end = description.size();
        const std::string line = description.substr(at, end - at);
        at = end + 1;
        const bool candidate = line.rfind("a=candidate:", 0) == 0;
        if (!candidate || line.find(" typ relay") != std::string::npos) out += line + "\n";
    }
    return out;
}

Agent::Agent() { quietOnce(); }

Agent::~Agent() {
    if (agent_ != nullptr) juice_destroy(agent_);  // waits for its thread to be done with us
}

bool Agent::reachedOutside() const {
    if (agent_ == nullptr || !gathered_.load()) return false;
    char buffer[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(agent_, buffer, sizeof buffer) != JUICE_ERR_SUCCESS) return false;
    return std::strstr(buffer, " typ srflx") != nullptr || std::strstr(buffer, " typ relay") != nullptr;
}

bool Agent::start(const Config& config) {
    if (agent_ != nullptr) return false;
    config_ = config;
    if (config_.turn.size() > 2) config_.turn.resize(2);
    for (TurnServer& t : config_.turn) {
        if (t.transport == "udp") continue;
        // libjuice speaks TURN over UDP alone: to this machine, and a bridge takes it on.
        auto stream = std::make_unique<TurnStream>();
        if (!stream->start(t.host, t.port, t.transport == "tls")) continue;
        t.host = "127.0.0.1";
        t.port = stream->localPort();
        streams_.push_back(std::move(stream));
    }
    std::vector<juice_turn_server_t> turn;
    for (const TurnServer& t : config_.turn) {
        if (t.transport != "udp" && t.host != "127.0.0.1") continue;  // (its bridge could not be opened)
        juice_turn_server_t s{};
        s.host = t.host.c_str();
        s.port = static_cast<std::uint16_t>(t.port);
        s.username = t.username.c_str();
        s.password = t.password.c_str();
        turn.push_back(s);
    }
    juice_config_t c{};
    c.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    if (!config_.stunHost.empty()) {
        c.stun_server_host = config_.stunHost.c_str();
        c.stun_server_port = static_cast<std::uint16_t>(config_.stunPort);
    }
    c.turn_servers = turn.empty() ? nullptr : turn.data();
    c.turn_servers_count = static_cast<int>(turn.size());
    c.cb_state_changed = cbState;
    c.cb_gathering_done = cbGathered;
    c.cb_recv = cbRecv;
    c.user_ptr = this;
    agent_ = juice_create(&c);
    if (agent_ == nullptr) return false;
    state_.store(static_cast<int>(State::Gathering));
    if (juice_gather_candidates(agent_) != JUICE_ERR_SUCCESS) {
        state_.store(static_cast<int>(State::Failed));
        return false;
    }
    return true;
}

std::string Agent::localDescription() const {
    if (agent_ == nullptr || !gathered_.load()) return {};
    char buffer[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(agent_, buffer, sizeof buffer) != JUICE_ERR_SUCCESS) return {};
    return config_.relayOnly ? relayOnly(buffer) : std::string(buffer);
}

bool Agent::setRemoteDescription(const std::string& description) {
    if (agent_ == nullptr) return false;
    const std::string taken = config_.relayOnly ? relayOnly(description) : description;
    if (juice_set_remote_description(agent_, taken.c_str()) != JUICE_ERR_SUCCESS) return false;
    juice_set_remote_gathering_done(agent_);
    return true;
}

void Agent::onState(int state) {
    switch (static_cast<juice_state_t>(state)) {
        case JUICE_STATE_GATHERING: state_.store(static_cast<int>(State::Gathering)); break;
        case JUICE_STATE_CONNECTING: state_.store(static_cast<int>(State::Connecting)); break;
        case JUICE_STATE_CONNECTED:
        case JUICE_STATE_COMPLETED:
            wasConnected_.store(true);
            state_.store(static_cast<int>(State::Connected));
            break;
        case JUICE_STATE_FAILED: state_.store(static_cast<int>(State::Failed)); break;
        case JUICE_STATE_DISCONNECTED:
            // Before a connection this only means "not yet"; after one, that it is gone.
            if (wasConnected_.load()) state_.store(static_cast<int>(State::Failed));
            break;
    }
}

void Agent::onPacket(const char* data, std::size_t size) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (in_.size() >= kQueued) in_.pop_front();
    in_.emplace_back(reinterpret_cast<const std::uint8_t*>(data), reinterpret_cast<const std::uint8_t*>(data) + size);
}

Agent::State Agent::state() const { return static_cast<State>(state_.load()); }

void Agent::send(const std::vector<std::uint8_t>& packet) {
    if (agent_ != nullptr && state() == State::Connected) juice_send(agent_, reinterpret_cast<const char*>(packet.data()), packet.size());
}

bool Agent::receive(std::vector<std::uint8_t>& packet) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (in_.empty()) return false;
    packet = std::move(in_.front());
    in_.pop_front();
    return true;
}

bool Agent::alive() const { return agent_ != nullptr && state() != State::Failed; }

std::string Agent::remote() const {
    char local[JUICE_MAX_ADDRESS_STRING_LEN], remote[JUICE_MAX_ADDRESS_STRING_LEN];
    if (agent_ == nullptr || juice_get_selected_addresses(agent_, local, sizeof local, remote, sizeof remote) != JUICE_ERR_SUCCESS) return "unknown:0";
    return remote;
}

std::string Agent::kind() const {
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    if (agent_ == nullptr || juice_get_selected_candidates(agent_, local, sizeof local, remote, sizeof remote) != JUICE_ERR_SUCCESS) return "Direct";
    if (std::strstr(local, " typ relay") != nullptr && !streams_.empty()) return "Relay (TCP)";
    return std::strstr(local, " typ relay") != nullptr || std::strstr(remote, " typ relay") != nullptr ? "Relay" : "Direct";
}

TestServer::TestServer(const std::string& username, const std::string& password, const std::string& address) {
    quietOnce();
    juice_server_credentials_t credentials{};
    credentials.username = username.c_str();
    credentials.password = password.c_str();
    juice_server_config_t c{};
    c.credentials = &credentials;
    c.credentials_count = 1;
    c.max_allocations = 100;
    c.bind_address = address.c_str();
    c.realm = "test";
    server_ = juice_server_create(&c);
    if (server_ != nullptr) port_ = juice_server_get_port(server_);
}

TestServer::~TestServer() {
    if (server_ != nullptr) juice_server_destroy(server_);
}

}  // namespace ab::ice
