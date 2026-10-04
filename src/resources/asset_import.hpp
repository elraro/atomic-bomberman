// Builds the modern game's asset folder from a user's copy of the original
// Atomic Bomberman. Only the files the game uses are taken; sounds are
// converted from headerless .rss to standard .wav. Nothing is downloaded and
// nothing in the source folder is changed.
#pragma once

#include <string>

namespace ab {

struct ImportReport {
    bool ok = false;
    int dataFiles = 0;     // palettes, fonts, pictures, schemes, animations, lists
    int sounds = 0;        // converted to .wav
    int missingSounds = 0; // listed in soundlst.res but absent in the source
    long long bytes = 0;
    std::string error;
};

// `source` is the folder that contains bm95.exe / COLOR.PAL / DATA (an install
// or the CD). File and folder names are matched without regard to case.
ImportReport importAssets(const std::string& source, const std::string& destination);

}  // namespace ab
