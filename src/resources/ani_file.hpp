// Reader for original .ANI files (sprite frames + named sequences).
// Format: docs/reverse-engineering/file-formats.md
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ab {

struct AniFrame {
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    bool hasKey = false;
    std::uint16_t key = 0;
    std::vector<std::uint16_t> pixels;  // RGB555, row-major
};

struct AniStep {
    int frame = 0;  // index into AniFile::frames
    int dx = 0;
    int dy = 0;
};

struct AniSequence {
    std::string name;
    std::vector<AniStep> steps;
};

struct AniFile {
    std::vector<AniFrame> frames;
    std::vector<AniSequence> sequences;
};

std::optional<AniFile> parseAni(const std::vector<std::uint8_t>& data);
std::optional<AniFile> loadAniFile(const std::string& path);

// 8-bit PCX image with its own 256-colour palette (used for backgrounds).
struct PcxImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> indices;
    std::vector<std::uint8_t> palette;  // 768 bytes, 8-bit RGB
};
std::optional<PcxImage> loadPcxFile(const std::string& path);

// color.pal: 256 RGB triples (6-bit) followed by a 32768-entry RGB555 -> index table.
struct GamePalette {
    std::vector<std::uint8_t> rgb6;    // 768
    std::vector<std::uint8_t> lookup;  // 32768
};
std::optional<GamePalette> loadPaletteFile(const std::string& path);

std::optional<std::vector<std::uint8_t>> readFileBytes(const std::string& path);

}  // namespace ab
