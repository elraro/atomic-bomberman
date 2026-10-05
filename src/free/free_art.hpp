// The free graphics set: every picture the game needs, drawn by code. Nothing
// here is taken from the original game's artwork; people without a copy of the
// original play with these. Pure computation (no OpenGL), so it can be tested.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ab {

// One image. Pixels marked in `tint` hold a grey level that is multiplied by the
// colour of the player the sprite is drawn for.
struct FreeFrame {
    int width = 0;
    int height = 0;
    int hotX = 0;  // the point of the image that is put on the object's position
    int hotY = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> tint;  // one byte per pixel: 1 = player colour
};

struct FreeArt {
    std::vector<FreeFrame> frames;
    // Animation sequences by the names the renderer asks for; each entry is a frame index.
    std::map<std::string, std::vector<int>> sequences;
};

struct FreePicture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct FreeFont {
    int height = 0;
    int spacing = 0;
    int atlasWidth = 0;
    std::vector<int> x;      // per character code 0-127: left edge in the atlas
    std::vector<int> width;
    std::vector<std::uint8_t> alpha;  // atlasWidth * height
};

// Colours of the ten players, 0-1.
extern const float kFreePlayerColour[10][3];

// All sprites: players, bombs, flames, powerups, extras, enemies, and the tiles of every level.
FreeArt makeFreeArt();
// A frame's pixels with the player colour applied (colour -1: white).
std::vector<std::uint8_t> colouredFrame(const FreeFrame& frame, int colour);
// A full-screen picture by the name the game uses ("mainmenu", "field3", "glue0",
// "results", "draw", "victory2", "team1", "roulette", "title"); nullopt for unknown names.
std::optional<FreePicture> makeFreePicture(const std::string& name);
FreeFont makeFreeFont();

}  // namespace ab
