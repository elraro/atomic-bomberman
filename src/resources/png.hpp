// A small PNG reader and writer, for the enlarged pictures (upscale.hpp): no library needed.
// Reads 8-bit grey, grey+alpha, RGB, RGBA and palette images that are not interlaced (what
// the upscaling program writes); writes RGB or RGBA with the data stored, not compressed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ab {

struct PngImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width*height*4
};

std::optional<PngImage> decodePng(const std::vector<std::uint8_t>& data);
std::optional<PngImage> loadPngFile(const std::string& path);

// `rgba` holds width*height*4 bytes; without `withAlpha` the alpha is left out of the file.
std::vector<std::uint8_t> encodePng(int width, int height, const std::uint8_t* rgba, bool withAlpha);
bool writePngFile(const std::string& path, int width, int height, const std::uint8_t* rgba, bool withAlpha);

// The raw DEFLATE stream (RFC 1951) of `in`; empty on a damaged stream.
std::optional<std::vector<std::uint8_t>> inflateRaw(const std::uint8_t* in, std::size_t size, std::size_t expected = 0);

}  // namespace ab
