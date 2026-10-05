#include "resources/mve_file.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ab {

namespace {

constexpr char kSignature[] = "Interplay MVE File\x1a";  // followed by a zero byte and three 16-bit words

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) { return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32); }

// Steps of the Interplay DPCM sound coding: each byte adds table[byte] to the
// running sample of its channel.
const std::int16_t* deltaTable() {
    static std::int16_t table[256];
    static bool built = false;
    if (!built) {
        static const int grow[] = {47,    51,    56,    61,    66,    72,    79,    86,    94,    102,   112,   122,   133,
                                   145,   158,   173,   189,   206,   225,   245,   267,   292,   318,   348,   379,   414,
                                   452,   493,   538,   587,   640,   699,   763,   832,   908,   991,   1081,  1180,  1288,
                                   1405,  1534,  1673,  1826,  1993,  2175,  2373,  2590,  2826,  3084,  3365,  3672,  4008,
                                   4373,  4772,  5208,  5683,  6202,  6767,  7385,  8059,  8794,  9597,  10472, 11428, 12471,
                                   13609, 14851, 16206, 17685, 19298, 21060, 22981, 25078, 27367, 29864, 32589};
        static const int wrap[] = {-29973, -26728, -23186, -19322, -15105, -10503, -5481, -1,
                                   1,      1,      5481,   10503,  15105,  19322,  23186, 26728, 29973};
        int n = 0;
        for (int i = 0; i <= 43; ++i) table[n++] = static_cast<std::int16_t>(i);
        for (int g : grow) table[n++] = static_cast<std::int16_t>(g);
        for (int w : wrap) table[n++] = static_cast<std::int16_t>(w);
        for (int i = static_cast<int>(std::size(grow)) - 1; i >= 0; --i) table[n++] = static_cast<std::int16_t>(-grow[i]);
        for (int i = 43; i >= 1; --i) table[n++] = static_cast<std::int16_t>(-i);
        while (n < 256) table[n++] = 0;
        built = true;
    }
    return table;
}

}  // namespace

bool MveDecoder::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return openBytes(std::move(bytes));
}

bool MveDecoder::openBytes(std::vector<std::uint8_t> bytes) {
    data_ = std::move(bytes);
    const std::size_t sigLen = sizeof kSignature;  // includes the zero byte
    // The last occurrence followed by the header words 0x001A 0x0100 0x1133 is the movie.
    bool found = false;
    for (std::size_t i = 0; i + sigLen + 6 <= data_.size(); ++i) {
        if (data_[i] != 'I' || std::memcmp(&data_[i], kSignature, sigLen) != 0) continue;
        const std::uint8_t* h = &data_[i + sigLen];
        if (le16(h) == 0x001a && le16(h + 2) == 0x0100 && le16(h + 4) == 0x1133) {
            start_ = i;
            found = true;
            break;
        }
    }
    if (!found) return false;
    pos_ = start_ + sigLen + 6;
    ended_ = false;
    frames_ = 0;
    audio_.clear();
    return true;
}

std::vector<std::int16_t> MveDecoder::takeAudio() {
    std::vector<std::int16_t> out;
    out.swap(audio_);
    return out;
}

void MveDecoder::decodeAudio(const std::uint8_t* p, std::size_t size) {
    // Header: sequence, stream mask, decoded length in bytes. Only the first stream is used.
    if (size < 6 || (le16(p + 2) & 1) == 0) return;
    const std::uint8_t* d = p + 6;
    std::size_t n = size - 6;
    if (!compressed_) {
        if (sixteenBit_)
            for (std::size_t i = 0; i + 1 < n; i += 2) audio_.push_back(static_cast<std::int16_t>(le16(d + i)));
        else
            for (std::size_t i = 0; i < n; ++i) audio_.push_back(static_cast<std::int16_t>((d[i] - 128) << 8));
        return;
    }
    const std::int16_t* table = deltaTable();
    int predictor[2] = {0, 0};
    for (int ch = 0; ch < channels_ && n >= 2; ++ch) {
        predictor[ch] = static_cast<std::int16_t>(le16(d));
        audio_.push_back(static_cast<std::int16_t>(predictor[ch]));
        d += 2;
        n -= 2;
    }
    int ch = 0;
    for (std::size_t i = 0; i < n; ++i) {
        predictor[ch] = std::clamp(predictor[ch] + table[d[i]], -32768, 32767);
        audio_.push_back(static_cast<std::int16_t>(predictor[ch]));
        if (channels_ == 2) ch ^= 1;
    }
}

