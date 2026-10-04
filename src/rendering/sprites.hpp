// Sprites built from the user's copy of the original data files.
// Converts frames to GL textures on demand, applying the original palette
// lookup and the per-player colour remap.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/ani_file.hpp"

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
    int level() const { return level_; }

    unsigned background() const { return background_; }
    int sequenceLength(const std::string& name) const;
    // Sprite for step `index` (wrapped) of a named sequence; colour -1 = unmodified,
    // 0-9 = player colour remap.
    std::optional<Sprite> sprite(const std::string& sequence, int index, int colour);

private:
    struct Ref {
        std::size_t file;
        std::size_t seq;
    };
    bool addAni(const std::string& path);
    unsigned makeTexture(const AniFrame& f, int colour);

    bool loaded_ = false;
    int level_ = 0;
    GamePalette palette_;
    std::array<std::vector<std::uint8_t>, 10> remap_;
    std::vector<AniFile> files_;
    std::unordered_map<std::string, Ref> sequences_;
    std::map<std::tuple<std::size_t, int, int>, unsigned> textures_;
    unsigned background_ = 0;
};

}  // namespace ab
