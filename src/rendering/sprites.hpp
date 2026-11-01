// Sprites built from the user's copy of the original data files, or, for a
// folder of the free asset set, drawn by the program (free/free_art.hpp).
// Converts frames to GL textures on demand, applying the original palette
// lookup and the per-player colour remap.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "free/free_art.hpp"
#include "resources/ani_file.hpp"
#include "resources/hires_pack.hpp"
#include "resources/png.hpp"

namespace ab {

struct Sprite {
    unsigned texture = 0;
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    int dx = 0;  // per-step offset from the sequence
    int dy = 0;
};

class SpriteBank {
public:
    ~SpriteBank();

    // Loads the palette, remap tables, the level background and the animation
    // files needed in a match. Returns false if the essential files are missing.
    bool load(const std::string& gameDir, int level);
    bool loaded() const { return loaded_; }
    bool freeSet() const { return free_; }
    int level() const { return level_; }
    // Makes the tiles of another level theme available too ("tile N solid", "tile N brick").
    void ensureTiles(int level);
    // For the debug keys: every animation sequence loaded, and the texture bytes uploaded so far.
    std::vector<std::string> sequenceNames() const;
    static std::size_t textureBytes();

    unsigned background() const { return background_; }
    // A full-screen picture from data/res (e.g. "mainmenu", "glue0", "results"); 0 if missing.
    unsigned picture(const std::string& name);
    // Pixel size of a picture loaded with picture(); false if it is missing.
    bool pictureSize(const std::string& name, int* width, int* height);

    // Text font (font1.fon) as one texture: glyphs side by side, white on transparent.
    struct Font {
        unsigned texture = 0;
        int height = 0;
        int spacing = 0;
        int atlasWidth = 0;
        std::vector<int> x;      // left edge of each glyph in the atlas
        std::vector<int> width;
    };
    const Font& font() const { return font_; }
    int sequenceLength(const std::string& name) const;
    // Sprite for step `index` (wrapped) of a named sequence; colour -1 = unmodified,
    // 0-9 = player colour remap.
    std::optional<Sprite> sprite(const std::string& sequence, int index, int colour);

    // Enlarged copies of the graphics made by --upscale-assets (resources/upscale.hpp) are used
    // when the "hires" folder next to the asset folder has them; off with --no-hires.
    static void setHiresEnabled(bool on);
    static void setHiresSmoothEdges(bool on);  // outline of enlarged sprites from the original mask (default on)
    bool hires() const { return !hiresDir_.empty(); }

private:
    // A texture from the enlarged picture `path` if there is one that is a whole multiple of
    // the logical size w x h (the size the game draws it at); else 0.
    // `post`: changes the enlarged frame before it is uploaded (the sprite's outline is smoothed, a bomb is drawn again); none for a picture.
    unsigned hiresTexture(const std::string& path, int w, int h, const std::function<void(PngImage&)>& post = {}) const;
    // The same for a decoded picture (from a .hani container); `label` names it in the warning.
    unsigned hiresTextureFrom(std::optional<PngImage> img, const std::string& label, int w, int h, const std::function<void(PngImage&)>& post) const;
    struct Ref {
        std::size_t file;
        std::size_t seq;
    };
    bool addAni(const std::string& path);
    bool loadFree(int level);

    bool free_ = false;  // the free asset set: frames from art_ instead of files_
    FreeArt art_;
    std::map<std::pair<int, int>, unsigned> freeTextures_;
    unsigned makeTexture(const AniFrame& f, int colour);

    bool loaded_ = false;
    std::array<bool, 11> tilesLoaded_{};
    int level_ = 0;
    GamePalette palette_;
    std::array<std::vector<std::uint8_t>, 10> remap_;
    std::vector<AniFile> files_;
    std::vector<std::string> fileStems_;  // file name of each of files_ without folder and extension
    std::string hiresDir_;                // empty: no enlarged pictures
    std::vector<std::optional<HiresPack>> hiresPacks_;  // parallel to files_: hires/ani/<stem>.hani, if there is one
    std::unordered_map<std::string, Ref> sequences_;
    std::map<std::tuple<std::size_t, int, int>, unsigned> textures_;
    unsigned background_ = 0;
    std::string gameDir_;
    std::unordered_map<std::string, unsigned> pictures_;
    std::unordered_map<std::string, std::pair<int, int>> pictureSizes_;
    Font font_;
};

}  // namespace ab
