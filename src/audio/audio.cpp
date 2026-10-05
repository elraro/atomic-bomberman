#include "audio/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace ab {

namespace {
constexpr int kMaxVoices = 8;
constexpr SDL_AudioSpec kRssSpec = {SDL_AUDIO_S16LE, 2, 22050};
}  // namespace

Audio::~Audio() {
    if (music_ != nullptr) SDL_DestroyAudioStream(music_);
    for (SDL_AudioStream* v : voices_) SDL_DestroyAudioStream(v);
    if (device_ != 0) SDL_CloseAudioDevice(device_);
}

bool Audio::init(const std::string& gameDir) {
    soundDir_ = gameDir + "/data/sound/";
    rng_ ^= static_cast<std::uint32_t>(SDL_GetPerformanceCounter());  // a different pick each run
    std::ifstream in(gameDir + "/data/res/soundlst.res", std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "WARN  soundlst.res not found, audio disabled\n");
        return false;
    }
    // Lines of "id,name"; ';' starts a comment.
    std::string line;
    while (std::getline(in, line)) {
        if (auto semi = line.find(';'); semi != std::string::npos) line.erase(semi);
        const auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        char* end = nullptr;
        const long id = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str()) continue;
        std::string name = line.substr(comma + 1);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }),
                   name.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (!name.empty()) names_[static_cast<int>(id)] = name;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "WARN  SDL audio init failed (%s), audio disabled\n", SDL_GetError());
        return false;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (device_ == 0) {
        std::fprintf(stderr, "WARN  no audio device (%s), audio disabled\n", SDL_GetError());
        return false;
    }
    ready_ = true;
    std::fprintf(stderr, "INFO  Audio ready sounds=%zu\n", names_.size());
    return true;
}

const std::vector<std::uint8_t>* Audio::load(const std::string& name) {
    auto it = cache_.find(name);
    if (it != cache_.end()) return &it->second;
    // An imported asset folder holds .wav files; an original game folder holds raw .rss.
    std::ifstream in(soundDir_ + name + ".wav", std::ios::binary);
    const bool wav = static_cast<bool>(in);
    if (!wav) {
        in.clear();
        in.open(soundDir_ + name + ".rss", std::ios::binary);
    }
    if (!in) {
        std::fprintf(stderr, "WARN  sound file missing name=%s\n", name.c_str());
        return nullptr;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (wav) {
        // Keep only the contents of the "data" chunk (the importer writes a 44-byte header).
        std::size_t start = data.size();
        for (std::size_t i = 12; i + 8 <= data.size();) {
            const std::size_t size = static_cast<std::size_t>(data[i + 4]) | (static_cast<std::size_t>(data[i + 5]) << 8) |
                                     (static_cast<std::size_t>(data[i + 6]) << 16) | (static_cast<std::size_t>(data[i + 7]) << 24);
            if (data[i] == 'd' && data[i + 1] == 'a' && data[i + 2] == 't' && data[i + 3] == 'a') {
                start = i + 8;
                break;
            }
            i += 8 + size + (size & 1);
        }
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min(start, data.size())));
    }
    return &cache_.emplace(name, std::move(data)).first->second;
}

void Audio::playSeries(int baseId) {
    if (!ready_ || names_.find(baseId) == names_.end()) return;
    int last = baseId;
    while (names_.find(last + 1) != names_.end()) ++last;
    int pick = baseId;
    for (int tries = 0; tries < 8; ++tries) {
        rng_ = rng_ * 1664525u + 1013904223u;
        pick = baseId + static_cast<int>((rng_ >> 16) % static_cast<unsigned>(last - baseId + 1));
        const auto it = lastOfSeries_.find(baseId);
        if (last == baseId || it == lastOfSeries_.end() || it->second != pick) break;
    }
    lastOfSeries_[baseId] = pick;
    playRange(pick, pick);
}

void Audio::playRange(int firstId, int lastId) {
    if (!ready_) return;
    std::vector<const std::string*> candidates;
    for (auto it = names_.lower_bound(firstId); it != names_.end() && it->first <= lastId; ++it)
        candidates.push_back(&it->second);
    if (candidates.empty()) return;
    rng_ = rng_ * 1664525u + 1013904223u;
    const std::string& name = *candidates[(rng_ >> 16) % candidates.size()];
    const std::vector<std::uint8_t>* data = load(name);
    if (data == nullptr || data->empty()) return;

    update();
    if (voices_.size() >= static_cast<std::size_t>(kMaxVoices)) {
        SDL_DestroyAudioStream(voices_.front());  // oldest voice gives way
        voices_.erase(voices_.begin());
    }
    SDL_AudioStream* stream = SDL_CreateAudioStream(&kRssSpec, &kRssSpec);
    if (stream == nullptr || !SDL_BindAudioStream(device_, stream)) {
        if (stream != nullptr) SDL_DestroyAudioStream(stream);
        return;
    }
    SDL_PutAudioStreamData(stream, data->data(), static_cast<int>(data->size()));
    SDL_FlushAudioStream(stream);
    voices_.push_back(stream);
}

void Audio::playMusic(int id) {
    if (!ready_) return;
    const auto it = names_.find(id);
    if (it == names_.end()) return;
    const std::vector<std::uint8_t>* data = load(it->second);
    if (data == nullptr || data->empty() || data == musicData_) return;
    if (music_ != nullptr) SDL_DestroyAudioStream(music_);
    music_ = SDL_CreateAudioStream(&kRssSpec, &kRssSpec);
    if (music_ == nullptr || !SDL_BindAudioStream(device_, music_)) {
        if (music_ != nullptr) SDL_DestroyAudioStream(music_);
        music_ = nullptr;
        musicData_ = nullptr;
        return;
    }
    SDL_SetAudioStreamGain(music_, 0.5f);  // keep effects audible over the tune
    musicData_ = data;
    SDL_PutAudioStreamData(music_, data->data(), static_cast<int>(data->size()));
    std::fprintf(stderr, "INFO  Music started name=%s\n", it->second.c_str());
}

void Audio::stopMusic() {
    if (music_ != nullptr) SDL_DestroyAudioStream(music_);
    music_ = nullptr;
    musicData_ = nullptr;
}

void Audio::update() {
    // Loop: queue the tune again shortly before it runs out.
    if (music_ != nullptr && musicData_ != nullptr && SDL_GetAudioStreamQueued(music_) < 22050 * 4)
        SDL_PutAudioStreamData(music_, musicData_->data(), static_cast<int>(musicData_->size()));
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](SDL_AudioStream* s) {
                                     if (SDL_GetAudioStreamQueued(s) > 0) return false;
                                     SDL_DestroyAudioStream(s);
                                     return true;
                                 }),
                  voices_.end());
}

}  // namespace ab
