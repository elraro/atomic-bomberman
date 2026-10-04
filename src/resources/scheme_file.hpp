// Reader for original Atomic Bomberman scheme files (data/schemes/*.sch).
// Legacy format code stays here; the game core only sees ab::Scheme.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "game/world.hpp"

namespace ab {

struct SchemeFile {
    Scheme scheme;
    std::string name;
    std::array<int, kMaxPlayers> team{};
};

// Parses the text of a .sch file. Lines: -N,name  -B,density
// -R,row,15 cells ('#' solid, ':' brick, '.' blank)  -S,player,x,y,team
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

}  // namespace ab
