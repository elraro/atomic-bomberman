// Draws the bombs of the original again (see resources/bomb_redraw.hpp) for every frame of a
// file and every player colour, and writes them as one picture, to see the result without the game.
//
//   bomb_sheet ASSET_DIR HIRES_DIR OUT.png [FILE=bombs] [SCALE=4]
//
// Row = frame, column = plain (the game's green), then the player colours 0..9; each cell is
// shown on a grey ground. The enlarged sprites of HIRES_DIR (hires/ani/<file>.hani) supply
// the fuse cap; where one is missing the original is enlarged by repeating its pixels.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "resources/ani_file.hpp"
#include "resources/bomb_redraw.hpp"
#include "resources/hires_pack.hpp"
#include "resources/png.hpp"
#include "resources/sprite_refine.hpp"

using namespace ab;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: bomb_sheet ASSET_DIR HIRES_DIR OUT.png [FILE=bombs] [SCALE=4]\n");
        return 2;
    }
    const std::string assets = argv[1], hires = argv[2], out = argv[3];
    const std::string stem = argc > 4 ? argv[4] : "bombs";
    const int scale = argc > 5 ? std::max(1, std::atoi(argv[5])) : 4;
    const auto palette = loadPaletteFile(assets + "/color.pal");
    const auto file = loadAniFile(assets + "/data/ani/" + stem + ".ani");
    if (!palette || !file) {
        std::fprintf(stderr, "cannot read the palette or %s.ani\n", stem.c_str());
        return 1;
    }
    std::vector<std::vector<std::uint8_t>> remap(10);
    for (int c = 0; c < 10; ++c)
        if (auto r = readFileBytes(assets + "/" + std::to_string(c) + ".rmp"); r && r->size() >= 256) remap[static_cast<std::size_t>(c)].assign(r->begin(), r->begin() + 256);

    const auto pack = HiresPack::open(hires + "/ani/" + stem + ".hani");
    int cellW = 0, cellH = 0;
    for (const AniFrame& f : file->frames) {
        cellW = std::max(cellW, f.width * scale);
        cellH = std::max(cellH, f.height * scale);
    }
    cellW += 4;
    cellH += 4;
    const int columns = 11;
    const int rows = static_cast<int>(file->frames.size());
    const int width = columns * cellW, height = rows * cellH;
    std::vector<std::uint8_t> sheet(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < sheet.size(); i += 4) {
        sheet[i] = sheet[i + 1] = sheet[i + 2] = 120;
        sheet[i + 3] = 255;
    }
    int redrawn = 0, skipped = 0;
    for (int row = 0; row < rows; ++row) {
        const AniFrame& f = file->frames[static_cast<std::size_t>(row)];
        if (f.width < 1 || f.height < 1 || f.pixels.empty()) continue;
        const std::vector<std::uint8_t> plain = aniFrameRgba(f, *palette, nullptr);
        for (int column = 0; column < columns; ++column) {
            const std::vector<std::uint8_t>* table = (column == 0 || remap[static_cast<std::size_t>(column - 1)].empty()) ? nullptr : &remap[static_cast<std::size_t>(column - 1)];
            PngImage big;
            const std::string path = hires + "/ani/" + stem + "/" + std::to_string(row) + ".png";
            std::optional<PngImage> img;
            if (pack)
                if (const auto bytes = pack->read(row, -1)) img = decodePng(*bytes);
            if (!img) img = loadPngFile(path);  // the folder of earlier runs
            if (img && img->width == f.width * scale && img->height == f.height * scale) {
                big = std::move(*img);
            } else {  // repeat the pixels
                big.width = f.width * scale;
                big.height = f.height * scale;
                big.rgba.resize(static_cast<std::size_t>(big.width) * static_cast<std::size_t>(big.height) * 4);
                for (int y = 0; y < big.height; ++y)
                    for (int x = 0; x < big.width; ++x)
                        std::copy_n(&plain[(static_cast<std::size_t>(y / scale) * f.width + static_cast<std::size_t>(x / scale)) * 4], 4, &big.rgba[(static_cast<std::size_t>(y) * big.width + x) * 4]);
            }
            refineSilhouette(big, f.width, f.height, plain);
            if (redrawBomb(big, f, *palette, table)) ++redrawn;
            else ++skipped;
            for (int y = 0; y < big.height; ++y)
                for (int x = 0; x < big.width; ++x) {
                    const std::uint8_t* p = &big.rgba[(static_cast<std::size_t>(y) * big.width + x) * 4];
                    std::uint8_t* q = &sheet[(static_cast<std::size_t>(row * cellH + 2 + y) * width + static_cast<std::size_t>(column * cellW + 2 + x)) * 4];
                    for (int k = 0; k < 3; ++k) q[k] = static_cast<std::uint8_t>((p[k] * p[3] + q[k] * (255 - p[3])) / 255);
                }
        }
    }
    if (!writePngFile(out, width, height, sheet.data(), false)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("%d cells redrawn, %d not a ball; %dx%d written to %s\n", redrawn, skipped, width, height, out.c_str());
    return 0;
}
