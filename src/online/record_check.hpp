// Judging a signed match record (docs/specifications/online-play.md, section 7): who
// signed it, whether a certificate vouches for that key, whether the round plays again
// to the recorded result, and, for a player's own game, whether the record agrees with
// what that game knows at first hand. Used by the game, the test client and
// atomic_verify. Header only: it needs both the network and the online library.
#pragma once

#include <string>
#include <vector>

#include "net/client.hpp"
#include "net/record.hpp"
#include "online/certificate.hpp"
#include "online/json.hpp"
#include "online/sign.hpp"

namespace ab::online {

struct RecordReport {
    bool readable = false;             // a signed document holding a record
    bool holds = false;                // nothing contradicts it
    net::MatchRecord record;
    PublicKey signer{};
    bool certified = false;            // a certificate that counts names the signer
    std::string operatorName;
    std::vector<std::string> problems;
    std::vector<std::string> notes;
};

inline bool verifyBytes(const net::Key& key, const std::vector<std::uint8_t>& message, const net::SignatureBytes& signature) {
    PublicKey k{};
    std::copy(key.begin(), key.end(), k.begin());
    return verify(k, std::string(message.begin(), message.end()), signature);
}

// `roots` and `revoked`: from the configuration; without roots no certificate can count.
// `own`: the client that played the round, to hold the record against (may be null).
inline RecordReport examineRecord(const std::string& envelope, const std::string& gameVersion, const std::vector<PublicKey>& roots, const std::vector<long long>& revoked,
                                  const net::Client* own = nullptr) {
    RecordReport out;
    auto problem = [&](const std::string& text) { out.problems.push_back(text); };
    // Whoever signed it: the key the envelope names, checked against its own signature.
    Json env;
    PublicKey named{};
    std::string document, why;
    if (!Json::parse(envelope, env) || !fromHex(env["key"].str(), named.data(), named.size()) || !openDocument(envelope, {named}, &document, &out.signer, &why)) {
        problem("not a signed record (" + (why.empty() ? std::string("unreadable") : why) + ")");
        return out;
    }
    if (!net::decodeRecord(std::vector<std::uint8_t>(document.begin(), document.end()), out.record)) {
        problem("signed, but not a match record");
        return out;
    }
    out.readable = true;
    const net::MatchRecord& r = out.record;
    // Does a certificate vouch for the key that signed? Judged as of the day the round was played.
    if (r.certificate.empty()) {
        out.notes.push_back("the server showed no certificate: nobody answers for this record");
    } else if (roots.empty()) {
        out.notes.push_back("the server's certificate was not checked (no root keys at hand)");
    } else {
        ServerCertificate cert;
        if (!readCertificate(r.certificate, roots, revoked, static_cast<long long>(r.startedUnix), "", cert, why)) problem("the server's certificate does not count: " + why);
        else if (cert.signingKey != out.signer) problem("the record is not signed with the key its server's certificate names");
        else out.certified = true, out.operatorName = cert.operatorName;
    }
    const net::RecordVerdict verdict = net::checkRecord(r, gameVersion, verifyBytes);
    out.problems.insert(out.problems.end(), verdict.problems.begin(), verdict.problems.end());
    out.notes.insert(out.notes.end(), verdict.notes.begin(), verdict.notes.end());
    // A player's own game knows three things at first hand: what it pressed, the number it
    // gave towards the seed, and the promises it was shown before giving it.
    if (own != nullptr && own->recordedRound() == r.start.roundId) {
        bool logFound = false;
        for (const net::PlayerLog& p : r.players) {
            if (p.client != own->id()) continue;
            logFound = true;
            if (p.values != own->ownLog()) problem("the record's log of your inputs is not the one this game kept");
        }
        if (!logFound && own->seat() >= 0 && !own->ownLog().empty()) problem("your inputs are missing from the record");
        if (!own->seenCommits().empty()) {
            bool ownShare = false;
            for (const net::SeedShare& s : r.shares) ownShare = ownShare || (s.client == own->id() && s.revealed && s.reveal == own->ownSeedReveal());
            if (!ownShare) problem("your share in the seed is not in the record as you gave it");
            for (const auto& [client, commit] : own->seenCommits()) {
                bool there = false;
                for (const net::SeedShare& s : r.shares) there = there || (s.client == client && s.commit == commit);
                if (!there) problem("the record's seed shares are not the ones you were shown before the round");
            }
            if (r.shares.size() != own->seenCommits().size()) problem("the record names other seed shares than you were shown before the round");
        }
    }
    out.holds = out.problems.empty();
    return out;
}

}  // namespace ab::online