bool MveDecoder::decodeVideo(const std::uint8_t* p, std::size_t size) {
    if (size < 14 || width_ <= 0 || map_.empty()) return false;
    const std::uint8_t* s = p + 14;  // after frame numbers, offsets, sizes and flags
    const std::uint8_t* end = p + size;
    const int w = width_, h = height_;
    auto need = [&](std::ptrdiff_t n) { return end - s >= n; };
    // Copies an 8x8 block from another (or the same) picture, displaced by (dx, dy).
    auto copyFrom = [&](const std::vector<std::uint8_t>& src, int bx, int by, int dx, int dy) {
        const int sx = bx + dx, sy = by + dy;
        if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h || src.empty()) return;
        for (int y = 0; y < 8; ++y)
            std::memmove(&current_[static_cast<std::size_t>((by + y) * w + bx)], &src[static_cast<std::size_t>((sy + y) * w + sx)], 8);
    };
    int block = 0;
    for (int by = 0; by < h; by += 8) {
        for (int bx = 0; bx < w; bx += 8, ++block) {
            const std::size_t mi = static_cast<std::size_t>(block / 2);
            if (mi >= map_.size()) return false;
            const int op = (block & 1) != 0 ? map_[mi] >> 4 : map_[mi] & 15;
            std::uint8_t* px = &current_[static_cast<std::size_t>(by * w + bx)];
            auto put = [&](int x, int y, std::uint8_t v) { px[y * w + x] = v; };
            std::uint8_t P[8];
            switch (op) {
                case 0x0: copyFrom(last_, bx, by, 0, 0); break;
                case 0x1: copyFrom(secondLast_, bx, by, 0, 0); break;
                case 0x2:
                case 0x3: {
                    if (!need(1)) return false;
                    const int b = *s++;
                    int x, y;
                    if (b < 56) {
                        x = 8 + (b % 7);
                        y = b / 7;
                    } else {
                        x = -14 + ((b - 56) % 29);
                        y = 8 + ((b - 56) / 29);
                    }
                    if (op == 0x2) copyFrom(secondLast_, bx, by, x, y);
                    else copyFrom(current_, bx, by, -x, -y);
                    break;
                }
                case 0x4: {
                    if (!need(1)) return false;
                    const int b = *s++;
                    copyFrom(last_, bx, by, -8 + (b & 15), -8 + (b >> 4));
                    break;
                }
                case 0x5: {
                    if (!need(2)) return false;
                    const int x = static_cast<std::int8_t>(s[0]), y = static_cast<std::int8_t>(s[1]);
                    s += 2;
                    copyFrom(last_, bx, by, x, y);
                    break;
                }
                case 0x6: break;  // not used by the coder
                case 0x7: {
                    if (!need(2)) return false;
                    P[0] = *s++;
                    P[1] = *s++;
                    if (P[0] <= P[1]) {
                        if (!need(8)) return false;
                        for (int y = 0; y < 8; ++y) {
                            const unsigned flags = *s++;
                            for (int x = 0; x < 8; ++x) put(x, y, P[(flags >> x) & 1]);
                        }
                    } else {
                        if (!need(2)) return false;
                        unsigned flags = le16(s);
                        s += 2;
                        for (int y = 0; y < 8; y += 2)
                            for (int x = 0; x < 8; x += 2, flags >>= 1) {
                                const std::uint8_t v = P[flags & 1];
                                put(x, y, v), put(x + 1, y, v), put(x, y + 1, v), put(x + 1, y + 1, v);
                            }
                    }
                    break;
                }
                case 0x8: {
                    if (!need(2)) return false;
                    P[0] = *s++;
                    P[1] = *s++;
                    if (P[0] <= P[1]) {
                        // Two colours for each quadrant: top left, bottom left, top right, bottom right.
                        unsigned flags = 0;
                        for (int q = 0; q < 16; ++q) {
                            if ((q & 3) == 0) {
                                if (q != 0) {
                                    if (!need(2)) return false;
                                    P[0] = *s++;
                                    P[1] = *s++;
                                }
                                if (!need(2)) return false;
                                flags = le16(s);
                                s += 2;
                            }
                            const int x0 = q < 8 ? 0 : 4, y = q & 7;
                            for (int x = 0; x < 4; ++x, flags >>= 1) put(x0 + x, y, P[flags & 1]);
                        }
                    } else {
                        if (!need(6)) return false;
                        std::uint32_t flags = le32(s);
                        s += 4;
                        P[2] = *s++;
                        P[3] = *s++;
                        if (P[2] <= P[3]) {
                            // Left and right halves.
                            for (int q = 0; q < 16; ++q) {
                                const int x0 = q < 8 ? 0 : 4, y = q & 7;
                                for (int x = 0; x < 4; ++x, flags >>= 1) put(x0 + x, y, P[flags & 1]);
                                if (q == 7) {
                                    if (!need(4)) return false;
                                    P[0] = P[2];
                                    P[1] = P[3];
                                    flags = le32(s);
                                    s += 4;
                                }
                            }
                        } else {
                            // Top and bottom halves.
                            for (int y = 0; y < 8; ++y) {
                                if (y == 4) {
                                    if (!need(4)) return false;
                                    P[0] = P[2];
                                    P[1] = P[3];
                                    flags = le32(s);
                                    s += 4;
                                }
                                for (int x = 0; x < 8; ++x, flags >>= 1) put(x, y, P[flags & 1]);
                            }
                        }
                    }
                    break;
                }
                case 0x9: {
                    if (!need(4)) return false;
                    std::memcpy(P, s, 4);
                    s += 4;
                    if (P[0] <= P[1]) {
                        if (P[2] <= P[3]) {
                            if (!need(16)) return false;
                            for (int y = 0; y < 8; ++y) {
                                unsigned flags = le16(s);
                                s += 2;
                                for (int x = 0; x < 8; ++x, flags >>= 2) put(x, y, P[flags & 3]);
                            }
                        } else {
                            if (!need(4)) return false;
                            std::uint32_t flags = le32(s);
                            s += 4;
                            for (int y = 0; y < 8; y += 2)
                                for (int x = 0; x < 8; x += 2, flags >>= 2) {
                                    const std::uint8_t v = P[flags & 3];
                                    put(x, y, v), put(x + 1, y, v), put(x, y + 1, v), put(x + 1, y + 1, v);
                                }
                        }
                    } else {
                        if (!need(8)) return false;
                        std::uint64_t flags = le64(s);
                        s += 8;
                        if (P[2] <= P[3]) {
                            for (int y = 0; y < 8; ++y)
                                for (int x = 0; x < 8; x += 2, flags >>= 2) put(x, y, P[flags & 3]), put(x + 1, y, P[flags & 3]);
                        } else {
                            for (int y = 0; y < 8; y += 2)
                                for (int x = 0; x < 8; ++x, flags >>= 2) put(x, y, P[flags & 3]), put(x, y + 1, P[flags & 3]);
                        }
                    }
                    break;
                }
                case 0xA: {
                    if (!need(4)) return false;
                    std::memcpy(P, s, 4);
                    s += 4;
                    if (P[0] <= P[1]) {
                        // Four colours for each quadrant.
                        std::uint32_t flags = 0;
                        for (int q = 0; q < 16; ++q) {
                            if ((q & 3) == 0) {
                                if (q != 0) {
                                    if (!need(4)) return false;
                                    std::memcpy(P, s, 4);
                                    s += 4;
                                }
                                if (!need(4)) return false;
                                flags = le32(s);
                                s += 4;
                            }
                            const int x0 = q < 8 ? 0 : 4, y = q & 7;
                            for (int x = 0; x < 4; ++x, flags >>= 2) put(x0 + x, y, P[flags & 3]);
                        }
                    } else {
                        // Four colours for each half: left/right or top/bottom.
                        if (!need(12)) return false;
                        std::uint64_t flags = le64(s);
                        s += 8;
                        std::memcpy(P + 4, s, 4);
                        s += 4;
                        const bool vertical = P[4] <= P[5];
                        for (int q = 0; q < 16; ++q) {
                            const int x0 = vertical ? (q < 8 ? 0 : 4) : (q & 1) * 4;
                            const int y = vertical ? (q & 7) : q / 2;
                            for (int x = 0; x < 4; ++x, flags >>= 2) put(x0 + x, y, P[flags & 3]);
                            if (q == 7) {
                                if (!need(8)) return false;
                                std::memcpy(P, P + 4, 4);
                                flags = le64(s);
                                s += 8;
                            }
                        }
                    }
                    break;
                }
                case 0xB:
                    if (!need(64)) return false;
                    for (int y = 0; y < 8; ++y, s += 8) std::memcpy(px + y * w, s, 8);
                    break;
                case 0xC:
                    if (!need(16)) return false;
                    for (int y = 0; y < 8; y += 2)
                        for (int x = 0; x < 8; x += 2) {
                            const std::uint8_t v = *s++;
                            put(x, y, v), put(x + 1, y, v), put(x, y + 1, v), put(x + 1, y + 1, v);
                        }
                    break;
                case 0xD:
                    if (!need(4)) return false;
                    for (int y = 0; y < 8; ++y) {
                        if ((y & 3) == 0) {
                            P[0] = *s++;
                            P[1] = *s++;
                        }
                        std::memset(px + y * w, P[0], 4);
                        std::memset(px + y * w + 4, P[1], 4);
                    }
                    break;
                case 0xE:
                    if (!need(1)) return false;
                    for (int y = 0; y < 8; ++y) std::memset(px + y * w, *s, 8);
                    ++s;
                    break;
                default:  // 0xF: a checkerboard of two colours
                    if (!need(2)) return false;
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x) put(x, y, s[(x + y) & 1]);
                    s += 2;
                    break;
            }
        }
    }
    return true;
}

