// Server certificates and key files (docs/specifications/online-play.md, section 7).
// A certificate says who answers for a game server: an offline root key signs the
// server's two keys together with its operator's name. It does not say that the
// server is honest; it says whom to hold to account.
#pragma once

#include <string>
#include <vector>

#include "online/sign.hpp"

namespace ab::online {

struct ServerCertificate {
    long long serial = 0, issued = 0, expires = 0;
    std::string operatorName, contact;
    std::string identityKey;  // hex of the X25519 key the server proves in every key exchange
    PublicKey signingKey{};   // the Ed25519 key it signs match records with
};

// The certificate in `envelope`, if it is one that counts: signed by one of `roots`, of the
// right kind, within its dates at `now`, not among the `revoked` serials, and made out to
// the identity key the server has just proved (`provenIdentityHex`; empty where no
// connection is at hand, as when a record is read later: what counts there is the signing
// key). Otherwise false, and `error` says which of these failed.
bool readCertificate(const std::string& envelope, const std::vector<PublicKey>& roots, const std::vector<long long>& revoked, long long now,
                     const std::string& provenIdentityHex, ServerCertificate& out, std::string& error);

// A private key kept in a file (64 hexadecimal digits): read, or made and written if the
// file is not there. False if the file cannot be read as a key or cannot be written.
// The file is made readable by its owner only where the system has such a notion.
bool loadOrCreateSeed(const std::string& path, Seed& seed, bool* created = nullptr);

}  // namespace ab::online
