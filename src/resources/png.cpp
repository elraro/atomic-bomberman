#include "resources/png.hpp"

#include <cmath>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace ab {

namespace {

// ---- DEFLATE decoder: canonical Huffman codes decoded a bit at a time (after zlib's "puff") ----

struct Huffman {
    std::array<short, 16> count{};   // codes of each length
    std::array<short, 288> symbol{};  // symbols in order of code
};

// Returns 0 for a complete code, > 0 for an incomplete one, < 0 for an over-subscribed one.
int construct(Huffman& h, const short* length, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s) ++h.count[static_cast<std::size_t>(length[s])];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[static_cast<std::size_t>(len)];
        if (left < 0) return left;
    }
    std::array<short, 16> offs{};
    for (int len = 1; len < 15; ++len)
        offs[static_cast<std::size_t>(len + 1)] = static_cast<short>(offs[static_cast<std::size_t>(len)] + h.count[static_cast<std::size_t>(len)]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0) h.symbol[static_cast<std::size_t>(offs[static_cast<std::size_t>(length[s])]++)] = static_cast<short>(s);
    return left;
}

class Inflater {
public:
    Inflater(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out) : in_(in), size_(size), out_(out) {}

    bool run() {
        for (bool last = false; !last;) {
            std::uint32_t b = 0;
            if (!bits(1, b)) return false;
            last = b != 0;
            std::uint32_t type = 0;
            if (!bits(2, type)) return false;
            bool ok = false;
            if (type == 0) ok = stored();
            else if (type == 1) ok = fixedBlock();
            else if (type == 2) ok = dynamicBlock();
            if (!ok) return false;
        }
        return true;
    }

private:
    bool bits(int need, std::uint32_t& value) {
        while (count_ < need) {
            if (pos_ >= size_) return false;
            buffer_ |= static_cast<std::uint32_t>(in_[pos_++]) << count_;
            count_ += 8;
        }
        value = buffer_ & ((1u << need) - 1u);
        buffer_ >>= need;
        count_ -= need;
        return true;
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            std::uint32_t b = 0;
            if (!bits(1, b)) return -1;
            code |= static_cast<int>(b);
            const int count = h.count[static_cast<std::size_t>(len)];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool stored() {
        buffer_ = 0;
        count_ = 0;
        if (pos_ + 4 > size_) return false;
        const std::size_t len = in_[pos_] | (static_cast<std::size_t>(in_[pos_ + 1]) << 8);
        const std::size_t nlen = in_[pos_ + 2] | (static_cast<std::size_t>(in_[pos_ + 3]) << 8);
        pos_ += 4;
        if (len != (~nlen & 0xFFFF) || pos_ + len > size_) return false;
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;
        return true;
    }

    bool codes(const Huffman& lit, const Huffman& dist) {
        static const short kLen[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short kDist[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lit);
            if (sym < 0) return false;
            if (sym < 256) {
                out_.push_back(static_cast<std::uint8_t>(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                std::uint32_t extra = 0;
                if (!bits(kLenExtra[sym], extra)) return false;
                const std::size_t len = static_cast<std::size_t>(kLen[sym]) + extra;
                const int ds = decode(dist);
                if (ds < 0 || ds >= 30) return false;
                if (!bits(kDistExtra[ds], extra)) return false;
                const std::size_t d = static_cast<std::size_t>(kDist[ds]) + extra;
                if (d > out_.size()) return false;
                std::size_t from = out_.size() - d;
                for (std::size_t i = 0; i < len; ++i) out_.push_back(out_[from + i]);
            }
        }
    }

    bool fixedBlock() {
        static Huffman lit, dist;
        static bool built = false;
        if (!built) {
            short lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            construct(lit, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            construct(dist, lengths, 30);
            built = true;
        }
        return codes(lit, dist);
    }

    bool dynamicBlock() {
        static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        std::uint32_t nlen = 0, ndist = 0, ncode = 0;
        if (!bits(5, nlen) || !bits(5, ndist) || !bits(4, ncode)) return false;
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30) return false;
        short lengths[320] = {};
        for (std::uint32_t i = 0; i < ncode; ++i) {
            std::uint32_t v = 0;
            if (!bits(3, v)) return false;
            lengths[kOrder[i]] = static_cast<short>(v);
        }
        Huffman lencode;
        if (construct(lencode, lengths, 19) != 0) return false;
        std::uint32_t index = 0;
        std::fill(std::begin(lengths), std::end(lengths), static_cast<short>(0));
        while (index < nlen + ndist) {
            const int sym = decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[index++] = static_cast<short>(sym);
                continue;
            }
            short previous = 0;
            std::uint32_t repeat = 0;
            if (sym == 16) {
                if (index == 0) return false;
                previous = lengths[index - 1];
                if (!bits(2, repeat)) return false;
                repeat += 3;
            } else if (sym == 17) {
                if (!bits(3, repeat)) return false;
                repeat += 3;
            } else {
                if (!bits(7, repeat)) return false;
                repeat += 11;
            }
            if (index + repeat > nlen + ndist) return false;
            while (repeat-- > 0) lengths[index++] = previous;
        }
        if (lengths[256] == 0) return false;
        Huffman lit, dist;
        int err = construct(lit, lengths, static_cast<int>(nlen));
        if (err != 0 && (err < 0 || static_cast<int>(nlen) != lit.count[0] + lit.count[1])) return false;
        err = construct(dist, lengths + nlen, static_cast<int>(ndist));
        if (err != 0 && (err < 0 || static_cast<int>(ndist) != dist.count[0] + dist.count[1])) return false;
        return codes(lit, dist);
    }

    const std::uint8_t* in_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint32_t buffer_ = 0;
    int count_ = 0;
    std::vector<std::uint8_t>& out_;
};

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
    const auto& t = crcTable();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void putChunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& body) {
    put32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> inflateRaw(const std::uint8_t* in, std::size_t size, std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    Inflater inflater(in, size, out);
    if (!inflater.run()) return std::nullopt;
    return out;
}

std::optional<PngImage> decodePng(const std::vector<std::uint8_t>& data) {
    static const std::uint8_t kMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 8 + 25 || !std::equal(kMagic, kMagic + 8, data.begin())) return std::nullopt;
    int width = 0, height = 0, depth = 0, colour = -1, interlace = 0;
    std::vector<std::uint8_t> idat, palette;
    std::size_t pos = 8;
    while (pos + 12 <= data.size()) {
        const std::uint32_t len = be32(&data[pos]);
        if (len > data.size() || pos + 12 + len > data.size()) return std::nullopt;
        const std::string tag(reinterpret_cast<const char*>(&data[pos + 4]), 4);
        const std::uint8_t* body = &data[pos + 8];
        if (tag == "IHDR" && len >= 13) {
            width = static_cast<int>(be32(body));
            height = static_cast<int>(be32(body + 4));
            depth = body[8];
            colour = body[9];
            interlace = body[12];
        } else if (tag == "PLTE") {
            palette.assign(body, body + len);
        } else if (tag == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        } else if (tag == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || depth != 8 || interlace != 0 || idat.size() < 6) return std::nullopt;
    int channels = 0;
    switch (colour) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return std::nullopt;
    }
    if (colour == 3 && palette.size() < 3) return std::nullopt;
    if ((idat[0] & 0x0F) != 8) return std::nullopt;  // zlib header: deflate
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    const std::size_t expected = (stride + 1) * static_cast<std::size_t>(height);
    auto raw = inflateRaw(idat.data() + 2, idat.size() - 2, expected);
    if (!raw || raw->size() < expected) return std::nullopt;

    // Undo the line filters in place.
    const std::size_t bpp = static_cast<std::size_t>(channels);
    std::vector<std::uint8_t>& r = *raw;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* line = &r[static_cast<std::size_t>(y) * (stride + 1)];
        const int filter = line[0];
        std::uint8_t* cur = line + 1;
        const std::uint8_t* prev = y > 0 ? line - stride : nullptr;  // previous line's data (after its filter byte)
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0;
            const int b = prev != nullptr ? prev[i] : 0;
            const int c = (prev != nullptr && i >= bpp) ? prev[i - bpp] : 0;
            int add = 0;
            switch (filter) {
                case 0: break;
                case 1: add = a; break;
                case 2: add = b; break;
                case 3: add = (a + b) / 2; break;
                case 4: add = paeth(a, b, c); break;
                default: return std::nullopt;
            }
            cur[i] = static_cast<std::uint8_t>(cur[i] + add);
        }
    }

    PngImage img;
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = &r[static_cast<std::size_t>(y) * (stride + 1) + 1];
        std::uint8_t* dst = &img.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4];
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* s = src + static_cast<std::size_t>(x) * bpp;
            std::uint8_t* d = dst + static_cast<std::size_t>(x) * 4;
            switch (colour) {
                case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
                case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
                case 3: {
                    const std::size_t k = static_cast<std::size_t>(s[0]) * 3;
                    if (k + 2 < palette.size()) {
                        d[0] = palette[k];
                        d[1] = palette[k + 1];
                        d[2] = palette[k + 2];
                    } else {
                        d[0] = d[1] = d[2] = 0;
                    }
                    d[3] = 255;
                    break;
                }
                case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                default: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
            }
        }
    }
    return img;
}

std::optional<PngImage> loadPngFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodePng(bytes);
}

std::vector<std::uint8_t> encodePng(int width, int height, const std::uint8_t* rgba, bool withAlpha) {
    const std::size_t channels = withAlpha ? 4 : 3;
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        const std::uint8_t* line = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            raw.insert(raw.end(), line + static_cast<std::size_t>(x) * 4, line + static_cast<std::size_t>(x) * 4 + channels);
        }
    }
    // zlib stream of stored blocks.
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    std::size_t off = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
    } while (off < raw.size());
    put32(z, (b << 16) | a);

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(width));
    put32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, static_cast<std::uint8_t>(withAlpha ? 6 : 2), 0, 0, 0});
    putChunk(out, "IHDR", ihdr);
    putChunk(out, "IDAT", z);
    putChunk(out, "IEND", {});
    return out;
}

bool writePngFile(const std::string& path, int width, int height, const std::uint8_t* rgba, bool withAlpha) {
    const std::vector<std::uint8_t> bytes = encodePng(width, height, rgba, withAlpha);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace ab
