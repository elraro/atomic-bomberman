#include "resources/settings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ab {

namespace {
constexpr std::array<int, 9> kPlayTimes{60, 90, 120, 150, 180, 240, 300, 600, Settings::kInfiniteTime};
}

void Settings::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto semi = line.find(';'); semi != std::string::npos) line.erase(semi);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            std::size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            s.erase(0, i);
        };
        trim(key);
        trim(value);
        const int n = std::atoi(value.c_str());
        if (key == "levelno") level = n;
        else if (key == "num_to_win_match") winsNeeded = n;
        else if (key == "enclosement_depth") enclosementDepth = n;
        else if (key == "conveyor_speed") conveyorSpeed = n;
        else if (key == "team_play") teamPlay = n != 0;
        else if (key == "random_start") randomStart = n != 0;
        else if (key == "stomped_bombs_detonate") stompedBombsDetonate = n != 0;
        else if (key == "win_by_kills") winByKills = n != 0;
        else if (key == "goldman") goldman = n != 0;
        else if (key == "playtime") playTime = n;
        else if (key == "diseases_destroyable") diseasesDestroyable = n != 0;
        else if (key == "disable_game_music") disableGameMusic = n != 0;
        else if (key == "schemefilename") {
            if (const auto dot = value.rfind('.'); dot != std::string::npos) value.erase(dot);
            if (!value.empty()) scheme = value;
        }
    }
    level = std::clamp(level, -1, 10);
    winsNeeded = std::max(1, winsNeeded);
    enclosementDepth = std::clamp(enclosementDepth, 0, 3);
    conveyorSpeed = std::clamp(conveyorSpeed, 0, 2);
    if (playTime < 60) playTime = 60;
    if (playTime != kInfiniteTime && playTime > 600) playTime = 600;
}

std::string Settings::serialize() const {
    std::ostringstream out;
    out << "; Atomic Bomberman settings (same keys as the original's options.ini)\n"
        << "levelno=" << level << "\nnum_to_win_match=" << winsNeeded << "\nenclosement_depth=" << enclosementDepth
        << "\nconveyor_speed=" << conveyorSpeed << "\nteam_play=" << teamPlay << "\nrandom_start=" << randomStart
        << "\nstomped_bombs_detonate=" << stompedBombsDetonate << "\nwin_by_kills=" << winByKills << "\ngoldman=" << goldman
        << "\nschemefilename=" << scheme << ".SCH\nplaytime=" << playTime << "\ndiseases_destroyable=" << diseasesDestroyable
        << "\ndisable_game_music=" << disableGameMusic << "\n";
    return out.str();
}

bool Settings::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << serialize();
    return static_cast<bool>(out);
}

int Settings::nextPlayTime(int current, int direction) {
    const int n = static_cast<int>(kPlayTimes.size());
    int i = 0;
    while (i < n - 1 && kPlayTimes[static_cast<std::size_t>(i)] < current) ++i;
    return kPlayTimes[static_cast<std::size_t>((i + (direction < 0 ? n - 1 : 1)) % n)];
}

std::string Settings::playTimeText(int seconds) {
    if (seconds >= kInfiniteTime) return "Infinite";  // message 280
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%d:%02d", seconds / 60, seconds % 60);  // message 281
    return buffer;
}

}  // namespace ab
