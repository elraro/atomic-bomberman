// Decoder for Interplay MVE movies (8-bit palettised video, 16-bit PCM or
// Interplay DPCM sound), as used by the intro movie of the original game.
// The movie may sit inside another file (the original ships it appended to
// its small player program, intro/bmintro.exe): the signature is searched for.
// Format after the public descriptions of the MVE container and of the
// "Interplay MVE video" block coding; no code of the original is involved.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ab {

class MveDecoder {
public:
    // Reads the whole file; false if no movie is found in it.
    bool open(const std::string& path);
    // The same from memory (the bytes are copied).
    bool openBytes(std::vector<std::uint8_t> bytes);

    // Decodes up to and including the next picture. False at the end of the movie.
    bool nextFrame();

    int width() const { return width_; }
    int height() const { return height_; }
    double frameSeconds() const { return frameSeconds_; }
    int framesDecoded() const { return frames_; }
    // The current picture, width * height * 4 bytes of RGBA.
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    // Sound decoded since the last call: interleaved signed 16-bit samples.
    std::vector<std::int16_t> takeAudio();

    // Byte range of the movie inside the opened file (for extracting it).
    std::size_t movieOffset() const { return start_; }
    const std::vector<std::uint8_t>& fileBytes() const { return data_; }

private:
    bool decodeVideo(const std::uint8_t* p, std::size_t size);
    void decodeAudio(const std::uint8_t* p, std::size_t size);
    void present();

    std::vector<std::uint8_t> data_;
    std::size_t start_ = 0;
    std::size_t pos_ = 0;
    bool ended_ = false;

    int width_ = 0;
    int height_ = 0;
    double frameSeconds_ = 1.0 / 15.0;
    int frames_ = 0;
    std::vector<std::uint8_t> current_, last_, secondLast_;  // palette indices
    std::vector<std::uint8_t> map_;                          // one 4-bit code per 8x8 block
    std::uint8_t palette_[256][3] = {};
    std::vector<std::uint8_t> rgba_;

    int sampleRate_ = 22050;
    int channels_ = 1;
    bool sixteenBit_ = true;
    bool compressed_ = false;
    std::vector<std::int16_t> audio_;
};

}  // namespace ab
