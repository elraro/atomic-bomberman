// Game settings, with the names and file format of the original's options.ini
// ("key=value" lines), so an original file can be read as it is.
#pragma once

#include <string>

namespace ab {

struct Settings {
    int level = 0;                    // levelno
    int winsNeeded = 2;               // num_to_win_match
    int enclosementDepth = 1;         // enclosement_depth: 0 none, 1 a little, 2 a lot, 3 all the way
    int conveyorSpeed = 1;            // conveyor_speed: 0 low, 1 medium, 2 high
    bool teamPlay = false;            // team_play
    bool randomStart = false;         // random_start
    bool stompedBombsDetonate = true; // stomped_bombs_detonate
    bool winByKills = false;          // win_by_kills
    bool goldman = false;             // goldman (the roulette)
    std::string scheme = "BASIC";     // schemefilename, stored with .SCH
    int playTime = 150;               // playtime in seconds; kInfiniteTime = no limit
    bool diseasesDestroyable = true;  // diseases_destroyable
    bool disableGameMusic = false;    // disable_game_music

    static constexpr int kInfiniteTime = 1001;

    // Unknown keys are ignored; out-of-range values are clamped as the original does.
    void parse(const std::string& text);
    std::string serialize() const;
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // The play-time choices of the original's settings screen, in order:
    // 1:00 1:30 2:00 2:30 3:00 4:00 5:00 10:00 Infinite, wrapping.
    static int nextPlayTime(int current, int direction);
    static std::string playTimeText(int seconds);
};

}  // namespace ab
