// Developer tool: decodes an Interplay MVE movie (or a file that contains one)
// and writes chosen pictures as PPM files and the sound as a WAV file.
//   mve_dump FILE OUTPREFIX [frame numbers...]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include "resources/mve_file.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mve_dump FILE OUTPREFIX [frame numbers...]\n");
        return 2;
    }
    ab::MveDecoder movie;
    if (!movie.open(argv[1])) {
        std::fprintf(stderr, "no MVE movie found in %s\n", argv[1]);
        return 1;
    }
    std::set<int> wanted;
    for (int i = 3; i < argc; ++i) wanted.insert(std::atoi(argv[i]));
    std::vector<std::int16_t> sound;
    long clipped = 0;
    while (movie.nextFrame()) {
        for (std::int16_t s : movie.takeAudio()) {
            clipped += s == 32767 || s == -32768 ? 1 : 0;
            sound.push_back(s);
        }
        if (wanted.count(movie.framesDecoded()) != 0) {
            std::ofstream out(std::string(argv[2]) + std::to_string(movie.framesDecoded()) + ".ppm", std::ios::binary);
            out << "P6\n" << movie.width() << " " << movie.height() << "\n255\n";
            const auto& px = movie.rgba();
            for (std::size_t i = 0; i < px.size(); i += 4) out.write(reinterpret_cast<const char*>(&px[i]), 3);
        }
    }
    std::printf("frames=%d size=%dx%d frame=%.4fs sound=%zu samples %d Hz %d ch clipped=%ld (%.1fs)\n", movie.framesDecoded(),
                movie.width(), movie.height(), movie.frameSeconds(), sound.size(), movie.sampleRate(), movie.channels(), clipped,
                static_cast<double>(sound.size()) / movie.channels() / movie.sampleRate());
    // A minimal WAV file.
    std::ofstream wav(std::string(argv[2]) + ".wav", std::ios::binary);
    const std::uint32_t bytes = static_cast<std::uint32_t>(sound.size() * 2);
    auto u32 = [&](std::uint32_t v) { wav.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { wav.write(reinterpret_cast<const char*>(&v), 2); };
    wav << "RIFF";
    u32(36 + bytes);
    wav << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(movie.channels()));
    u32(static_cast<std::uint32_t>(movie.sampleRate()));
    u32(static_cast<std::uint32_t>(movie.sampleRate() * movie.channels() * 2));
    u16(static_cast<std::uint16_t>(movie.channels() * 2));
    u16(16);
    wav << "data";
    u32(bytes);
    wav.write(reinterpret_cast<const char*>(sound.data()), static_cast<std::streamsize>(bytes));
    return 0;
}
