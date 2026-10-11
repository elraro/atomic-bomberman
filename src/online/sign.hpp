// Signatures (Ed25519, RFC 8032) and the signed documents of online play: the
// configuration, and later server certificates, match records and tournament
// files. The arithmetic is Monocypher's, not this project's own.
//
// A signed document travels as a small JSON envelope: the document's exact
// bytes (base64), the signature over them (base64), and the public key that
// signed (hex). Whoever reads one says which keys it believes.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ab::online {

using PublicKey = std::array<std::uint8_t, 32>;
using Seed = std::array<std::uint8_t, 32>;  // the private half
using Signature = std::array<std::uint8_t, 64>;

PublicKey publicKeyOf(const Seed& seed);
Signature sign(const Seed& seed, const std::string& message);
bool verify(const PublicKey& key, const std::string& message, const Signature& signature);

std::string toHex(const std::uint8_t* data, std::size_t size);
template <std::size_t N>
std::string toHex(const std::array<std::uint8_t, N>& a) {
    return toHex(a.data(), N);
}
bool fromHex(const std::string& hex, std::uint8_t* out, std::size_t size);  // exactly `size` bytes
std::string toBase64(const std::string& bytes);
bool fromBase64(const std::string& text, std::string& bytes);
// SHA-256 as hex (for the pin of a TLS key and the hash of a file).
std::string sha256Hex(const std::uint8_t* data, std::size_t size);

std::string signDocument(const Seed& seed, const std::string& document);
// The document, if the envelope is well formed and signed by one of `trusted`. Otherwise
// false, with the reason in *error.
bool openDocument(const std::string& envelope, const std::vector<PublicKey>& trusted, std::string* document, PublicKey* signer, std::string* error);

}  // namespace ab::online
