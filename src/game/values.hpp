// Tuning table. The original reads every gameplay constant from
// data/res/valuelst.res by numeric id; the modern core does the same.
#pragma once

#include <string>
#include <unordered_map>

namespace ab {

class Values {
public:
    // Ids used by the core, with the values shipped in Atomic Bomberman 1.0.
    static Values defaults();

    // Parses the text format "id,value[,value...] ; comment". Entries with
    // several values occupy consecutive ids, as in the original.
    // Returns false if the file cannot be opened.
    bool loadFile(const std::string& path);
    void parse(const std::string& text);

    int get(int id) const;  // throws std::out_of_range for an undefined id
    void set(int id, int value) { table_[id] = value; }
    bool has(int id) const { return table_.contains(id); }

private:
    std::unordered_map<int, int> table_;
};

// Ids referenced by name in the core.
namespace vid {
inline constexpr int kFlameFrames = 10;
inline constexpr int kBrickBurnFrames = 20;
inline constexpr int kFramesPerSecond = 30;
inline constexpr int kMaxTickMs = 31;
inline constexpr int kEnclosementDepth = 27;
inline constexpr int kFuseFrames = 41;
inline constexpr int kWallsDetonateBombs = 46;
inline constexpr int kRoundSeconds = 100;
inline constexpr int kHurrySeconds = 101;
inline constexpr int kBaseSpeed = 42;
inline constexpr int kStartInventory = 50;   // + powerup id
inline constexpr int kSkateSpeed = 90;
inline constexpr int kClogSpeed = 91;
inline constexpr int kOverpowerSeconds = 102;
inline constexpr int kDeathAnimations = 105;
inline constexpr int kDiseasesDestroyable = 120;
inline constexpr int kDiseasesMultiply = 123;
inline constexpr int kDiseasesCurable = 124;
inline constexpr int kDiseaseCureChance = 125;
inline constexpr int kDiseasePassCooldown = 129;
inline constexpr int kDiseaseFrames = 130;      // + disease number
inline constexpr int kConveyorSpeed = 190;    // + speed setting 0-2
inline constexpr int kKickSpeed = 300;
inline constexpr int kPunchSpeed = 301;
inline constexpr int kJellyTurnChance = 667;
inline constexpr int kHeadHitLossMin = 670;
inline constexpr int kHeadHitLossRandom = 671;
inline constexpr int kTrampolineFrames = 680;
inline constexpr int kTrampolineRise = 681;
inline constexpr int kDudMinSeconds = 320;
inline constexpr int kDudRandomSeconds = 321;
inline constexpr int kDudChance = 322;
inline constexpr int kDudFrames = 323;
inline constexpr int kLevelCount = 400;       // + powerup id
inline constexpr int kInventoryCap = 550;     // + powerup id
}  // namespace vid

}  // namespace ab
