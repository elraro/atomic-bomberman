// The cryptography of the network mode: standard algorithms written out here so
// that the game needs no extra library. X25519 (RFC 7748) to agree on a secret,
// ChaCha20-Poly1305 (RFC 8439) to encrypt and authenticate every message,
// SHA-256 and HMAC (FIPS 180-4, RFC 2104) to derive keys and prove knowledge
// of the password. Each is checked against its published test vectors in the
// tests. Not audited; good enough to keep a game's traffic private and
// untampered, not a basis for anything more valuable.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ab::net {

using Key = std::array<std::uint8_t, 32>;
using Bytes = std::vector<std::uint8_t>;

Key sha256(const std::uint8_t* data, std::size_t size);
inline Key sha256(const Bytes& data) { return sha256(data.data(), data.size()); }
Key hmacSha256(const Key& key, const std::uint8_t* data, std::size_t size);

// X25519: public = x25519Base(secret); shared = x25519(own secret, their public).
Key x25519(const Key& secret, const Key& point);
Key x25519Base(const Key& secret);

// Raw ChaCha20 keystream block and Poly1305 tag, for the tests.
std::array<std::uint8_t, 64> chacha20Block(const Key& key, std::uint32_t counter, const std::array<std::uint8_t, 12>& nonce);
std::array<std::uint8_t, 16> poly1305(const Key& key, const std::uint8_t* data, std::size_t size);

// Authenticated encryption. The result of seal is the ciphertext followed by a 16-byte
// tag. open returns false (and leaves `plain` empty) if anything was altered.
Bytes seal(const Key& key, const std::array<std::uint8_t, 12>& nonce, const std::uint8_t* aad, std::size_t aadSize, const std::uint8_t* plain, std::size_t size);
bool open(const Key& key, const std::array<std::uint8_t, 12>& nonce, const std::uint8_t* aad, std::size_t aadSize, const std::uint8_t* sealed, std::size_t size, Bytes& plain);
// The nonce for message number `counter` of a direction.
std::array<std::uint8_t, 12> counterNonce(std::uint64_t counter);

// Unpredictable bytes from the operating system.
void randomBytes(std::uint8_t* out, std::size_t size);
bool equalConstantTime(const std::uint8_t* a, const std::uint8_t* b, std::size_t size);

// The keys of one connection, derived from the key exchange.
struct SessionKeys {
    Key master{};      // from the shared secret and both public keys
    Key toServer{};    // TCP, client to server
    Key toClient{};    // TCP, server to client
    Key udpToServer{};
    Key udpToClient{};
};
// False if the shared secret is degenerate (a hostile public key).
bool deriveSession(const Key& ownSecret, const Key& theirPublic, const Key& clientPublic, const Key& serverPublic, SessionKeys& out);
// What a client sends instead of the password: it shows knowledge of the password for this
// connection only, and tells an eavesdropper nothing.
Key passwordProof(const std::string& password, const Key& master);

}  // namespace ab::net
