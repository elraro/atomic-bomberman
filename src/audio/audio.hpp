// Sound playback through SDL3. Plays the original's .rss files (headerless
// 22050 Hz stereo 16-bit PCM) read from the user's game directory at run time.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_AudioStream;

namespace ab {

class Audio {
public:
    ~Audio();
    // Opens the default playback device and reads soundlst.res. Returns false
    // (and stays silent) if either is unavailable.
    bool init(const std::string& gameDir);
    // Plays one sound chosen at random among the ids defined in [firstId, lastId].
    void playRange(int firstId, int lastId);
    // Starts a looping tune by sound id (stops the previous one). Level music is id 1100 + level.
    void playMusic(int id);
    void stopMusic();
    // Releases finished voices and keeps the music looping. Call once per frame.
    void update();

private:
    const std::vector<std::uint8_t>* load(const std::string& name);

    bool ready_ = false;
    std::uint32_t device_ = 0;
    std::string soundDir_;
    std::map<int, std::string> names_;  // sound id -> file name without extension
    std::unordered_map<std::string, std::vector<std::uint8_t>> cache_;
    std::vector<SDL_AudioStream*> voices_;
    SDL_AudioStream* music_ = nullptr;
    const std::vector<std::uint8_t>* musicData_ = nullptr;
    std::uint32_t rng_ = 12345;
};

}  // namespace ab
