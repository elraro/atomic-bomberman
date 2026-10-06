// Game settings, with the names and file format of the original's options.ini
// ("key=value" lines), so an original file can be read as it is.
#pragma once

#include <array>
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
    bool smallMemory = false;         // smallmemory: the original's "normal" (not enhanced) memory model
    int musicVolume = 100;            // music_volume, sound_volume: 0-100 (not in the original, whose
    int soundVolume = 100;            // "Adjust Audio" screen was never made)
    bool assignKeyboards = true;      // assign_keyboards: a keyboard player in the default player list
    bool freeAssets = false;          // free_assets: play with the free graphics and sounds although original data is there (not in the original)
    // Network play (this implementation's client-server mode; not the original's keys).
    std::string netName;              // net_name: the player's name; empty until first set
    std::string netAddress;           // net_address: the server last joined
    std::string netServerName;        // net_server_name: name of a game hosted from the menu
    int netPort = 27410;              // net_port: port of a game hosted from the menu
    std::string netRelay;             // net_relay: relay to register a hosted game with ("host[:port]"), or empty
    bool netPrediction = true;        // net_prediction: show own moves at once instead of after the round trip
    // keydef=set,index,code: two keyboard sets of six keys (up, right, down, left,
    // action 1, action 2) as DirectInput key codes, which are PC scan codes. Defaults
    // as in the original: arrows, Space, Enter; and R, G, F, D, S, A.
    static constexpr int kKeySets = 2;
    static constexpr int kKeysPerSet = 6;
    std::array<std::array<int, kKeysPerSet>, kKeySets> keys{{{0xC8, 0xCD, 0xD0, 0xCB, 0x39, 0x1C}, {0x13, 0x22, 0x21, 0x20, 0x1F, 0x1E}}};
    void resetKeys() { keys = Settings().keys; }

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
