// What a round is set up from, and how rounds add up to a match. Shared by the
// local game and by both ends of a network game, so that a round built from the
// same RoundSetup is the same round everywhere.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "game/world.hpp"

namespace ab {

// Per-powerup settings of a scheme (-P lines), for types 0-12.
struct SchemePower {
    int bornWith = 0;          // > 0: every player starts with this many
    bool hasOverride = false;  // replace the level's count of this powerup
    int overrideValue = 0;     // n >= 0: that many; n < 0: |n| tries at 1 in 10
    bool forbidden = false;    // never produced by the "random" powerup
};

inline constexpr int kInfinitePlayTime = 1001;  // play time setting meaning "no clock"

struct RoundSetup {
    int level = 0;                       // theme 0-10: selects the level rules (ice, regeneration)
    Scheme scheme;                       // start cells in the order they are to be used
    std::array<SchemePower, 13> powers{};
    std::vector<Extra> extras;           // the level's arrows, warps, conveyors, trampolines
    int conveyorSpeed = 1;               // 0-2
    bool teamPlay = false;
    std::array<int, kMaxPlayers> teams{};
    int enclosementDepth = 1;            // 0-3
    bool stompedBombsDetonate = true;
    bool diseasesDestroyable = true;
    int playTime = 150;                  // seconds; kInfinitePlayTime or more: no clock
    bool winByKills = false;
    bool campaign = false;
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> human{};  // not a computer player (the ice delay is for humans)
};

// Starts the round in `world`. `base` holds the tuning values the world was made with;
// the scheme's powerup rules are applied on top of them (original 0x404630).
void applyRoundSetup(World& world, const Values& base, const RoundSetup& setup);

// Round wins and kills of a match (original results code 0x42AB04).
struct MatchScore {
    std::array<int, kMaxPlayers> wins{};   // per player, or per team (0, 1) in team play
    std::array<int, kMaxPlayers> kills{};  // per player, over the rounds of the match
    int matchWinner = -1;                  // player or team once the match is decided

    // To be called once when a round is decided. Returns its winner (player, or team in
    // team play), -1 for a draw. The match goes to the first with `winsNeeded` round wins,
    // or with `winByKills` (not in team play) to the single player with the most kills
    // once that total reaches the target.
    int roundDecided(const World& world, int winsNeeded, bool winByKills);
};

}  // namespace ab
