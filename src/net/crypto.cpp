#include "net/crypto.hpp"

#include <cstring>
#include <random>

namespace ab::net {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
u32 rotr(u32 v, int n) { return (v >> n) | (v << (32 - n)); }
u32 le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
void put64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

// --- SHA-256 -----------------------------------------------------------------

const u32 kSha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void shaBlock(u32 h[8], const u8* block) {
    u32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<u32>(block[4 * i]) << 24) | (static_cast<u32>(block[4 * i + 1]) << 16) | (static_cast<u32>(block[4 * i + 2]) << 8) | block[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha[i] + w[i];
        const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

// --- Curve25519 field arithmetic: 16 limbs of 16 bits (after TweetNaCl's layout) ---

using Gf = i64[16];
const i64 k121665[16] = {0xDB41, 1};

void carry(i64* o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1 + 37 * (c - 1) * (i == 15 ? 1 : 0);
        o[i] -= c * 65536;
    }
}
void select(i64* p, i64* q, int b) {
    const i64 c = ~(static_cast<i64>(b) - 1);
    for (int i = 0; i < 16; ++i) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
void pack(u8* o, const i64* n) {
    Gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<u8>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<u8>(t[i] >> 8);
    }
}
void unpack(i64* o, const u8* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<i64>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
void add(i64* o, const i64* a, const i64* b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
void sub(i64* o, const i64* a, const i64* b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
void mul(i64* o, const i64* a, const i64* b) {
    i64 t[31];
    for (i64& v : t) v = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o);
    carry(o);
}
void invert(i64* o, const i64* in) {
    Gf c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 253; a >= 0; --a) {
        mul(c, c, c);
        if (a != 2 && a != 4) mul(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

// --- ChaCha20 ----------------------------------------------------------------

void quarter(u32* s, int a, int b, int c, int d) {
    s[a] += s[b], s[d] = rotl(s[d] ^ s[a], 16);
    s[c] += s[d], s[b] = rotl(s[b] ^ s[c], 12);
    s[a] += s[b], s[d] = rotl(s[d] ^ s[a], 8);
    s[c] += s[d], s[b] = rotl(s[b] ^ s[c], 7);
}

void chachaXor(const Key& key, const std::array<u8, 12>& nonce, u32 counter, const u8* in, u8* out, std::size_t size) {
    for (std::size_t done = 0; done < size; done += 64, ++counter) {
        const std::array<u8, 64> block = chacha20Block(key, counter, nonce);
        for (std::size_t i = 0; i < 64 && done + i < size; ++i) out[done + i] = static_cast<u8>(in[done + i] ^ block[i]);
    }
}

// The message Poly1305 signs in the combined mode: associated data and ciphertext, each
// padded to 16 bytes, then both lengths.
std::array<u8, 16> aeadTag(const Key& key, const std::array<u8, 12>& nonce, const u8* aad, std::size_t aadSize, const u8* cipher, std::size_t size) {
    const std::array<u8, 64> first = chacha20Block(key, 0, nonce);
    Key polyKey;
    std::memcpy(polyKey.data(), first.data(), 32);
    Bytes message;
    message.reserve(aadSize + size + 48);
    message.insert(message.end(), aad, aad + aadSize);
    message.resize((message.size() + 15) / 16 * 16, 0);
    message.insert(message.end(), cipher, cipher + size);
    message.resize((message.size() + 15) / 16 * 16, 0);
    u8 lengths[16];
    put64(lengths, aadSize);
    put64(lengths + 8, size);
    message.insert(message.end(), lengths, lengths + 16);
    return poly1305(polyKey, message.data(), message.size());
}

}  // namespace

Key sha256(const std::uint8_t* data, std::size_t size) {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t done = 0;
    for (; done + 64 <= size; done += 64) shaBlock(h, data + done);
    u8 last[128] = {0};
    const std::size_t rest = size - done;
    if (rest > 0) std::memcpy(last, data + done, rest);
    last[rest] = 0x80;
    const std::size_t total = rest < 56 ? 64 : 128;
    const u64 bits = static_cast<u64>(size) * 8;
    for (int i = 0; i < 8; ++i) last[total - 1 - static_cast<std::size_t>(i)] = static_cast<u8>(bits >> (8 * i));
    shaBlock(h, last);
    if (total == 128) shaBlock(h, last + 64);
    Key out;
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 4; ++j) out[4 * i + j] = static_cast<u8>(h[i] >> (24 - 8 * j));
    return out;
}

Key hmacSha256(const Key& key, const std::uint8_t* data, std::size_t size) {
    Bytes inner(64, 0x36), outer(64, 0x5c);
    for (std::size_t i = 0; i < 32; ++i) inner[i] ^= key[i], outer[i] ^= key[i];
    inner.insert(inner.end(), data, data + size);
    const Key first = sha256(inner);
    outer.insert(outer.end(), first.begin(), first.end());
    return sha256(outer);
}

Key x25519(const Key& secret, const Key& point) {
    u8 z[32];
    std::memcpy(z, secret.data(), 32);
    z[31] = static_cast<u8>((z[31] & 127) | 64);
    z[0] &= 248;
    i64 x[80];
    Gf a, b, c, d, e, f;
    unpack(x, point.data());
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        select(a, b, r);
        select(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        mul(d, e, e);
        mul(f, a, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        mul(b, a, a);
        sub(c, d, f);
        mul(a, c, k121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        mul(b, e, e);
        select(a, b, r);
        select(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    invert(x + 32, x + 32);
    mul(x + 16, x + 16, x + 32);
    Key out;
    pack(out.data(), x + 16);
    return out;
}

Key x25519Base(const Key& secret) {
    Key base{};
    base[0] = 9;
    return x25519(secret, base);
}

std::array<std::uint8_t, 64> chacha20Block(const Key& key, std::uint32_t counter, const std::array<std::uint8_t, 12>& nonce) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) s[4 + i] = le32(key.data() + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = le32(nonce.data() + 4 * i);
    u32 w[16];
    std::memcpy(w, s, sizeof w);
    for (int round = 0; round < 10; ++round) {
        quarter(w, 0, 4, 8, 12);
        quarter(w, 1, 5, 9, 13);
        quarter(w, 2, 6, 10, 14);
        quarter(w, 3, 7, 11, 15);
        quarter(w, 0, 5, 10, 15);
        quarter(w, 1, 6, 11, 12);
        quarter(w, 2, 7, 8, 13);
        quarter(w, 3, 4, 9, 14);
    }
    std::array<u8, 64> out;
    for (int i = 0; i < 16; ++i) put32(out.data() + 4 * i, w[i] + s[i]);
    return out;
}

std::array<std::uint8_t, 16> poly1305(const Key& key, const std::uint8_t* m, std::size_t size) {
    // Arithmetic modulo 2^130 - 5 in five limbs of 26 bits.
    const u32 r0 = le32(key.data()) & 0x3ffffff, r1 = (le32(key.data() + 3) >> 2) & 0x3ffff03, r2 = (le32(key.data() + 6) >> 4) & 0x3ffc0ff,
              r3 = (le32(key.data() + 9) >> 6) & 0x3f03fff, r4 = (le32(key.data() + 12) >> 8) & 0x00fffff;
    const u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (size > 0) {
        u8 block[17] = {0};
        const std::size_t n = size < 16 ? size : 16;
        std::memcpy(block, m, n);
        block[n] = 1;  // the bit above the message bytes
        h0 += le32(block) & 0x3ffffff;
        h1 += (le32(block + 3) >> 2) & 0x3ffffff;
        h2 += (le32(block + 6) >> 4) & 0x3ffffff;
        h3 += (le32(block + 9) >> 6) & 0x3ffffff;
        h4 += (le32(block + 12) >> 8) | (static_cast<u32>(block[16]) << 24);
        const u64 d0 = static_cast<u64>(h0) * r0 + static_cast<u64>(h1) * s4 + static_cast<u64>(h2) * s3 + static_cast<u64>(h3) * s2 + static_cast<u64>(h4) * s1;
        u64 d1 = static_cast<u64>(h0) * r1 + static_cast<u64>(h1) * r0 + static_cast<u64>(h2) * s4 + static_cast<u64>(h3) * s3 + static_cast<u64>(h4) * s2;
        u64 d2 = static_cast<u64>(h0) * r2 + static_cast<u64>(h1) * r1 + static_cast<u64>(h2) * r0 + static_cast<u64>(h3) * s4 + static_cast<u64>(h4) * s3;
        u64 d3 = static_cast<u64>(h0) * r3 + static_cast<u64>(h1) * r2 + static_cast<u64>(h2) * r1 + static_cast<u64>(h3) * r0 + static_cast<u64>(h4) * s4;
        u64 d4 = static_cast<u64>(h0) * r4 + static_cast<u64>(h1) * r3 + static_cast<u64>(h2) * r2 + static_cast<u64>(h3) * r1 + static_cast<u64>(h4) * r0;
        u64 c = d0 >> 26;
        h0 = static_cast<u32>(d0) & 0x3ffffff;
        d1 += c, c = d1 >> 26, h1 = static_cast<u32>(d1) & 0x3ffffff;
        d2 += c, c = d2 >> 26, h2 = static_cast<u32>(d2) & 0x3ffffff;
        d3 += c, c = d3 >> 26, h3 = static_cast<u32>(d3) & 0x3ffffff;
        d4 += c, c = d4 >> 26, h4 = static_cast<u32>(d4) & 0x3ffffff;
        h0 += static_cast<u32>(c) * 5;
        h1 += h0 >> 26;
        h0 &= 0x3ffffff;
        m += n;
        size -= n;
    }
    // Full carry, then h mod (2^130 - 5).
    u32 c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c, c = h2 >> 26, h2 &= 0x3ffffff;
    h3 += c, c = h3 >> 26, h3 &= 0x3ffffff;
    h4 += c, c = h4 >> 26, h4 &= 0x3ffffff;
    h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
    h1 += c;
    u32 g0 = h0 + 5;
    c = g0 >> 26, g0 &= 0x3ffffff;
    u32 g1 = h1 + c;
    c = g1 >> 26, g1 &= 0x3ffffff;
    u32 g2 = h2 + c;
    c = g2 >> 26, g2 &= 0x3ffffff;
    u32 g3 = h3 + c;
    c = g3 >> 26, g3 &= 0x3ffffff;
    const u32 g4 = h4 + c - (1u << 26);
    const u32 useG = (g4 >> 31) - 1;  // all ones if h >= p
    h0 = (h0 & ~useG) | (g0 & useG);
    h1 = (h1 & ~useG) | (g1 & useG);
    h2 = (h2 & ~useG) | (g2 & useG);
    h3 = (h3 & ~useG) | (g3 & useG);
    h4 = (h4 & ~useG) | (g4 & useG);
    // Back to 32-bit words, plus the second half of the key.
    const u32 w0 = h0 | (h1 << 26), w1 = (h1 >> 6) | (h2 << 20), w2 = (h2 >> 12) | (h3 << 14), w3 = (h3 >> 18) | (h4 << 8);
    u64 f = static_cast<u64>(w0) + le32(key.data() + 16);
    std::array<u8, 16> tag;
    put32(tag.data(), static_cast<u32>(f));
    f = static_cast<u64>(w1) + le32(key.data() + 20) + (f >> 32);
    put32(tag.data() + 4, static_cast<u32>(f));
    f = static_cast<u64>(w2) + le32(key.data() + 24) + (f >> 32);
    put32(tag.data() + 8, static_cast<u32>(f));
    f = static_cast<u64>(w3) + le32(key.data() + 28) + (f >> 32);
    put32(tag.data() + 12, static_cast<u32>(f));
    return tag;
}

Bytes seal(const Key& key, const std::array<std::uint8_t, 12>& nonce, const std::uint8_t* aad, std::size_t aadSize, const std::uint8_t* plain, std::size_t size) {
    Bytes out(size + 16);
    chachaXor(key, nonce, 1, plain, out.data(), size);
    const std::array<u8, 16> tag = aeadTag(key, nonce, aad, aadSize, out.data(), size);
    std::memcpy(out.data() + size, tag.data(), 16);
    return out;
}

bool open(const Key& key, const std::array<std::uint8_t, 12>& nonce, const std::uint8_t* aad, std::size_t aadSize, const std::uint8_t* sealed, std::size_t size, Bytes& plain) {
    plain.clear();
    if (size < 16) return false;
    const std::array<u8, 16> tag = aeadTag(key, nonce, aad, aadSize, sealed, size - 16);
    if (!equalConstantTime(tag.data(), sealed + size - 16, 16)) return false;
    plain.resize(size - 16);
    chachaXor(key, nonce, 1, sealed, plain.data(), size - 16);
    return true;
}

std::array<std::uint8_t, 12> counterNonce(std::uint64_t counter) {
    std::array<u8, 12> nonce{};
    put64(nonce.data() + 4, counter);
    return nonce;
}

void randomBytes(std::uint8_t* out, std::size_t size) {
    std::random_device device;  // the operating system's source on the supported platforms
    for (std::size_t i = 0; i < size; i += 4) {
        const u32 v = device();
        for (std::size_t j = 0; j < 4 && i + j < size; ++j) out[i + j] = static_cast<u8>(v >> (8 * j));
    }
}

bool equalConstantTime(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) {
    u8 diff = 0;
    for (std::size_t i = 0; i < size; ++i) diff = static_cast<u8>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

bool deriveSession(const Key& ownSecret, const Key& theirPublic, const Key& clientPublic, const Key& serverPublic, const Key& identityShared,
                   const Key& identityPublic, SessionKeys& out) {
    const Key shared = x25519(ownSecret, theirPublic);
    u8 zero = 0, zeroIdentity = 0;
    for (u8 b : shared) zero |= b;
    for (u8 b : identityShared) zeroIdentity |= b;
    if (zero == 0 || zeroIdentity == 0) return false;
    Bytes material(shared.begin(), shared.end());
    material.insert(material.end(), identityShared.begin(), identityShared.end());
    material.insert(material.end(), identityPublic.begin(), identityPublic.end());
    const char* label = "atomic-bomberman-modern net 5";
    material.insert(material.end(), label, label + std::strlen(label));
    material.insert(material.end(), clientPublic.begin(), clientPublic.end());
    material.insert(material.end(), serverPublic.begin(), serverPublic.end());
    out.master = sha256(material);
    auto sub = [&](u8 which) {
        Bytes m(out.master.begin(), out.master.end());
        m.push_back(which);
        return sha256(m);
    };
    out.toServer = sub(1);
    out.toClient = sub(2);
    out.udpToServer = sub(3);
    out.udpToClient = sub(4);
    return true;
}

std::string toHex(const Key& key) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    for (u8 b : key) out += kDigits[b >> 4], out += kDigits[b & 15];
    return out;
}

bool fromHex(const std::string& hex, Key& key) {
    if (hex.size() != 64) return false;
    for (std::size_t i = 0; i < 32; ++i) {
        int v = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char ch = hex[2 * i + j];
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        key[i] = static_cast<u8>(v);
    }
    return true;
}

std::string fingerprint(const Key& identityPublic) {
    const Key hash = sha256(identityPublic.data(), identityPublic.size());
    static const char* const kDigits = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < 8; ++i) {
        if (i > 0 && i % 2 == 0) out += '-';
        out += kDigits[hash[i] >> 4];
        out += kDigits[hash[i] & 15];
    }
    return out;
}

Key passwordProof(const std::string& password, const Key& master) {
    Bytes salted;
    const char* label = "atomic-bomberman-modern password";
    salted.insert(salted.end(), label, label + std::strlen(label));
    salted.insert(salted.end(), password.begin(), password.end());
    return hmacSha256(sha256(salted), master.data(), master.size());
}

}  // namespace ab::net
