#include "net/relay.hpp"

#include "net/crypto.hpp"
#include "net/protocol.hpp"

namespace ab::net {

namespace {
constexpr std::size_t kMaxHosts = 200;
constexpr std::size_t kMaxLinks = 1000;
constexpr std::size_t kMaxFresh = 200;
constexpr std::uint64_t kFreshMs = 5000;      // to say what one is
constexpr std::uint64_t kWaitingMs = 15000;   // for the host to pick a player up
constexpr std::uint64_t kHostSilenceMs = 45000;

std::string hostText(const Address& a) {
    const std::string text = a.text();
    return text.substr(0, text.rfind(':'));
}

std::uint32_t randomU32() {
    std::uint8_t raw[4];
    randomBytes(raw, 4);
    return static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) | (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24);
}

// Six characters that are easy to read out: no 0/O, 1/I.
std::string newCode() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    for (int i = 0; i < 6; ++i) code += kAlphabet[randomU32() % 32];
    return code;
}
}  // namespace

bool splitRelayAddress(const std::string& address, std::string* code, std::string* relay) {
    const auto at = address.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= address.size()) return false;
    *code = address.substr(0, at);
    for (char& ch : *code) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    *relay = address.substr(at + 1);
    return true;
}

Relay::Relay() = default;
Relay::~Relay() { stop(); }

void Relay::log(const std::string& line) const {
    if (log_) log_(line);
}

bool Relay::start(std::uint16_t port, std::function<void(const std::string&)> logger) {
    stop();
    log_ = std::move(logger);
    if (!startup() || !listener_.listen(port)) return false;
    log("INFO  Relay listening port=" + std::to_string(listener_.port()));
    return true;
}

void Relay::stop() {
    listener_.close();
    fresh_.clear();
    hosts_.clear();
    waiting_.clear();
    links_.clear();
}

void Relay::refuse(TcpSocket& socket, const std::string& why) {
    ByteWriter w;
    w.text(why);
    socket.send(static_cast<std::uint8_t>(RelayMsg::Refuse), w.data());
    socket.pump();
    socket.close();
}

void Relay::update(std::uint64_t nowMs) {
    while (auto socket = listener_.accept()) {
        std::deque<std::uint64_t>& recent = connects_[hostText(socket->peer())];
        recent.push_back(nowMs);
        while (!recent.empty() && nowMs - recent.front() > 10000) recent.pop_front();
        if (recent.size() > 40 || fresh_.size() >= kMaxFresh) continue;  // dropped
        fresh_.push_back({std::move(*socket), nowMs});
    }

    // Newcomers say what they are: a host, a player with a code, or a host fetching a player.
    for (std::size_t i = 0; i < fresh_.size();) {
        Fresh& f = fresh_[i];
        const bool alive = f.socket.pump();
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        bool done = !alive || nowMs - f.since > kFreshMs;
        if (alive && f.socket.receive(type, payload)) {
            done = true;
            ByteReader r(payload);
            const bool ours = r.u32() == kRelayMagic;
            if (!ours) {
                // Not for us.
            } else if (static_cast<RelayMsg>(type) == RelayMsg::Host) {
                if (hosts_.size() >= kMaxHosts) {
                    refuse(f.socket, "The relay is full");
                } else {
                    std::string code = newCode();
                    while (hosts_.count(code) != 0) code = newCode();
                    ByteWriter w;
                    w.text(code);
                    f.socket.send(static_cast<std::uint8_t>(RelayMsg::Hosted), w.data());
                    log("INFO  Host registered code=" + code + " from " + f.socket.peer().text());
                    hosts_[code] = HostEntry{std::move(f.socket), nowMs};
                }
            } else if (static_cast<RelayMsg>(type) == RelayMsg::Join) {
                const std::string code = r.text(16);
                const auto host = hosts_.find(code);
                if (!r.ok() || host == hosts_.end()) {
                    refuse(f.socket, "No game with that code on this relay");
                } else if (links_.size() + waiting_.size() >= kMaxLinks) {
                    refuse(f.socket, "The relay is full");
                } else {
                    std::uint32_t ticket = randomU32();
                    while (waiting_.count(ticket) != 0) ticket = randomU32();
                    ByteWriter w;
                    w.u32(ticket);
                    w.text(hostText(f.socket.peer()));  // so that the host's bans and limits see the real address
                    host->second.control.send(static_cast<std::uint8_t>(RelayMsg::Incoming), w.data());
                    waiting_[ticket] = Waiting{std::move(f.socket), nowMs};
                }
            } else if (static_cast<RelayMsg>(type) == RelayMsg::Accept) {
                const std::uint32_t ticket = r.u32();
                const auto waiting = waiting_.find(ticket);
                if (r.ok() && waiting != waiting_.end() && links_.size() < kMaxLinks) {
                    auto link = std::make_unique<Link>();
                    link->a = std::move(waiting->second.socket);
                    link->b = std::move(f.socket);
                    waiting_.erase(waiting);
                    links_.push_back(std::move(link));
                }
            }
        }
        if (done) fresh_.erase(fresh_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }

    // Hosts: kept while they say they are there.
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        HostEntry& h = it->second;
        const bool alive = h.control.pump();
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        while (h.control.receive(type, payload)) h.lastHeard = nowMs;
        if (!alive || nowMs - h.lastHeard > kHostSilenceMs) {
            log("INFO  Host gone code=" + it->first);
            it = hosts_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        // Nothing is read from a waiting player: what it has sent stays for the host.
        if (nowMs - it->second.since > kWaitingMs) {
            refuse(it->second.socket, "The host did not answer");
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }

    // Links: every frame from one side goes to the other as it came.
    for (std::size_t i = 0; i < links_.size();) {
        Link& l = *links_[i];
        const bool aAlive = l.a.pump(), bAlive = l.b.pump();
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        while (l.a.receive(type, payload)) l.b.send(type, payload);
        while (l.b.receive(type, payload)) l.a.send(type, payload);
        l.a.pump();
        l.b.pump();
        if (!aAlive || !bAlive || !l.a.open() || !l.b.open()) links_.erase(links_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

}  // namespace ab::net
