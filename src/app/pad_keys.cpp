#include "app/pad_keys.hpp"

#include <cstdlib>

namespace ab {

namespace {
constexpr int kStickOn = 16384;   // half deflection: a direction
constexpr int kStickOff = 9830;   // 30 %: back in the middle (so a stick at rest does not chatter)
constexpr std::uint64_t kFirstRepeatMs = 400;
constexpr std::uint64_t kRepeatMs = 130;
}  // namespace

void PadKeys::setPlaying(bool on) {
    if (on == playing_) return;
    playing_ = on;
    // What is held when the mode changes means nothing in the new one.
    held_ = -1;
}

int PadKeys::heldDirection() const {
    for (int d : dir_)
        if (d >= 0) return d;
    return -1;
}

void PadKeys::setDirection(int source, int dir, std::uint64_t nowMs) {
    int& now = dir_[static_cast<std::size_t>(source)];
    if (now == dir) return;
    now = dir;
    if (playing_) return;
    if (dir >= 0) {
        out_.push_back(static_cast<PadKey>(dir));
        held_ = dir;
        repeatAt_ = nowMs + kFirstRepeatMs;
    } else {
        held_ = heldDirection();
        repeatAt_ = nowMs + kFirstRepeatMs;
    }
}

void PadKeys::button(Button b, bool down, std::uint64_t nowMs) {
    const int i = static_cast<int>(b);
    if (i < 4) {
        dpad_[static_cast<std::size_t>(i)] = down;
        int dir = -1;
        if (down) dir = i;
        else
            for (int d = 0; d < 4; ++d)
                if (dpad_[static_cast<std::size_t>(d)]) dir = d;
        setDirection(0, dir, nowMs);
        return;
    }
    if (!down) return;
    if (b == Button::Start) {
        out_.push_back(PadKey::Menu);
        return;
    }
    if (playing_) return;
    out_.push_back(b == Button::South ? PadKey::Confirm : b == Button::East ? PadKey::Back : b == Button::West ? PadKey::Erase : PadKey::Space);
}

void PadKeys::stick(int pad, int x, int y, std::uint64_t nowMs) {
    if (pad < 0 || pad >= kSources - 1) return;
    const int source = 1 + pad;
    const int was = dir_[static_cast<std::size_t>(source)];
    const int ax = std::abs(x), ay = std::abs(y);
    int dir = was;
    if (ax < kStickOff && ay < kStickOff) dir = -1;
    else if (ax >= kStickOn || ay >= kStickOn) dir = ax > ay ? (x > 0 ? 1 : 3) : (y > 0 ? 2 : 0);
    setDirection(source, dir, nowMs);
}

void PadKeys::update(std::uint64_t nowMs) {
    if (playing_ || held_ < 0 || nowMs < repeatAt_) return;
    out_.push_back(static_cast<PadKey>(held_));
    repeatAt_ = nowMs + kRepeatMs;
}

std::vector<PadKey> PadKeys::take() {
    std::vector<PadKey> keys;
    keys.swap(out_);
    return keys;
}

}  // namespace ab
