// The pictures that Steam asks for when the game is added as a "non-Steam game" (Customize ->
// Artwork) or has a store page, and the icon for its shortcut, made from the game's own graphics:
// the level background, the players, a bomb and the game's font. Pure computation (no OpenGL).
//
//   capsule_600x900.png        main capsule        (Steam: "Hauptkapsel")
//   hero_3840x1240.png         background          (Steam: "Hintergrund")
//   logo_1280x720.png          logo, transparent   (Steam: "Logo")
//   wide_capsule_920x430.png   wide main capsule   (Steam: "Weite Hauptkapsel")
//   icon_256.png, icon.ico     icon of the shortcut
//
// With the imported original data the pictures show the original's artwork, so they are as
// copyrighted as it is and are not to be shared; with the free graphics set (no original data
// given, or `free`) nothing of the original is in them. Enlarged sprites made by --upscale-assets
// are used where there are some.
#pragma once

#include <string>
#include <vector>

namespace ab {

struct SteamArtOptions {
    std::string assetsDir;   // imported game data; empty or without color.pal: the free graphics set
    std::string outDir;      // where the pictures go; default: "steam" beside the asset folder
    bool free = false;       // use the free set even if original data is given
    bool useHires = true;    // use the enlarged sprites of hiresDirFor(assetsDir) where they exist
};

struct SteamArtReport {
    bool ok = false;
    std::string error;
    std::string source;              // "original" or "free"
    std::string outDir;
    std::vector<std::string> files;  // names of the files written, in outDir
};

SteamArtReport makeSteamArt(const SteamArtOptions& options);

}  // namespace ab
