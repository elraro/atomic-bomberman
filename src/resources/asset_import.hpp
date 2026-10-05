// Builds the modern game's asset folder from a user's copy of the original
// Atomic Bomberman. Only the files the game uses are taken; sounds are
// converted from headerless .rss to standard .wav. Nothing is downloaded and
// nothing in the source folder is changed.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ab {

struct ImportReport {
    bool ok = false;
    int dataFiles = 0;     // palettes, fonts, pictures, schemes, animations, lists
    int sounds = 0;        // converted to .wav
    int missingSounds = 0; // listed in soundlst.res but absent in the source
    std::vector<std::string> missing;  // their names as listed
    int extraSounds = 0;   // converted although the game's sound list does not name them
    long long bytes = 0;
    int failed = 0;        // files that could not be written (then ok is false)
    bool cancelled = false;
    std::string error;
};

// `source` is the folder that contains bm95.exe / COLOR.PAL / DATA (an install
// or the CD). File and folder names are matched without regard to case.
// How far a conversion has got: `done` of `total` files, and the one in hand.
struct ImportProgress {
    int done = 0;
    int total = 0;
    std::string item;     // file being copied or converted
    bool sounds = false;  // converting sounds (the long part); otherwise copying data files
};

// True if `folder` holds a copy of the original game (color.pal and data/res/valuelst.res,
// names in any case).
bool looksLikeOriginalGame(const std::string& folder);
// `folder` itself if it is one, else the first folder directly inside it that is (a CD
// copied as a whole often ends up one level down); empty if neither.
std::string findOriginalGame(const std::string& folder);

// `allSounds`: also the sound files the game's own sound list does not name (about as many
// again, never played by the game; the sound test screen can play them).
// `progress`, if given, is called before each file; returning false stops the work
// (the report then says cancelled, and the destination is incomplete).
ImportReport importAssets(const std::string& source, const std::string& destination, bool allSounds = false,
                          const std::function<bool(const ImportProgress&)>& progress = {});

// Keeping a converted folder up to date by itself (the Android app; --import-folder elsewhere):
// the user leaves a copy of the original game in a folder, and the program converts it when
// there is no converted folder yet or when that one was made by another release.
//
// The converted folder carries a note naming the release that made it.
std::string importStamp(const std::string& converted);  // empty: none (or not made this way)
// True if `converted` has to be (re)made for `release`.
bool importIsDue(const std::string& converted, const std::string& release);
// Converts `source` into `converted` for `release`. The work is done in a folder beside it
// ("<converted>.new") that replaces the old one only when everything went well, so that a
// conversion that fails or is stopped leaves what was there untouched.
ImportReport importForRelease(const std::string& source, const std::string& converted, const std::string& release,
                              const std::function<bool(const ImportProgress&)>& progress = {});

}  // namespace ab
