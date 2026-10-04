#include "game/roulette.hpp"

#include <algorithm>
#include <cmath>

namespace ab {

Roulette::Roulette(const Values& values, std::uint32_t seed) : values_(values), rng_(seed) {
    slot_ = std::max(1, values_.get(vid::kRouletteResolution));
    direction_ = rng_.below(2) * 2 - 1;
    wheel_ = rng_.below(resolution());
    pointer_ = rng_.below(resolution());
    wheelSpeed_ = rng_.below(20) + 20;
    pointerSpeed_ = rng_.below(20) + wheelSpeed_;
}

// Moves one ring by max(speed, 6) positions. While slowing down, every slot
// boundary passed takes one off the speed; the ring stops on a boundary.
int Roulette::advance(int* position, int direction, int* speed) {
    int crossed = 0;
    if (*speed <= 0) return 0;
    const int moves = std::max(6, *speed);
    for (int i = 0; i < moves; ++i) {
        *position = (*position + direction + resolution()) % resolution();
        if (*position % slot_ != 0) continue;
        ++crossed;
        if (state_ == State::Slowing && --*speed == 0) break;
    }
    return crossed;
}

int Roulette::step() {
    const int ticks = advance(&pointer_, direction_, &pointerSpeed_);
    advance(&wheel_, -direction_, &wheelSpeed_);
    if (wheelSpeed_ == 0 && pointerSpeed_ == 0 && state_ != State::Stopped) {
        const int ahead = (pointer_ < wheel_ ? pointer_ + resolution() : pointer_) - wheel_;
        prize_ = kPrize[static_cast<std::size_t>((ahead / slot_) % kSlots)];
        state_ = State::Stopped;
    }
    return ticks;
}

bool Roulette::press() {
    if (state_ == State::Spinning) state_ = State::Slowing;
    return state_ == State::Stopped;
}

void Roulette::screenPosition(int position, float* x, float* y) const {
    const double turn = 2.0 * 3.1415926 / static_cast<double>(resolution());
    const double ax = static_cast<double>(position * values_.get(vid::kRouletteLissajousX)) * turn;
    const double ay = static_cast<double>(position * values_.get(vid::kRouletteLissajousY)) * turn;
    *x = static_cast<float>(std::lround(values_.get(vid::kRouletteCentreX) + values_.get(vid::kRouletteRadiusX) * std::cos(ax)));
    *y = static_cast<float>(std::lround(values_.get(vid::kRouletteCentreY) + values_.get(vid::kRouletteRadiusY) * std::sin(ay)));
}

}  // namespace ab
