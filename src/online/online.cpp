#include "online/online.hpp"

#include <ctime>

namespace ab::online {

namespace {

LobbyInfo lobbyFrom(const Json& j) {
    LobbyInfo l;
    l.id = j["id"].str();
    l.name = j["name"].str();
    l.players = static_cast<int>(j["players"].integer());
    l.seats = static_cast<int>(j["seats"].integer());
    l.phase = j["phase"].str();
    l.level = j["level"].str();
    l.region = j["region"].str();
    l.version = static_cast<int>(j["version"].integer());
    l.certified = j["certified"].boolean();
    return l;
}

Json describe(const LobbyDesc& d) {
    Json j = Json::object();
    j.set("name", d.name).set("public", d.listed).set("players", d.players).set("seats", d.seats).set("phase", d.phase).set("level", d.level);
    j.set("region", d.region).set("identity_key", d.identityKey).set("certificate", d.certificate).set("port", d.port);
    Json candidates = Json::array();
    for (const std::string& c : d.candidates) candidates.push(c);
    j.set("candidates", candidates);
    return j;
}

// Only letters, digits and a few marks go into a path.
std::string pathSafe(const std::string& s) {
    std::string out;
    for (const char c : s)
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_') out += c;
    return out;
}

}  // namespace

void Online::configure(const Bootstrap& bootstrap, const std::string& serialFile, const std::string& playerName, int protocolVersion) {
    reset();
    bootstrap_ = bootstrap;
    serialFile_ = serialFile;
    name_ = playerName;
    version_ = protocolVersion;
}

void Online::reset() {
    job_.abandon();
    jobRunning_ = false;
    queue_.clear();
    haveConfig_ = false;
    config_ = OnlineConfig{};
    token_.clear();
    sessionId_.clear();
    error_.clear();
    lobbies_.clear();
    lobbiesTotal_ = 0;
    haveFound_ = haveJoin_ = false;
    lobbyId_.clear();
    hostToken_.clear();
    code_.clear();
    announcing_ = false;
}

// Before anything is asked of the master: the last error is forgotten and the session is
// seen to. False if there is no use trying (the reason is then in error_).
bool Online::begin() {
    error_.clear();
    connect();
    return error_.empty();
}

// A failure that makes everything waiting pointless.
void Online::dropQueue() {
    queue_.clear();
    announcing_ = false;
}

// True if the request did not do what was asked; `error` then says why in words fit for a screen.
bool Online::failed(const HttpResponse& r, Json& body, std::string& error) {
    if (r.status == 0) {
        error = "The master server cannot be reached (" + r.error + ")";
        return true;
    }
    Json parsed;
    if (Json::parse(r.body, parsed)) body = std::move(parsed);
    if (r.ok()) return false;
    error = body["error"].isString() ? body["error"].str() : "The master server answered " + std::to_string(r.status);
    return true;
}

HttpRequest Online::call(const std::string& method, const std::string& path, const Json* body, bool hostToken, int timeoutMs) const {
    HttpRequest q;
    q.method = method;
    q.url = config_.masterUrl + "/v1" + path;
    q.pins = config_.masterPins;
    q.timeoutMs = timeoutMs;
    if (!token_.empty()) q.headers.emplace_back("Authorization", "Bearer " + token_);
    if (hostToken) q.headers.emplace_back("X-Host-Token", hostToken_);
    if (body != nullptr) q.body = body->dump();
    return q;
}

// The document may be at several places; the first that gives a valid one will do.
void Online::fetchConfig(std::size_t urlIndex) {
    Op op;
    op.needsSession = false;
    op.make = [this, urlIndex]() {
        HttpRequest q;
        q.url = bootstrap_.urls[urlIndex];  // any key will do: the document is checked by its signature
        return q;
    };
    op.done = [this, urlIndex](const HttpResponse& r) {
        std::string why;
        const long long now = clock_ ? clock_() : static_cast<long long>(std::time(nullptr));
        OnlineConfig fresh;
        if (r.status == 0) why = r.error;
        else if (!r.ok()) why = "the server answered " + std::to_string(r.status);
        else if (readConfig(r.body, bootstrap_.key, now, readSerial(serialFile_), fresh, why)) {
            config_ = std::move(fresh);
            haveConfig_ = true;
            writeSerial(serialFile_, config_.serial);
            if (config_.statusState == "maintenance") {
                error_ = "Online play is down for maintenance" + (config_.statusMessage.empty() ? std::string() : ": " + config_.statusMessage);
                dropQueue();
                haveConfig_ = false;
            } else if (config_.protocolMin > version_) {
                error_ = "This version of the game is too old for online play";
                dropQueue();
                haveConfig_ = false;
            }
            return;
        }
        if (urlIndex + 1 < bootstrap_.urls.size()) {
            // Not from here: the next place, before anything else that is waiting.
            std::deque<Op> rest;
            rest.swap(queue_);
            fetchConfig(urlIndex + 1);
            for (Op& o : rest) queue_.push_back(std::move(o));
            return;
        }
        // Without a current configuration nothing online is done: none is kept from before.
        error_ = "Online play is not available: no valid configuration could be fetched (" + why + ")";
        dropQueue();
    };
    queue_.push_back(std::move(op));
}

void Online::openSession() {
    Op op;
    op.needsSession = false;
    op.make = [this]() {
        Json body = Json::object();
        body.set("name", name_).set("version", version_);
        return call("POST", "/session", &body);
    };
    op.done = [this](const HttpResponse& r) {
        Json body;
        if (failed(r, body, error_)) {
            dropQueue();
            return;
        }
        token_ = body["token"].str();
        sessionId_ = body["id"].str();
        if (token_.empty()) {
            error_ = "The master server gave no session";
            dropQueue();
        }
    };
    queue_.push_back(std::move(op));
}

void Online::connect() {
    if (!bootstrap_.valid()) {
        error_ = "Online play is not set up in this copy of the game (no configuration address)";
        return;
    }
    // A configuration is good until it expires; then a new one is fetched, and with it a new session.
    const long long now = clock_ ? clock_() : static_cast<long long>(std::time(nullptr));
    if (haveConfig_ && now >= config_.expires) {
        haveConfig_ = false;
        sessionLost();
    }
    if (ready()) return;
    // Already on its way?
    for (const Op& op : queue_)
        if (!op.needsSession) return;
    if (!haveConfig_) fetchConfig(0);
    openSession();
}

void Online::loadConfig() {
    if (!bootstrap_.valid()) return;
    const long long now = clock_ ? clock_() : static_cast<long long>(std::time(nullptr));
    if (haveConfig_ && now < config_.expires) return;
    for (const Op& op : queue_)
        if (!op.needsSession) return;  // on its way
    haveConfig_ = false;
    fetchConfig(0);
}

// The master has forgotten the session (it was restarted, or a day has passed): a new one next time.
void Online::sessionLost() {
    token_.clear();
    sessionId_.clear();
    lobbyId_.clear();
    hostToken_.clear();
    code_.clear();
}

void Online::update() {
    if (jobRunning_) {
        if (!job_.finished()) return;
        jobRunning_ = false;
        const HttpResponse r = job_.take();
        if (!queue_.empty()) {
            Op op = std::move(queue_.front());
            queue_.pop_front();
            if (r.status == 401 && op.needsSession) sessionLost();
            if (op.done) op.done(r);
        }
    }
    while (!queue_.empty() && !jobRunning_) {
        Op& op = queue_.front();
        if (op.needsSession && !ready()) {
            // Asked for before the session was there and that did not come about.
            if (error_.empty()) error_ = "Not connected to the master server";
            dropQueue();
            break;
        }
        job_.start(op.make());
        jobRunning_ = true;
    }
}

void Online::refreshLobbies() {
    if (!begin()) return;
    Op op;
    op.make = [this]() { return call("GET", "/lobbies", nullptr); };
    op.done = [this](const HttpResponse& r) {
        Json body;
        if (failed(r, body, error_)) return;
        lobbies_.clear();
        for (const Json& l : body["lobbies"].items()) lobbies_.push_back(lobbyFrom(l));
        lobbiesTotal_ = static_cast<int>(body["total"].integer());
        ++lobbiesSerial_;
    };
    queue_.push_back(std::move(op));
}

void Online::findByCode(const std::string& code) {
    haveFound_ = false;
    if (!begin()) return;
    Op op;
    op.make = [this, code]() { return call("GET", "/lobbies/by-code/" + pathSafe(code), nullptr); };
    op.done = [this](const HttpResponse& r) {
        Json body;
        if (failed(r, body, error_)) return;
        found_ = lobbyFrom(body);
        haveFound_ = true;
    };
    queue_.push_back(std::move(op));
}

bool Online::takeFound(LobbyInfo& out) {
    if (!haveFound_) return false;
    haveFound_ = false;
    out = found_;
    return true;
}

void Online::join(const std::string& lobbyId, const std::string& code) {
    haveJoin_ = false;
    if (!begin()) return;
    Op op;
    op.make = [this, lobbyId, code]() {
        Json body = Json::object();
        body.set("code", code);
        return call("POST", "/lobbies/" + pathSafe(lobbyId) + "/join", &body);
    };
    op.done = [this](const HttpResponse& r) {
        Json body;
        if (failed(r, body, error_)) return;
        JoinInfo j;
        j.lobby = lobbyFrom(body["lobby"]);
        for (const Json& c : body["candidates"].items())
            if (c.isString()) j.candidates.push_back(c.str());
        j.identityKey = body["identity_key"].str();
        j.certificate = body["certificate"].str();
        j.relayUser = body["relay"]["username"].str();
        j.relayPassword = body["relay"]["password"].str();
        for (const Json& u : body["relay"]["urls"].items())
            if (u.isString()) j.relayUrls.push_back(u.str());
        join_ = std::move(j);
        haveJoin_ = true;
    };
    queue_.push_back(std::move(op));
}

bool Online::takeJoin(JoinInfo& out) {
    if (!haveJoin_) return false;
    haveJoin_ = false;
    out = join_;
    return true;
}

void Online::announce(const LobbyDesc& desc) {
    if (announcing_) return;  // one at a time: the next call brings the newest description anyway
    if (!begin()) return;
    announcing_ = true;
    Op op;
    op.make = [this, desc]() {
        const Json body = describe(desc);
        if (lobbyId_.empty()) return call("POST", "/lobbies", &body);
        return call("PUT", "/lobbies/" + lobbyId_, &body, true);
    };
    op.done = [this, desc](const HttpResponse& r) {
        announcing_ = false;
        Json body;
        const bool wasUpdate = !lobbyId_.empty();
        if (failed(r, body, error_)) {
            // The master no longer knows the lobby (it was restarted, or we were silent too long): announce it anew next time.
            if (r.status == 404 && wasUpdate) lobbyId_.clear(), hostToken_.clear(), code_.clear(), error_.clear();
            return;
        }
        if (!wasUpdate) {
            lobbyId_ = body["id"].str();
            hostToken_ = body["host_token"].str();
        }
        if (body["code"].isString()) code_ = body["code"].str();
        announcedDesc_ = desc;
    };
    queue_.push_back(std::move(op));
}

void Online::withdraw() {
    if (lobbyId_.empty()) return;
    const std::string id = lobbyId_, token = hostToken_;
    lobbyId_.clear();
    code_.clear();
    Op op;
    op.make = [this, id, token]() {
        HttpRequest q = call("DELETE", "/lobbies/" + id, nullptr);
        q.headers.emplace_back("X-Host-Token", token);
        return q;
    };
    op.done = [](const HttpResponse&) {};
    queue_.push_back(std::move(op));
}

}  // namespace ab::online
