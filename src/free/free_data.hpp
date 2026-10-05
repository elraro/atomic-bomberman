// The free asset set on disk: the parts of it that are ordinary files in the
// formats the game already reads (tuning table, arenas, level extras, help
// pages, and synthesised sounds as .wav). Written to a folder of the user's
// own on first start; the graphics are made at run time (free_art.hpp).
// Nothing here comes from the original game's files.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ab {

// True for a folder written by ensureFreeAssets (it holds the marker file).
bool isFreeAssetDir(const std::string& dir);
// Writes the set into `dir` unless an up-to-date one is there. `withSounds` off
// leaves the sounds out (the dedicated server needs none). False if the folder
// cannot be written.
bool ensureFreeAssets(const std::string& dir, bool withSounds);

// The sounds: file name -> mono samples at 22050 Hz, and the list "id -> name".
struct FreeSounds {
    std::map<std::string, std::vector<std::int16_t>> samples;
    std::vector<std::pair<int, std::string>> ids;
};
FreeSounds makeFreeSounds();

}  // namespace ab
