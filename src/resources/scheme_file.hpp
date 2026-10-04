// Reader for original Atomic Bomberman scheme files (data/schemes/*.sch).
// Legacy format code stays here; the game core only sees ab::Scheme.
#pragma once

#include <optional>
#include <string>

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

}  // namespace ab
