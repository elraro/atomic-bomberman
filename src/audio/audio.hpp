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
    // smallMemory: the original's "normal" memory model, which keeps one sound per series.
    bool init(const std::string& gameDir, bool smallMemory = false);
    // Reads soundlst.res again and chooses a new selection of the voice series (the
    // original does this when its sound cache is flushed, every value 7 seconds).
    void chooseSounds();
    // Seconds since the selection was last chosen.
    double selectionAge() const;
    // For the debug window: bytes of sound data held, and how often a sound was already loaded.
    std::size_t cachedBytes() const;
    int cacheHitPercent() const { return loads_ > 0 ? hits_ * 100 / loads_ : 0; }
    // Volumes 0..100.
    void setVolumes(int music, int effects);
    // Plays one sound chosen at random among the ids defined in [firstId, lastId].
    void playRange(int firstId, int lastId);
    // As the original's sound call (0x427961): one of the run of consecutive ids that
    // starts at baseId, not the one played last from that run; nothing if baseId is not defined.
    void playSeries(int baseId);
    // Every sound file of the game data, for the sound test: also those the game's sound
    // list does not name (the disc holds about as many again as the game uses).
    struct Entry {
        std::string name;        // file name without extension
        std::vector<int> ids;    // its numbers in the sound list; empty: not used by the game
        double seconds = 0.0;
    };
    std::vector<Entry> catalogue() const;
    // Plays one of them by name, alone: whatever effect was sounding is cut off.
    void playNamed(const std::string& name);
    void stopEffects();
    // Starts a looping tune by sound id (stops the previous one). Level music is id 1100 + level.
    void playMusic(int id);
    void stopMusic();
    // A sound track fed piece by piece (the intro movie): interleaved signed 16-bit samples.
    bool beginStream(int sampleRate, int channels);
    void pushStream(const std::vector<std::int16_t>& samples);
    void endStream();
    // Releases finished voices and keeps the music looping. Call once per frame.
    void update();

private:
    const std::vector<std::uint8_t>* load(const std::string& name);
    void startVoice(const std::vector<std::uint8_t>& data);

    bool ready_ = false;
    std::uint32_t device_ = 0;
    std::string soundDir_;
    std::map<int, std::string> names_;  // sound id -> file name without extension
    std::unordered_map<std::string, std::vector<std::uint8_t>> cache_;
    std::vector<SDL_AudioStream*> voices_;
    SDL_AudioStream* music_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    const std::vector<std::uint8_t>* musicData_ = nullptr;
    void cull(int firstId, int lastId, int keep);

    std::string gameDir_;
    bool smallMemory_ = false;
    std::uint64_t chosenAt_ = 0;
    int loads_ = 0;
    int hits_ = 0;
    float musicGain_ = 0.5f;
    float effectsGain_ = 1.0f;
    std::uint32_t rng_ = 12345;
    std::map<int, int> lastOfSeries_;
};

}  // namespace ab
