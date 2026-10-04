#include "resources/scheme_file.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace ab {

namespace {

std::vector<std::string> splitCommas(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : line) {
        if (ch == ',') {
            parts.push_back(cur);
            cur.clear();
        } else if (ch != '\r') {
            cur += ch;
        }
    }
    parts.push_back(cur);
    return parts;
}

}  // namespace

std::optional<SchemeFile> parseSchemeText(const std::string& text) {
    SchemeFile out;
    out.scheme = Scheme::pillars();
    int rows = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] != '-') continue;
        const std::vector<std::string> f = splitCommas(line);
        const char kind = line[1];
        if (kind == 'N' && f.size() >= 2) {
            out.name = f[1];
        } else if (kind == 'B' && f.size() >= 2) {
            out.scheme.brickDensity = std::atoi(f[1].c_str());
        } else if (kind == 'R' && f.size() >= 3) {
            const int y = std::atoi(f[1].c_str());
            if (y < 0 || y >= kGridH || f[2].size() < static_cast<std::size_t>(kGridW)) return std::nullopt;
            for (int x = 0; x < kGridW; ++x) {
                const char ch = f[2][static_cast<std::size_t>(x)];
                out.scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                    ch == '#' ? Tile::Solid : ch == ':' ? Tile::Brick : Tile::Blank;
            }
            ++rows;
        } else if (kind == 'S' && f.size() >= 4) {
            const int p = std::atoi(f[1].c_str());
            if (p < 0 || p >= kMaxPlayers) continue;
            out.scheme.start[static_cast<std::size_t>(p)] = {std::atoi(f[2].c_str()), std::atoi(f[3].c_str())};
            if (f.size() >= 5) out.team[static_cast<std::size_t>(p)] = std::atoi(f[4].c_str());
        }
    }
    if (rows != kGridH) return std::nullopt;
    return out;
}

namespace {

int wrapCoord(const std::string& text, int size) {
    int v = std::atoi(text.c_str());
    while (v < 0) v += size;
    return v >= size ? size - 1 : v;
}

Dir dirFromLetter(const std::string& s) {
    for (char ch : s) {
        switch (ch) {
            case 'N': case 'n': return 0;
            case 'E': case 'e': return 1;
            case 'S': case 's': return 2;
            case 'W': case 'w': return 3;
            default: break;
        }
    }
    return kNoDir;
}

bool isHidden(const std::string& s) { return s.find('H') != std::string::npos || s.find('h') != std::string::npos; }

}  // namespace

std::vector<Extra> parseExtrasText(const std::string& text) {
    std::vector<Extra> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] != '-') continue;
        const std::vector<std::string> f = splitCommas(line);
        Extra e;
        const char kind = line[1];
        if ((kind == 'A' || kind == 'C') && f.size() >= 4) {
            e.type = kind == 'A' ? ExtraType::Arrow : ExtraType::Conveyor;
            e.dir = dirFromLetter(f[1]);
            if (e.dir == kNoDir) continue;
            e.cell = {wrapCoord(f[2], kGridW), wrapCoord(f[3], kGridH)};
        } else if (kind == 'T' && f.size() >= 3) {
            e.type = ExtraType::Trampoline;
            e.cell = isHidden(f[1]) ? Cell{-1, -1} : Cell{wrapCoord(f[1], kGridW), wrapCoord(f[2], kGridH)};
        } else if (kind == 'W' && f.size() >= 6) {
            e.type = ExtraType::Warp;
            e.id = std::atoi(f[2].c_str());
            e.cell = {wrapCoord(f[3], kGridW), wrapCoord(f[4], kGridH)};
            e.linkTo = std::atoi(f[5].c_str());
        } else {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

std::vector<Extra> loadExtrasFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseExtrasText(ss.str());
}

std::optional<SchemeFile> loadSchemeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseSchemeText(ss.str());
}

}  // namespace ab
