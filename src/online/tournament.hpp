// Tournaments as signed files (docs/specifications/online-play.md, section 7): an
// organiser signs a document that names the rules, the players (by their keys), the
// game servers (by theirs), who plays whom where, and the results so far. It is
// published anywhere and issued again with a higher serial as results come in. No
// master server and no accounts are involved; whose organiser key to believe is the
// player's own choice.
#pragma once

#include <string>
#include <vector>

#include "online/sign.hpp"

namespace ab::online {

struct TournamentRules {
    std::string gameVersion;  // the release the matches are played (and their records judged) with
    int protocol = 0;
    int wins = 2;             // round wins that decide a match
    int level = 0;            // 0-10, -1 a random one each match
    std::string scheme = "basic";
    int playTime = 150;
    int enclosement = 1;
    bool teamPlay = false;
    int computers = 0;
};

struct TournamentPlayer {
    std::string name;
    PublicKey key{};
};

struct TournamentServer {
    std::string name;
    std::string address;      // host:port
    std::string identityKey;  // hex: what the server proves in every key exchange
    PublicKey signingKey{};   // what it signs match records with
};

struct TournamentRecordRef {
    std::string sha256;  // of the signed record as published (hex)
    std::string url;     // where to fetch it; may be empty
};

struct TournamentMatch {
    std::string id;
    int round = 1;
    // Names of players, or "@ID" for "the winner of match ID" where that is not known yet.
    std::vector<std::string> players;
    std::string server;   // a server's name; empty: not set yet
    long long time = 0;   // when it is to be played (seconds since 1970); 0: not set
    std::string winner;   // a player's name once played; empty before
    std::vector<TournamentRecordRef> records;
};

struct Tournament {
    std::string name, organiser;
    long long serial = 0, issued = 0;
    TournamentRules rules;
    std::vector<TournamentPlayer> players;
    std::vector<TournamentServer> servers;
    std::vector<TournamentMatch> matches;
    PublicKey organiserKey{};  // who signed it

    const TournamentPlayer* player(const std::string& name) const;
    const TournamentPlayer* playerByKey(const PublicKey& key) const;
    const TournamentServer* server(const std::string& name) const;
    const TournamentMatch* match(const std::string& id) const;
    // The matches a player (by key) is named in and that have no winner yet, soonest first.
    std::vector<const TournamentMatch*> openMatchesOf(const PublicKey& key) const;
    // Can it be played now: everybody in it is known (nobody is still "the winner of" another
    // match) and it has a server?
    bool ready(const TournamentMatch& m) const;
};

// The tournament in `envelope` if it is well formed and consistent (every match names
// players and a server the file lists), signed by `trusted` if one is given (otherwise by
// whoever signed it: the caller then decides whether to believe that key), and with a
// serial not below `lowestSerial`. Otherwise false and the reason.
bool readTournament(const std::string& envelope, const PublicKey* trusted, long long lowestSerial, Tournament& out, std::string& error);

// "location#key": where the file is (a path, or an http(s) address) and the organiser's key
// in hex. The key part may be missing (the player is then asked whether to believe the signer).
void splitInvitation(const std::string& invitation, std::string& location, std::string& keyHex);
// Reads the file or fetches the address (blocks).
bool fetchTournament(const std::string& location, std::string& envelope, std::string& error);

// A key as people compare it: "3F9A-11C0-7B42-E5D8".
std::string keyFingerprint(const PublicKey& key);

}  // namespace ab::online
