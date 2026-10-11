#include "online/certificate.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "online/json.hpp"

namespace ab::online {

bool readCertificate(const std::string& envelope, const std::vector<PublicKey>& roots, const std::vector<long long>& revoked, long long now,
                     const std::string& provenIdentityHex, ServerCertificate& out, std::string& error) {
    std::string document;
    if (!openDocument(envelope, roots, &document, nullptr, &error)) return false;
    Json doc;
    if (!Json::parse(document, doc) || !doc.isObject() || doc["kind"].str() != "server-certificate") {
        error = "not a server certificate";
        return false;
    }
    ServerCertificate c;
    c.serial = doc["serial"].integer();
    c.issued = doc["issued"].integer();
    c.expires = doc["expires"].integer();
    c.operatorName = doc["operator"]["name"].str();
    c.contact = doc["operator"]["contact"].str();
    c.identityKey = doc["identity_key"].str();
    for (char& ch : c.identityKey) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!doc["serial"].isNumber() || !doc["issued"].isNumber() || !doc["expires"].isNumber() || c.operatorName.empty() || c.identityKey.size() != 64 ||
        !fromHex(doc["signing_key"].str(), c.signingKey.data(), c.signingKey.size())) {
        error = "the certificate is incomplete";
        return false;
    }
    if (now >= c.expires) {
        error = "the certificate has expired";
        return false;
    }
    if (now + 24 * 3600 < c.issued) {
        error = "the certificate is dated in the future";
        return false;
    }
    if (std::find(revoked.begin(), revoked.end(), c.serial) != revoked.end()) {
        error = "the certificate has been revoked";
        return false;
    }
    std::string proven = provenIdentityHex;
    for (char& ch : proven) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (proven != c.identityKey) {
        error = "the certificate was made out to another server";
        return false;
    }
    out = std::move(c);
    return true;
}

bool loadOrCreateSeed(const std::string& path, Seed& seed, bool* created) {
    if (created != nullptr) *created = false;
    {
        std::ifstream in(path);
        std::string hex;
        if (in >> hex) return fromHex(hex, seed.data(), seed.size());
    }
    // New: from the system's random source, by way of the TLS library's generator.
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    static const unsigned char kPurpose[] = "atomic bomberman key";
    const bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, kPurpose, sizeof kPurpose) == 0 &&
                    mbedtls_ctr_drbg_random(&drbg, seed.data(), seed.size()) == 0;
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (!ok) return false;
    {
        std::ofstream out(path, std::ios::binary);
        out << toHex(seed) << "\n";
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    if (created != nullptr) *created = true;
    return true;
}

}  // namespace ab::online
