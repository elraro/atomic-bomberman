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
inline constexpr int kFuseFrames = 41;
inline constexpr int kBaseSpeed = 42;
inline constexpr int kStartInventory = 50;   // + powerup id
inline constexpr int kSkateSpeed = 90;
inline constexpr int kClogSpeed = 91;
inline constexpr int kOverpowerSeconds = 102;
inline constexpr int kDiseasesDestroyable = 120;
inline constexpr int kKickSpeed = 300;
inline constexpr int kLevelCount = 400;       // + powerup id
inline constexpr int kInventoryCap = 550;     // + powerup id
}  // namespace vid

}  // namespace ab
