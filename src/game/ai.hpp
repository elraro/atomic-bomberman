// Computer player. Produces a PlayerInput each tick from the public state of
// the World, exactly as a keyboard would; the core treats it like any input.
//
// Structure follows the original (docs/reverse-engineering/ai.md): a fixed
// priority list of behaviours, the first one that acts wins the tick.
// The searches work as the original's do (a flood of "walkers"); see that
// document for what was read and what remains unverified.
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
    void markWalls(const World& w);
    int danger(Cell c) const;
    bool blocked(const World& w, Cell c) const;      // tile or bomb
    bool safeWalkable(const World& w, Cell c) const; // free, no flame, no danger
    // First step of a route through unblocked cells; kNoDir if none within maxDepth passes.
    std::array<bool, kGridW * kGridH> passability(const World& w) const;
    Dir pathStep(const World& w, Cell from, Cell to, int maxDepth);
    // Nearest cell with lower danger than `from`; returns the first step and the cell.
    Dir stepToSafety(const World& w, Cell from, Cell* target);
    bool nearestPowerup(const World& w, Cell from, int maxDepth, Cell* found);
    int pickTarget(const World& w, int self);
    void press(bool& button, bool& toggle);

    Rng rng_;
    Cell spawn_{};       // where this round began for the player (the attack behaviour keeps away from it)
    int lastTick_ = -1;
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
