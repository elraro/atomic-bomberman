// Computer player. Produces a PlayerInput each tick from the public state of
// the World, exactly as a keyboard would; the core treats it like any input.
//
// Structure follows the original (docs/reverse-engineering/ai.md): a fixed
// priority list of behaviours, the first one that acts wins the tick.
// The path searches and the danger values of bombs are re-implemented, not
// transcribed; see that document for what is and is not faithful.
#pragma once

#include <array>

#include "game/world.hpp"

namespace ab {

class AiPlayer {
public:
    explicit AiPlayer(std::uint32_t seed = 1) : rng_(seed) {}
    PlayerInput decide(const World& world, int self, int dtMs);

private:
    using Grid = std::array<int, kGridW * kGridH>;

    void buildDanger(const World& w);
    int danger(Cell c) const;
    bool blocked(const World& w, Cell c) const;      // tile or bomb
    bool safeWalkable(const World& w, Cell c) const; // free, no flame, no danger
    // First step of a shortest path through unblocked cells; kNoDir if none within maxDepth.
    Dir pathStep(const World& w, Cell from, Cell to, int maxDepth) const;
    // Nearest cell with lower danger than `from`; returns the first step and the cell.
    Dir stepToSafety(const World& w, Cell from, Cell* target) const;
    bool nearestPowerup(const World& w, Cell from, int maxDepth, Cell* found) const;
    int pickTarget(const World& w, int self);
    void press(bool& button, bool& toggle);

    Rng rng_;
    Grid danger_{};
    Dir wanderDir_ = 2;
    bool fleeing_ = false;
    Cell fleeTarget_{};
    bool seekingPowerup_ = false;
    Cell powerupCell_{};
    int powerupMs_ = 0;
    bool hunting_ = false;
    int huntTarget_ = -1;
    int huntMs_ = 0;
    bool lastButton1_ = false;
    bool lastButton2_ = false;
};

}  // namespace ab
