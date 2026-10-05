// Text in the program is UTF-8. The game's font (the original's font1.fon, and
// the free set's) has 256 character slots laid out as the DOS code page 437:
// ASCII, then a number of accented letters. These helpers turn text into font
// slots and take typed text only as far as the font can show it.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ab {

// The font slot of a character, or -1 if the font has none. Accented capitals the
// font lacks fall back to the plain letter.
inline int glyphFor(char32_t c) {
    if (c < 128) return static_cast<int>(c);
    struct Pair {
        char32_t code;
        int slot;
    };
    static constexpr Pair kTable[] = {
        {0xC7, 128}, {0xFC, 129}, {0xE9, 130}, {0xE2, 131}, {0xE4, 132}, {0xE0, 133}, {0xE7, 135}, {0xEA, 136}, {0xE8, 138},
        {0xEE, 140}, {0xC4, 142}, {0xF4, 147}, {0xF6, 148}, {0xFB, 150}, {0xF9, 151}, {0xFF, 152}, {0xD6, 153}, {0xDC, 154},
        {0xE1, 160}, {0xED, 161}, {0xF3, 162}, {0xFA, 163}, {0xF1, 164}, {0xD1, 165}, {0xBF, 168}, {0xA1, 173},
        // No slot of their own: shown without the accent.
        {0xC1, 'A'}, {0xC0, 'A'}, {0xC2, 'A'}, {0xC9, 'E'}, {0xC8, 'E'}, {0xCA, 'E'}, {0xCD, 'I'}, {0xCC, 'I'}, {0xCE, 'I'},
        {0xD3, 'O'}, {0xD2, 'O'}, {0xD4, 'O'}, {0xDA, 'U'}, {0xD9, 'U'}, {0xDB, 'U'}, {0xEC, 'i'}, {0xF2, 'o'}, {0xEB, 'e'},
        {0xEF, 'i'}, {0xE3, 'a'}, {0xF5, 'o'}, {0xAA, 'a'}, {0xBA, 'o'}};
    for (const Pair& p : kTable)
        if (p.code == c) return p.slot;
    return -1;
}

// Reads one character of UTF-8 at s[i] and moves i past it. Bytes that are not UTF-8
// are taken as they are (the original's own text files use the font's slots directly).
inline char32_t nextChar(const std::string& s, std::size_t& i, bool* raw = nullptr) {
    const auto b = static_cast<unsigned char>(s[i]);
    auto continuation = [&](std::size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
    if (raw != nullptr) *raw = false;
    if (b < 0x80) return ++i, b;
    if ((b & 0xE0) == 0xC0 && continuation(1)) {
        const char32_t c = (static_cast<char32_t>(b & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        return i += 2, c;
    }
    if ((b & 0xF0) == 0xE0 && continuation(1) && continuation(2)) {
        const char32_t c = (static_cast<char32_t>(b & 0x0F) << 12) | ((static_cast<char32_t>(static_cast<unsigned char>(s[i + 1])) & 0x3F) << 6) |
                           (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        return i += 3, c;
    }
    if ((b & 0xF8) == 0xF0 && continuation(1) && continuation(2) && continuation(3)) return i += 4, 0xFFFD;
    if (raw != nullptr) *raw = true;
    return ++i, b;
}

// The font slots to draw for a text; what the font cannot show becomes '?'.
inline std::vector<int> glyphsOf(const std::string& text) {
    std::vector<int> out;
    for (std::size_t i = 0; i < text.size();) {
        bool raw = false;
        const char32_t c = nextChar(text, i, &raw);
        const int slot = raw ? static_cast<int>(c) : glyphFor(c);
        out.push_back(slot >= 0 ? slot : '?');
    }
    return out;
}

// Adds typed text to `target`: only characters the font can show, never beyond maxBytes.
inline void appendTyped(std::string& target, const char* utf8, std::size_t maxBytes, bool digitsOnly = false) {
    const std::string typed = utf8 != nullptr ? utf8 : "";
    for (std::size_t i = 0; i < typed.size();) {
        const std::size_t start = i;
        bool raw = false;
        const char32_t c = nextChar(typed, i, &raw);
        if (raw || c < 32 || c == 127 || glyphFor(c) < 0) continue;
        if (digitsOnly && (c < '0' || c > '9')) continue;
        if (target.size() + (i - start) > maxBytes) break;
        target.append(typed, start, i - start);
    }
}

// Backspace: removes the last character, however many bytes it has.
inline void removeLastChar(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // the tail of a multi-byte character
    if (!s.empty()) s.pop_back();
}

}  // namespace ab
