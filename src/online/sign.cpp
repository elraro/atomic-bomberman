#include "online/sign.hpp"

#include <cstring>

#include <mbedtls/sha256.h>

#include "monocypher-ed25519.h"
#include "online/json.hpp"

namespace ab::online {

PublicKey publicKeyOf(const Seed& seed) {
    std::uint8_t secret[64];
    std::uint8_t copy[32];
    std::memcpy(copy, seed.data(), 32);  // the library wipes the seed it is given
    PublicKey out{};
    crypto_ed25519_key_pair(secret, out.data(), copy);
    crypto_wipe(secret, sizeof secret);
    return out;
}

Signature sign(const Seed& seed, const std::string& message) {
    std::uint8_t secret[64];
    std::uint8_t pub[32];
    std::uint8_t copy[32];
    std::memcpy(copy, seed.data(), 32);
    crypto_ed25519_key_pair(secret, pub, copy);
    Signature out{};
    crypto_ed25519_sign(out.data(), secret, reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    crypto_wipe(secret, sizeof secret);
    return out;
}

bool verify(const PublicKey& key, const std::string& message, const Signature& signature) {
    return crypto_ed25519_check(signature.data(), key.data(), reinterpret_cast<const std::uint8_t*>(message.data()), message.size()) == 0;
}

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 15];
    }
    return out;
}

bool fromHex(const std::string& hex, std::uint8_t* out, std::size_t size) {
    if (hex.size() != size * 2) return false;
    for (std::size_t i = 0; i < size; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = hex[i * 2 + static_cast<std::size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        out[i] = static_cast<std::uint8_t>(v);
    }
    return true;
}

std::string toBase64(const std::string& bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = left > 1 ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = left > 2 ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += kAlphabet[a >> 2];
        out += kAlphabet[((a & 3) << 4) | (b >> 4)];
        out += left > 1 ? kAlphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += left > 2 ? kAlphabet[c & 63] : '=';
    }
    return out;
}

bool fromBase64(const std::string& text, std::string& bytes) {
    if (text.size() % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=' ? (text[i + 2] == '=' ? 2 : 1) : 0) : 0;
        int v[4] = {value(text[i]), value(text[i + 1]), pad >= 2 ? 0 : value(text[i + 2]), pad >= 1 ? 0 : value(text[i + 3])};
        if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0) return false;
        const unsigned n = (static_cast<unsigned>(v[0]) << 18) | (static_cast<unsigned>(v[1]) << 12) | (static_cast<unsigned>(v[2]) << 6) | static_cast<unsigned>(v[3]);
        out += static_cast<char>(n >> 16);
        if (pad < 2) out += static_cast<char>((n >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(n & 0xFF);
    }
    bytes = std::move(out);
    return true;
}

std::string sha256Hex(const std::uint8_t* data, std::size_t size) {
    std::uint8_t sum[32];
    mbedtls_sha256(data, size, sum, 0);
    return toHex(sum, sizeof sum);
}

std::string signDocument(const Seed& seed, const std::string& document) {
    const Signature s = sign(seed, document);
    Json envelope = Json::object();
    envelope.set("document", toBase64(document));
    envelope.set("signature", toBase64(std::string(reinterpret_cast<const char*>(s.data()), s.size())));
    envelope.set("key", toHex(publicKeyOf(seed)));
    return envelope.dump();
}

bool openDocument(const std::string& envelope, const std::vector<PublicKey>& trusted, std::string* document, PublicKey* signer, std::string* error) {
    auto no = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    Json env;
    if (!Json::parse(envelope, env) || !env.isObject()) return no("not a signed document");
    std::string doc, sig;
    PublicKey key{};
    if (!env["document"].isString() || !fromBase64(env["document"].str(), doc) || !fromBase64(env["signature"].str(), sig) || sig.size() != 64 ||
        !fromHex(env["key"].str(), key.data(), key.size()))
        return no("not a signed document");
    bool known = false;
    for (const PublicKey& t : trusted) known = known || t == key;
    if (!known) return no("signed by a key that is not trusted");
    Signature signature{};
    std::memcpy(signature.data(), sig.data(), 64);
    if (!verify(key, doc, signature)) return no("the signature does not match the document");
    if (document != nullptr) *document = std::move(doc);
    if (signer != nullptr) *signer = key;
    return true;
}

}  // namespace ab::online
