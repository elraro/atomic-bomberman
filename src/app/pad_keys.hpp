// A gamepad as the keys the screens are worked with: the d-pad or the left stick
// are the arrows (repeating while held), the four face buttons confirm, go back,
// erase and add a space, Start opens a menu. While a player is steering in a
// match only Start counts; everything else belongs to the player.
// No SDL here: the program maps SDL's buttons in and key codes out.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ab {

enum class PadKey { Up, Right, Down, Left, Confirm, Back, Erase, Space, Menu };

class PadKeys {
public:
    enum class Button { Up, Right, Down, Left, South, East, West, North, Start, Count };

    // A match is being played with the pads: only Start is passed on.
    void setPlaying(bool on);
    void button(Button b, bool down, std::uint64_t nowMs);
    // The most gamepads at once: one per seat of a match.
    static constexpr int kMaxPads = 10;
    // The left stick of pad `pad` (0 to kMaxPads - 1), each axis -32768..32767.
    void stick(int pad, int x, int y, std::uint64_t nowMs);
    // Once per frame: a held direction repeats.
    void update(std::uint64_t nowMs);
    // The keys pressed since the last call.
    std::vector<PadKey> take();

private:
    void setDirection(int source, int dir, std::uint64_t nowMs);
    int heldDirection() const;

    static constexpr int kSources = 1 + kMaxPads;  // the d-pads (taken together), then one stick per pad
    bool playing_ = false;
    std::array<bool, 4> dpad_{};
    std::array<int, kSources> dir_ = [] {
        std::array<int, kSources> d{};
        d.fill(-1);
        return d;
    }();
    int held_ = -1;               // the direction that repeats
    std::uint64_t repeatAt_ = 0;
    std::vector<PadKey> out_;
};

}  // namespace ab
