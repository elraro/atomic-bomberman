// The configuration of online play (docs/specifications/online-play.md, section 2):
// a signed document that says where the master server is, which relay to use and
// which keys may vouch for game servers. The game holds only a "bootstrap": where
// to fetch the document and the key that must have signed it. Nothing is cached:
// without a current, verified document there is no online play.
#pragma once

#include <string>
#include <vector>

#include "online/json.hpp"
#include "online/sign.hpp"

namespace ab::online {

struct Bootstrap {
    std::vector<std::string> urls;  // the document, at one or several places (mirrors)
    PublicKey key{};
    bool valid() const { return !urls.empty(); }
};
// From a list of addresses separated by commas or spaces, and the key in hex.
// False (with the reason) if either is malformed.
bool makeBootstrap(const std::string& urls, const std::string& keyHex, Bootstrap& out, std::string& error);

struct TurnServer {
    std::string host;
    int port = 3478;
    std::string transport = "udp";  // udp, tcp or tls
};

struct OnlineConfig {
    long long serial = 0, issued = 0, expires = 0;  // times in seconds since 1970
    std::string masterUrl;
    std::vector<std::string> masterPins;
    std::vector<std::string> stun;  // host:port
    std::vector<TurnServer> turn;
    int protocolMin = 0, protocolCurrent = 0;
    std::string statusState = "ok", statusMessage;
    std::vector<PublicKey> roots;       // keys that may sign server certificates
    std::vector<long long> revoked;     // serials of certificates no longer believed
    Json flags;
};

inline constexpr long long kConfigLongestLife = 7 * 24 * 3600;

// Checks an envelope as fetched: signed by `key`, well formed, issued for seven days at
// most, not expired at `now`, and with a serial not below `lowestSerial` (the highest one
// accepted before, so that an old document cannot be played back).
bool readConfig(const std::string& envelope, const PublicKey& key, long long now, long long lowestSerial, OnlineConfig& out, std::string& error);

// The highest serial accepted so far is the one thing remembered between runs (a number, not a document).
long long readSerial(const std::string& file);
void writeSerial(const std::string& file, long long serial);

}  // namespace ab::online
