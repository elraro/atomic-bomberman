// The "goldman" roulette shown between rounds (original FUN_004034BC): the
// winner of the last round spins for a prize that is added to the starting
// inventory in the next round. Pure logic; drawing and sound are the front end's.
#pragma once

#include <array>

#include "game/values.hpp"
#include "game/world.hpp"

namespace ab {

class Roulette {
public:
    enum class State { Spinning, Slowing, Stopped };
    static constexpr int kSlots = 6;
    // Powerup type in each slot of the wheel (table at 0x45B7BC):
    // bomb, flame, kicker, goldflame, skate, clog.
    static constexpr std::array<int, kSlots> kPrize{0, 1, 3, 8, 4, 13};

    Roulette(const Values& values, std::uint32_t seed);

    // One frame of the original's loop. Returns how many slot boundaries the
    // pointer crossed (one "tick" sound each).
    int step();
    // The action key: starts the slow-down. Returns true once the wheel has
    // stopped, which is when the key leaves the screen.
    bool press();

    State state() const { return state_; }
    int prize() const { return prize_; }  // powerup type, or -1 until stopped
    int resolution() const { return slot_ * kSlots; }
    int wheel() const { return wheel_; }      // position of slot 0
    int pointer() const { return pointer_; }  // position of the ring
    int slotPosition(int slot) const { return (wheel_ + slot * slot_) % resolution(); }
    // Screen position of a point on the wheel's path.
    void screenPosition(int position, float* x, float* y) const;

private:
    int advance(int* position, int direction, int* speed);

    const Values& values_;
    Rng rng_;
    int slot_ = 70;
    int direction_ = 1;
    int wheel_ = 0;
    int pointer_ = 0;
    int wheelSpeed_ = 0;
    int pointerSpeed_ = 0;
    State state_ = State::Spinning;
    int prize_ = -1;
};

}  // namespace ab
