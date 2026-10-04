#include "resources/ani_file.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ab {

namespace {

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct Chunk {
    char tag[5] = {0, 0, 0, 0, 0};
    const std::uint8_t* body = nullptr;
    std::size_t size = 0;
    bool is(const char* t) const { return std::memcmp(tag, t, 4) == 0; }
};

// Splits [p, end) into chunks: tag[4], u32 length, u16 id, body.
std::vector<Chunk> chunks(const std::uint8_t* p, const std::uint8_t* end) {
    std::vector<Chunk> out;
    while (p + 10 <= end) {
        Chunk c;
        std::memcpy(c.tag, p, 4);
        const std::size_t len = u32(p + 4);
        if (len > static_cast<std::size_t>(end - (p + 10))) break;
        c.body = p + 10;
        c.size = len;
        out.push_back(c);
        p += 10 + len;
    }
    return out;
}

// Encoding 0x11: byte-controlled run-length coding of 16-bit units.
void unpackRle16(const std::uint8_t* src, std::size_t srcLen, std::vector<std::uint16_t>& out, std::size_t count) {
    std::size_t i = 0;
    while (i < srcLen && out.size() < count) {
        const std::uint8_t c = src[i];
        if (c == 0xFF) break;
        if ((c & 0x80) != 0) {
            if (i + 3 > srcLen) break;
            const std::uint16_t v = u16(src + i + 1);
            for (int n = (c & 0x7F) + 1; n > 0 && out.size() < count; --n) out.push_back(v);
            i += 3;
        } else {
            const std::size_t n = static_cast<std::size_t>(c) + 1;
            if (i + 1 + n * 2 > srcLen) break;
            for (std::size_t k = 0; k < n && out.size() < count; ++k) out.push_back(u16(src + i + 1 + k * 2));
            i += 1 + n * 2;
        }
    }
    out.resize(count, 0);
}

bool decodeCimg(const Chunk& c, AniFrame& f) {
    if (c.size < 24) return false;
    const std::uint8_t* b = c.body;
    const int kind = u16(b);
    const int flags = u16(b + 2);
    const std::size_t dataOff = u32(b + 4);
    f.width = u16(b + 12);
    f.height = u16(b + 14);
    f.hotX = u16(b + 16);
    f.hotY = u16(b + 18);
    f.key = static_cast<std::uint16_t>(u32(b + 20));
    f.hasKey = (flags & 4) != 0;
    if ((kind & 7) != 4 || dataOff + 12 > c.size) return false;  // only 16-bit images, as in the original
    const std::uint8_t enc = b[dataOff];
    const std::size_t hdr = u16(b + dataOff + 2);
    // The stored size counts the sub-header as well as the pixel data.
    const std::size_t stored = u32(b + dataOff + 4);
    if (stored < hdr || dataOff + stored > c.size) return false;
    const std::size_t csize = stored - hdr;
    const std::uint8_t* payload = b + dataOff + hdr;
    const std::size_t count = static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height);
    f.pixels.clear();
    f.pixels.reserve(count);
    if (enc == 0x11) {
        unpackRle16(payload, csize, f.pixels, count);
    } else if (enc == 0x00) {
        for (std::size_t k = 0; k < count && k * 2 + 1 < csize; ++k) f.pixels.push_back(u16(payload + k * 2));
        f.pixels.resize(count, 0);
    } else {
        return false;
    }
    return true;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<AniFile> parseAni(const std::vector<std::uint8_t>& data) {
    if (data.size() < 16 || std::memcmp(data.data(), "CHFILEANI ", 10) != 0) return std::nullopt;
    AniFile out;
    for (const Chunk& c : chunks(data.data() + 16, data.data() + data.size())) {
        if (c.is("FRAM")) {
            AniFrame f;
            for (const Chunk& s : chunks(c.body, c.body + c.size))
                if (s.is("CIMG")) decodeCimg(s, f);  // an undecodable frame stays empty but keeps its index
            out.frames.push_back(std::move(f));
        } else if (c.is("SEQ ")) {
            AniSequence seq;
            for (const Chunk& s : chunks(c.body, c.body + c.size)) {
                if (s.is("HEAD")) {
                    const char* n = reinterpret_cast<const char*>(s.body);
                    seq.name.assign(n, strnlen(n, s.size < 32 ? s.size : 32));
                } else if (s.is("STAT")) {
                    for (const Chunk& r : chunks(s.body, s.body + s.size))
                        if (r.is("FRAM") && r.size >= 8)
                            seq.steps.push_back({u16(r.body + 2), static_cast<std::int16_t>(u16(r.body + 4)),
                                                 static_cast<std::int16_t>(u16(r.body + 6))});
                }
            }
            out.sequences.push_back(std::move(seq));
        }
    }
    return out;
}

std::optional<AniFile> loadAniFile(const std::string& path) {
    const auto bytes = readFileBytes(path);
    if (!bytes) return std::nullopt;
    return parseAni(*bytes);
}

std::optional<PcxImage> loadPcxFile(const std::string& path) {
    const auto bytes = readFileBytes(path);
    if (!bytes || bytes->size() < 128 + 769) return std::nullopt;
    const std::uint8_t* d = bytes->data();
    if (d[0] != 0x0A || d[3] != 8 || d[65] != 1) return std::nullopt;  // 8 bits, one plane
    PcxImage img;
    img.width = u16(d + 8) - u16(d + 4) + 1;
    img.height = u16(d + 10) - u16(d + 6) + 1;
    const int stride = u16(d + 66);
    if (img.width <= 0 || img.height <= 0 || stride < img.width) return std::nullopt;
    img.indices.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), 0);
    std::size_t p = 128;
    const std::size_t end = bytes->size() - 769;
    for (int y = 0; y < img.height; ++y) {
        int x = 0;
        while (x < stride && p < end) {
            std::uint8_t v = d[p++];
            int run = 1;
            if ((v & 0xC0) == 0xC0) {
                run = v & 0x3F;
                if (p >= end) break;
                v = d[p++];
            }
            for (; run > 0 && x < stride; --run, ++x)
                if (x < img.width) img.indices[static_cast<std::size_t>(y * img.width + x)] = v;
        }
    }
    if (d[bytes->size() - 769] != 0x0C) return std::nullopt;
    img.palette.assign(d + bytes->size() - 768, d + bytes->size());
    return img;
}

