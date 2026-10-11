#include "online/tournament.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

#include "online/http.hpp"
#include "online/json.hpp"

namespace ab::online {

namespace {
constexpr std::size_t kMaxPlayers = 1024;
constexpr std::size_t kMaxMatches = 4096;

std::string lowerHex(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

const TournamentPlayer* Tournament::player(const std::string& n) const {
    for (const TournamentPlayer& p : players)
        if (p.name == n) return &p;
    return nullptr;
}

const TournamentPlayer* Tournament::playerByKey(const PublicKey& key) const {
    for (const TournamentPlayer& p : players)
        if (p.key == key) return &p;
    return nullptr;
}

const TournamentServer* Tournament::server(const std::string& n) const {
    for (const TournamentServer& s : servers)
        if (s.name == n) return &s;
    return nullptr;
}

const TournamentMatch* Tournament::match(const std::string& id) const {
    for (const TournamentMatch& m : matches)
        if (m.id == id) return &m;
    return nullptr;
}

std::vector<const TournamentMatch*> Tournament::openMatchesOf(const PublicKey& key) const {
    std::vector<const TournamentMatch*> out;
    const TournamentPlayer* me = playerByKey(key);
    if (me == nullptr) return out;
    for (const TournamentMatch& m : matches)
        if (m.winner.empty() && std::find(m.players.begin(), m.players.end(), me->name) != m.players.end()) out.push_back(&m);
    std::stable_sort(out.begin(), out.end(), [](const TournamentMatch* a, const TournamentMatch* b) {
        if (a->round != b->round) return a->round < b->round;
        return (a->time == 0 ? INT64_MAX : a->time) < (b->time == 0 ? INT64_MAX : b->time);
    });
    return out;
}

bool Tournament::ready(const TournamentMatch& m) const {
    for (const std::string& who : m.players)
        if (who.empty() || who[0] == '@') return false;
    return server(m.server) != nullptr;
}

bool readTournament(const std::string& envelope, const PublicKey* trusted, long long lowestSerial, Tournament& out, std::string& error) {
    // Whoever signed it: the key the envelope names, unless a particular one is asked for.
    std::vector<PublicKey> keys;
    if (trusted != nullptr) {
        keys.push_back(*trusted);
    } else {
        Json env;
        PublicKey named{};
        if (!Json::parse(envelope, env) || !fromHex(env["key"].str(), named.data(), named.size())) {
            error = "not a signed document";
            return false;
        }
        keys.push_back(named);
    }
    std::string document;
    Tournament t;
    if (!openDocument(envelope, keys, &document, &t.organiserKey, &error)) {
        if (trusted != nullptr && error == "signed by a key that is not trusted") error = "not signed by the organiser whose key you hold";
        return false;
    }
    Json doc;
    if (!Json::parse(document, doc) || !doc.isObject() || doc["kind"].str() != "tournament") {
        error = "not a tournament file";
        return false;
    }
    t.name = doc["name"].str();
    t.organiser = doc["organiser"].str();
    t.serial = doc["serial"].integer();
    t.issued = doc["issued"].integer();
    if (t.name.empty() || !doc["serial"].isNumber()) {
        error = "the tournament file is incomplete";
        return false;
    }
    if (t.serial < lowestSerial) {
        error = "this is an older issue of the tournament than one already seen";
        return false;
    }
    const Json& rules = doc["rules"];
    t.rules.gameVersion = rules["game_version"].str();
    t.rules.protocol = static_cast<int>(rules["protocol"].integer());
    if (rules["wins"].isNumber()) t.rules.wins = std::clamp(static_cast<int>(rules["wins"].integer()), 1, 9);
    if (rules["level"].isNumber()) t.rules.level = std::clamp(static_cast<int>(rules["level"].integer()), -1, 10);
    if (rules["scheme"].isString()) t.rules.scheme = rules["scheme"].str();
    if (rules["play_time"].isNumber()) t.rules.playTime = std::clamp(static_cast<int>(rules["play_time"].integer()), 30, 1001);
    if (rules["enclosement"].isNumber()) t.rules.enclosement = std::clamp(static_cast<int>(rules["enclosement"].integer()), 0, 3);
    t.rules.teamPlay = rules["team_play"].boolean();
    if (rules["computers"].isNumber()) t.rules.computers = std::clamp(static_cast<int>(rules["computers"].integer()), 0, 9);

    std::set<std::string> names;
    if (doc["players"].items().size() > kMaxPlayers || doc["matches"].items().size() > kMaxMatches) {
        error = "the tournament file is too large";
        return false;
    }
    for (const Json& p : doc["players"].items()) {
        TournamentPlayer player;
        player.name = p["name"].str();
        if (player.name.empty() || player.name[0] == '@' || !fromHex(p["key"].str(), player.key.data(), player.key.size()) || !names.insert(player.name).second) {
            error = "the tournament file lists a player without a name of their own or without a key";
            return false;
        }
        for (const TournamentPlayer& other : t.players)
            if (other.key == player.key) {
                error = "the tournament file lists one key for two players";
                return false;
            }
        t.players.push_back(std::move(player));
    }
    std::set<std::string> serverNames;
    for (const Json& s : doc["servers"].items()) {
        TournamentServer server;
        server.name = s["name"].str();
        server.address = s["address"].str();
        server.identityKey = lowerHex(s["identity_key"].str());
        std::uint8_t check[32];
        if (server.name.empty() || server.address.empty() || !fromHex(server.identityKey, check, 32) || !fromHex(s["signing_key"].str(), server.signingKey.data(), 32) ||
            !serverNames.insert(server.name).second) {
            error = "the tournament file lists a server without a name, an address or its two keys";
            return false;
        }
        t.servers.push_back(std::move(server));
    }
    std::set<std::string> ids;
    for (const Json& m : doc["matches"].items()) ids.insert(m["id"].str());
    std::set<std::string> seen;
    for (const Json& m : doc["matches"].items()) {
        TournamentMatch match;
        match.id = m["id"].str();
        match.round = std::max(1, static_cast<int>(m["round"].integer()));
        match.server = m["server"].str();
        match.time = m["time"].integer();
        match.winner = m["winner"].str();
        if (match.id.empty() || match.id[0] == '@' || !seen.insert(match.id).second) {
            error = "the tournament file has a match without an id of its own";
            return false;
        }
        for (const Json& p : m["players"].items()) {
            const std::string who = p.str();
            const bool later = !who.empty() && who[0] == '@';
            if (who.empty() || (later ? ids.count(who.substr(1)) == 0 || who.substr(1) == match.id : names.count(who) == 0)) {
                error = "match " + match.id + " names a player the tournament does not list";
                return false;
            }
            match.players.push_back(who);
        }
        if (match.players.size() < 2 || match.players.size() > 10) {
            error = "match " + match.id + " needs two to ten players";
            return false;
        }
        if (!match.server.empty() && serverNames.count(match.server) == 0) {
            error = "match " + match.id + " names a server the tournament does not list";
            return false;
        }
        if (!match.winner.empty() && std::find(match.players.begin(), match.players.end(), match.winner) == match.players.end()) {
            error = "match " + match.id + " names a winner who did not play in it";
            return false;
        }
        for (const Json& r : m["records"].items()) {
            TournamentRecordRef ref;
            ref.sha256 = lowerHex(r["sha256"].str());
            ref.url = r["url"].str();
            std::uint8_t check[32];
            if (!fromHex(ref.sha256, check, 32)) {
                error = "match " + match.id + " names a record without its hash";
                return false;
            }
            match.records.push_back(std::move(ref));
        }
        t.matches.push_back(std::move(match));
    }
    out = std::move(t);
    return true;
}

void splitInvitation(const std::string& invitation, std::string& location, std::string& keyHex) {
    const std::size_t hash = invitation.rfind('#');
    location = invitation.substr(0, hash);
    keyHex = hash == std::string::npos ? std::string() : invitation.substr(hash + 1);
    for (std::string* part : {&location, &keyHex})
        while (!part->empty() && (part->back() == ' ' || part->back() == '\n' || part->back() == '\r')) part->pop_back();
}

bool fetchTournament(const std::string& location, std::string& envelope, std::string& error) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        HttpRequest q;
        q.url = location;  // from anywhere: what counts is the signature
        const HttpResponse r = httpFetch(q);
        if (!r.ok()) {
            error = r.status == 0 ? r.error : "the server answered " + std::to_string(r.status);
            return false;
        }
        envelope = r.body;
        return true;
    }
    std::ifstream in(location, std::ios::binary);
    if (!in) {
        error = "cannot read " + location;
        return false;
    }
    envelope.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string keyFingerprint(const PublicKey& key) {
    std::string hex = toHex(key.data(), 8);
    for (char& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
}

}  // namespace ab::online
