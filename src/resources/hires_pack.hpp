// One file per .ani for the enlarged frames (resources/upscale.hpp), instead of a folder of PNGs:
//   hires/ani/<file>.hani
// Our own container, little-endian:
//   "HANI", u32 version (1), u32 scale, u32 count
//   count x { u32 frame, u32 colour (0xFFFFFFFF: the plain frame, else player colour 0-9), u32 offset, u32 size }
//   the PNG files, as they are; offset and size count from the start of the file
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ab {

struct HiresPackItem {
    int frame = 0;
    int colour = -1;  // -1: plain
    std::vector<std::uint8_t> png;
};

// Writes the container (the items are sorted by frame and colour). False if the file cannot be written.
bool writeHiresPack(const std::string& path, int scale, std::vector<HiresPackItem> items);

class HiresPack {
public:
    // Reads the directory only; the pictures are read when asked for. Nothing for a missing or damaged file.
    static std::optional<HiresPack> open(const std::string& path);

    int scale() const { return scale_; }
    std::size_t size() const { return entries_.size(); }
    bool has(int frame, int colour) const;
    // The PNG file of a frame (colour -1: plain); nothing if the container has none.
    std::optional<std::vector<std::uint8_t>> read(int frame, int colour) const;
    std::vector<std::pair<int, int>> keys() const;  // (frame, colour), sorted

private:
    struct Entry {
        int frame;
        int colour;
        std::uint32_t offset;
        std::uint32_t size;
    };
    const Entry* find(int frame, int colour) const;

    std::string path_;
    int scale_ = 0;
    std::vector<Entry> entries_;  // sorted by (frame, colour)
};

// "12" or "12_c3": the name of a frame's PNG file without the extension.
std::string hiresFrameName(int frame, int colour);
// The inverse; false if the name is neither.
bool parseHiresFrameName(const std::string& name, int* frame, int* colour);

// A folder of "<frame>.png" and "<frame>_c<N>.png" files (the form the upscaling works in) into a
// container, and back.
bool packHiresFolder(const std::string& dir, int scale, const std::string& packFile);
bool unpackHiresPack(const std::string& packFile, const std::string& dir);

}  // namespace ab
