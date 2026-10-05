// Developer tool: writes the free graphics set as PPM sheets to look at.
//   free_art_dump OUTPUT_DIR
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "free/free_art.hpp"

namespace {
void writePpm(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    const ab::FreeArt art = ab::makeFreeArt();
    // One sheet: every sequence's frames (at most 16) in a row, on a checked background.
    const int cell = 82, cols = 16;
    const int rows = static_cast<int>(art.sequences.size());
    std::vector<std::uint8_t> sheet(static_cast<std::size_t>(cell * cols * cell * rows * 3));
    for (std::size_t i = 0; i < sheet.size() / 3; ++i) {
        const int x = static_cast<int>(i) % (cell * cols), y = static_cast<int>(i) / (cell * cols);
        const std::uint8_t v = ((x / 8 + y / 8) & 1) != 0 ? 90 : 110;
        sheet[i * 3] = sheet[i * 3 + 1] = sheet[i * 3 + 2] = v;
    }
    int row = 0;
    for (const auto& [name, frames] : art.sequences) {
        std::printf("%3d %-28s frames=%zu\n", row, name.c_str(), frames.size());
        for (int k = 0; k < cols && k < static_cast<int>(frames.size()); ++k) {
            const std::size_t pick = static_cast<std::size_t>(k) * frames.size() / static_cast<std::size_t>(std::min<int>(cols, static_cast<int>(frames.size())));
            const ab::FreeFrame& f = art.frames[static_cast<std::size_t>(frames[pick])];
            const std::vector<std::uint8_t> px = ab::colouredFrame(f, 2 + row % 7);
            for (int y = 0; y < f.height && y < cell; ++y)
                for (int x = 0; x < f.width && x < cell; ++x) {
                    const std::size_t s = static_cast<std::size_t>(y * f.width + x) * 4;
                    const std::size_t d = static_cast<std::size_t>((row * cell + y) * cell * cols + k * cell + x) * 3;
                    const int a = px[s + 3];
                    for (std::size_t c = 0; c < 3; ++c) sheet[d + c] = static_cast<std::uint8_t>((px[s + c] * a + sheet[d + c] * (255 - a)) / 255);
                }
        }
        ++row;
    }
    writePpm(dir + "/sprites.ppm", cell * cols, cell * rows, sheet);
    for (const char* name : {"mainmenu", "field0", "field2", "field7", "glue0", "glue1", "results", "draw", "victory3", "team1", "roulette"}) {
        const auto pic = ab::makeFreePicture(name);
        if (!pic) continue;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pic->width * pic->height * 3));
        for (std::size_t i = 0; i < rgb.size() / 3; ++i)
            for (std::size_t c = 0; c < 3; ++c) rgb[i * 3 + c] = pic->rgba[i * 4 + c];
        writePpm(dir + "/" + name + ".ppm", pic->width, pic->height, rgb);
    }
    return 0;
}
