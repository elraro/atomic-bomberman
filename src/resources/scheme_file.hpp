// Reader for original Atomic Bomberman scheme files (data/schemes/*.sch).
// Legacy format code stays here; the game core only sees ab::Scheme.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "game/match.hpp"
#include "game/world.hpp"

namespace ab {

struct SchemeFile {
    Scheme scheme;
    std::string name;
    std::array<int, kMaxPlayers> team{};
    std::array<SchemePower, 13> powers{};
};

// The text of a scheme file as the original's editor writes it.
std::string serializeScheme(const SchemeFile& file);
bool saveSchemeFile(const std::string& path, const SchemeFile& file);

// Parses the text of a .sch file. Lines: -N,name  -B,density
// -R,row,15 cells ('#' solid, ':' brick, '.' blank)  -S,player,x,y,team
// -P,powerup,bornwith,has_override,override_value,forbidden,comment
std::optional<SchemeFile> parseSchemeText(const std::string& text);
std::optional<SchemeFile> loadSchemeFile(const std::string& path);

// Level extras (data/res/extraN.res). Lines:
//   -A,dir,x,y   arrow        -C,dir,x,y        conveyor      (dir: N E S W)
//   -T,x,y       trampoline   -W,type,id,x,y,linkto  warp hole
// Negative coordinates count from the right / bottom edge. "H" coordinates
// (trampolines) mean a random position, returned as cell (-1,-1) for the
// game core to place.
std::vector<Extra> parseExtrasText(const std::string& text);
std::vector<Extra> loadExtrasFile(const std::string& path);  // empty if the file does not exist

struct SchemeEntry {
    std::string file;   // name without extension, lower case
    std::string title;  // the scheme's own name
    std::string path;
};

// Schemes of the game data and of the user's folder (a user file hides a
// game file of the same name), sorted by file name.
std::vector<SchemeEntry> listSchemes(const std::string& gameSchemesDir, const std::string& userSchemesDir);

}  // namespace ab