std::optional<FontFile> parseFont(const std::vector<std::uint8_t>& d) {
    if (d.size() < 20) return std::nullopt;
    const auto count = static_cast<std::size_t>(u32(d.data()));
    FontFile f;
    f.height = static_cast<int>(u32(d.data() + 4));
    f.spacing = static_cast<int>(u32(d.data() + 8));
    if (count == 0 || count > 256 || f.height <= 0 || f.height > 64) return std::nullopt;
    const std::size_t table = 20;
    const std::size_t bits = table + count * 8;
    if (bits > d.size()) return std::nullopt;
    f.glyphs.resize(count);
    for (std::size_t g = 0; g < count; ++g) {
        const int w = static_cast<int>(u32(d.data() + table + g * 8));
        const std::size_t off = u32(d.data() + table + g * 8 + 4);
        if (w < 0 || w > 64) return std::nullopt;
        const std::size_t rowBytes = static_cast<std::size_t>((w + 7) / 8);
        if (bits + off + rowBytes * static_cast<std::size_t>(f.height) > d.size()) return std::nullopt;
        FontGlyph& glyph = f.glyphs[g];
        glyph.width = w;
        glyph.alpha.assign(static_cast<std::size_t>(w * f.height), 0);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < w; ++x)
                if ((d[bits + off + static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x / 8)] & (0x80 >> (x % 8))) != 0)
                    glyph.alpha[static_cast<std::size_t>(y * w + x)] = 255;
    }
    return f;
}

std::optional<FontFile> loadFontFile(const std::string& path) {
    const auto bytes = readFileBytes(path);
    if (!bytes) return std::nullopt;
    return parseFont(*bytes);
}

std::optional<GamePalette> loadPaletteFile(const std::string& path) {
    const auto bytes = readFileBytes(path);
    if (!bytes || bytes->size() < 768 + 32768) return std::nullopt;
    GamePalette p;
    p.rgb6.assign(bytes->begin(), bytes->begin() + 768);
    p.lookup.assign(bytes->begin() + 768, bytes->begin() + 768 + 32768);
    return p;
}

}  // namespace ab