void MveDecoder::present() {
    rgba_.resize(current_.size() * 4);
    for (std::size_t i = 0; i < current_.size(); ++i) {
        const std::uint8_t* c = palette_[current_[i]];
        rgba_[i * 4] = c[0];
        rgba_[i * 4 + 1] = c[1];
        rgba_[i * 4 + 2] = c[2];
        rgba_[i * 4 + 3] = 255;
    }
    // The picture just shown becomes the previous one; its buffer is reused two pictures later.
    secondLast_.swap(last_);
    last_.swap(current_);
    if (current_.size() != last_.size()) current_.assign(last_.size(), 0);
    ++frames_;
}

bool MveDecoder::nextFrame() {
    if (ended_ || data_.empty()) return false;
    while (pos_ + 4 <= data_.size()) {
        const std::size_t chunkLen = le16(&data_[pos_]);
        const std::size_t chunkEnd = std::min(data_.size(), pos_ + 4 + chunkLen);
        std::size_t p = pos_ + 4;
        pos_ = chunkEnd;
        bool shown = false;
        while (p + 4 <= chunkEnd) {
            const std::size_t len = le16(&data_[p]);
            const int op = data_[p + 2];
            const int version = data_[p + 3];
            const std::uint8_t* d = data_.data() + p + 4;
            p += 4 + len;
            if (p > chunkEnd) break;
            switch (op) {
                case 0x00:  // end of the movie
                    ended_ = true;
                    return false;
                case 0x02:  // timer: microseconds per tick, ticks per picture
                    if (len >= 6) frameSeconds_ = static_cast<double>(le32(d)) * le16(d + 4) / 1.0e6;
                    break;
                case 0x03:  // sound format
                    if (len >= 6) {
                        const unsigned flags = le16(d + 2);
                        channels_ = (flags & 1) != 0 ? 2 : 1;
                        sixteenBit_ = (flags & 2) != 0;
                        compressed_ = version >= 1 && (flags & 4) != 0;
                        sampleRate_ = le16(d + 4);
                    }
                    break;
                case 0x05:  // picture size in 8x8 blocks
                    if (len >= 4) {
                        width_ = le16(d) * 8;
                        height_ = le16(d + 2) * 8;
                        const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
                        current_.assign(n, 0);
                        last_.assign(n, 0);
                        secondLast_.assign(n, 0);
                    }
                    break;
                case 0x07:  // show the picture
                    if (!current_.empty()) {
                        present();
                        shown = true;
                    }
                    break;
                case 0x08: decodeAudio(d, len); break;
                case 0x0C:  // palette: first index, count, then 6-bit red, green, blue
                    if (len >= 4) {
                        const unsigned first = le16(d), count = le16(d + 2);
                        for (unsigned i = 0; i < count && first + i < 256 && 4 + i * 3 + 2 < len; ++i)
                            for (int c = 0; c < 3; ++c) {
                                const unsigned v = d[4 + i * 3 + static_cast<unsigned>(c)] & 63u;
                                palette_[first + i][c] = static_cast<std::uint8_t>((v << 2) | (v >> 4));
                            }
                    }
                    break;
                case 0x0F: map_.assign(d, d + len); break;  // block codes of the next picture
                case 0x11: decodeVideo(d, len); break;
                default: break;  // sound start, silence, video mode and others: nothing to do
            }
        }
        if (shown) return true;
    }
    ended_ = true;
    return false;
}

}  // namespace ab
