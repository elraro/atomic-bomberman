#include "resources/scheme_file.hpp"

#include <cctype>
#include <map>
#include <filesystem>
#include <cstdio>
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
        } else if (kind == 'P' && f.size() >= 5) {
            const int t = std::atoi(f[1].c_str());
            if (t < 0 || t >= static_cast<int>(out.powers.size())) continue;
            SchemePower& pw = out.powers[static_cast<std::size_t>(t)];
            pw.bornWith = std::atoi(f[2].c_str());
            pw.hasOverride = std::atoi(f[3].c_str()) != 0;
            pw.overrideValue = std::atoi(f[4].c_str());
            pw.forbidden = f.size() >= 6 && std::atoi(f[5].c_str()) != 0;
        }
    }
    if (rows != kGridH) return std::nullopt;
    return out;
}

std::string serializeScheme(const SchemeFile& file) {
    // Layout and comments as in the files written by the original (scheme.c).
    static const char* const kPowerText[13] = {
        "an extra bomb", "longer flame length", "a disease", "the ability to kick bombs", "extra speed",
        "the ability to punch bombs", "the ability to grab bombs", "the spooger", "goldflame", "a trigger mechanism",
        "jelly (bouncy) bombs", "super bad disease", "random"};
    std::ostringstream out;
    char buffer[128];
    out << "\r\n; NOTE! This is an Atomic Bomberman Scheme File.\r\n"
           "; Modify at your own risk.  It is machine-generated and updated.\r\n\r\n\r\n"
           "; this is an internal version control number\r\n-V,2\r\n\r\n"
           "; this is the textual name of the scheme\r\n-N," << file.name << "\r\n\r\n"
           "; scheme brick density (0-100 percent)\r\n-B," << file.scheme.brickDensity << "\r\n\r\n"
           "; actual array data (# is solid, : is brick, . is blank)\r\n;               11111\r\n;     012345678901234\r\n";
    for (int y = 0; y < kGridH; ++y) {
        std::snprintf(buffer, sizeof buffer, "-R,%2d,", y);
        out << buffer;
        for (int x = 0; x < kGridW; ++x) {
            const Tile t = file.scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            out << (t == Tile::Solid ? '#' : t == Tile::Brick ? ':' : '.');
        }
        out << "\r\n";
    }
    out << "\r\n; player starting locations (playerno,X,Y)\r\n";
    for (int p = 0; p < kMaxPlayers; ++p)
        out << "-S," << p << "," << file.scheme.start[static_cast<std::size_t>(p)].x << "," << file.scheme.start[static_cast<std::size_t>(p)].y
            << "," << file.team[static_cast<std::size_t>(p)] << "\r\n";
    out << "\r\n; powerup information; the fields are:\r\n"
           ";   powerup #, bornwith, has_override, override_value, forbidden\r\n"
           ";   (note the last text field has no effect; it is only a comment)\r\n";
    for (std::size_t t = 0; t < file.powers.size(); ++t) {
        const SchemePower& pw = file.powers[t];
        std::snprintf(buffer, sizeof buffer, "-P,%2d,%2d,%d,%2d,%2d,%s\r\n", static_cast<int>(t), pw.bornWith, pw.hasOverride ? 1 : 0,
                      pw.overrideValue, pw.forbidden ? 1 : 0, kPowerText[t]);
        out << buffer;
    }
    return out.str();
}

bool saveSchemeFile(const std::string& path, const SchemeFile& file) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << serializeScheme(file);
    return static_cast<bool>(out);
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

std::vector<SchemeEntry> listSchemes(const std::string& gameSchemesDir, const std::string& userSchemesDir) {
    std::map<std::string, SchemeEntry> byName;
    for (const std::string& dir : {gameSchemesDir, userSchemesDir}) {
        std::error_code ec;
        if (dir.empty()) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string ext = e.path().extension().string();
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext != ".sch") continue;
            std::string name = e.path().stem().string();
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (auto sf = loadSchemeFile(e.path().string())) byName[name] = {name, sf->name, e.path().string()};
        }
    }
    std::vector<SchemeEntry> out;
    for (auto& [name, entry] : byName) out.push_back(entry);
    return out;
}

}  // namespace ab
