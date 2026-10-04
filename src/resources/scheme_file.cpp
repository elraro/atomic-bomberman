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

std::optional<SchemeFile> loadSchemeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseSchemeText(ss.str());
}

}  // namespace ab
