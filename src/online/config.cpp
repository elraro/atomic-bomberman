#include "online/config.hpp"

#include <fstream>

namespace ab::online {

bool makeBootstrap(const std::string& urls, const std::string& keyHex, Bootstrap& out, std::string& error) {
    Bootstrap b;
    std::string one;
    auto flush = [&]() {
        if (!one.empty()) b.urls.push_back(one);
        one.clear();
    };
    for (const char c : urls) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n') flush();
        else one += c;
    }
    flush();
    if (b.urls.empty()) {
        error = "no address of the configuration is set";
        return false;
    }
    for (const std::string& url : b.urls)
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
            error = "not an address: " + url;
            return false;
        }
    if (!fromHex(keyHex, b.key.data(), b.key.size())) {
        error = "the configuration's key must be 64 hexadecimal digits";
        return false;
    }
    out = std::move(b);
    return true;
}

bool readConfig(const std::string& envelope, const PublicKey& key, long long now, long long lowestSerial, OnlineConfig& out, std::string& error) {
    std::string document;
    if (!openDocument(envelope, {key}, &document, nullptr, &error)) return false;
    Json doc;
    if (!Json::parse(document, doc) || !doc.isObject()) {
        error = "the configuration is not readable";
        return false;
    }
    OnlineConfig c;
    c.serial = doc["serial"].integer();
    c.issued = doc["issued"].integer();
    c.expires = doc["expires"].integer();
    if (!doc["serial"].isNumber() || !doc["issued"].isNumber() || !doc["expires"].isNumber() || c.expires <= c.issued) {
        error = "the configuration carries no dates";
        return false;
    }
    if (c.expires - c.issued > kConfigLongestLife) {
        error = "the configuration was issued for longer than seven days";
        return false;
    }
    if (now >= c.expires) {
        error = "the configuration has expired";
        return false;
    }
    if (c.issued > now + 24 * 3600) {
        error = "the configuration is dated in the future (is this computer's clock right?)";
        return false;
    }
    if (c.serial < lowestSerial) {
        error = "the configuration is older than one already seen";
        return false;
    }
    const Json& master = doc["master"];
    c.masterUrl = master["url"].str();
    while (!c.masterUrl.empty() && c.masterUrl.back() == '/') c.masterUrl.pop_back();
    for (const Json& pin : master["tls_pins"].items())
        if (pin.isString()) c.masterPins.push_back(pin.str());
    if (c.masterUrl.rfind("https://", 0) != 0 && c.masterUrl.rfind("http://", 0) != 0) {
        error = "the configuration names no master server";
        return false;
    }
    if (c.masterUrl.rfind("https://", 0) == 0 && c.masterPins.empty()) {
        error = "the configuration gives no key for the master server";
        return false;
    }
    for (const Json& s : doc["stun"].items())
        if (s.isString()) c.stun.push_back(s.str());
    for (const Json& t : doc["turn"].items()) {
        TurnServer server;
        server.host = t["host"].str();
        server.port = static_cast<int>(t["port"].integer());
        server.transport = t["transport"].str();
        if (server.host.empty() || server.port < 1 || server.port > 65535) continue;
        if (server.transport != "udp" && server.transport != "tcp" && server.transport != "tls") continue;
        c.turn.push_back(server);
    }
    c.protocolMin = static_cast<int>(doc["protocol"]["min"].integer());
    c.protocolCurrent = static_cast<int>(doc["protocol"]["current"].integer());
    if (doc["status"]["state"].isString()) c.statusState = doc["status"]["state"].str();
    c.statusMessage = doc["status"]["message"].str();
    for (const Json& root : doc["trust"]["roots"].items()) {
        PublicKey k{};
        if (fromHex(root.str(), k.data(), k.size())) c.roots.push_back(k);
    }
    for (const Json& serial : doc["trust"]["revoked"].items())
        if (serial.isNumber()) c.revoked.push_back(serial.integer());
    c.flags = doc["flags"];
    out = std::move(c);
    return true;
}

long long readSerial(const std::string& file) {
    long long serial = 0;
    if (file.empty()) return 0;
    std::ifstream in(file);
    in >> serial;
    return in ? serial : 0;
}

void writeSerial(const std::string& file, long long serial) {
    if (file.empty()) return;
    std::ofstream out(file, std::ios::binary);
    out << serial << "\n";
}

}  // namespace ab::online
