// Deterministic gameplay core. No platform, rendering or audio dependencies.
//
// Rules implemented here follow docs/specifications/. Each rule was derived
// from the original game by static analysis; see docs/reverse-engineering/.
//
// Not implemented yet: punch, grab, spooge, flying bombs, duds, diseases,
// level extras (arrows, warps, conveyors, trampolines), closing walls,
// team play, campaign, AI, networking.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "game/geometry.hpp"
#include "game/values.hpp"

namespace ab {

enum class Tile : std::uint8_t { Blank = 0, Solid = 1, Brick = 2 };

enum PowerupType : int {
    kPowBomb = 0,
    kPowFlame = 1,
    kPowDisease = 2,
    kPowKicker = 3,
    kPowSkate = 4,
    kPowPunch = 5,
    kPowGrab = 6,
    kPowSpooge = 7,
    kPowGoldflame = 8,
    kPowTrigger = 9,
    kPowJelly = 10,
    kPowSuperDisease = 11,
    kPowRandom = 12,
    kPowClog = 13,
    kPowTypeCount = 15,
};

inline constexpr int kMaxPlayers = 10;
inline constexpr int kMaxBombs = 100;
inline constexpr int kFrameMs = 50;  // derived from value 30 at construction

// Pseudo-random source. The sequence of the original's generator has not
// been matched; only the distribution of each draw follows the original.
class Rng {
public:
    explicit Rng(std::uint32_t seed = 1) : state_(seed) {}
    int next() {
        state_ = state_ * 1103515245u + 12345u;
        return static_cast<int>((state_ >> 16) & 0x7fff);
    }
    int below(int n) { return n > 0 ? next() % n : 0; }

private:
    std::uint32_t state_;
};

struct PlayerInput {
    std::array<bool, 4> dir{};  // north, east, south, west held
    bool button1 = false;       // bomb
    bool button2 = false;       // action
};

struct Player {
    bool present = false;
    bool alive = false;
    int x = 0;
    int y = 0;
    Dir facing = 2;
    int baseSpeed = 0;
    int fuseFrames = 0;
    std::array<int, kPowTypeCount> inventory{};
    int moveAcc = 0;
    int triggerBombsLaid = 0;
    bool prevButton1 = false;
    bool prevButton2 = false;
    int kills = 0;
    int killedBy = -1;  // player index, or -1
    bool dying = false;  // death animation running
    int dyingFrames = 0;
    int dyingAcc = 0;
};

enum class BombType : std::uint8_t { Regular = 0, Trigger = 1, Jelly = 2 };
enum class BombMode : std::uint8_t { Resting = 0, Sliding = 1 };

struct Bomb {
    bool active = false;
    int owner = -1;
    BombType type = BombType::Regular;
    BombMode mode = BombMode::Resting;
    int x = 0;
    int y = 0;
    int range = 0;
    int fuseMs = 0;
    int elapsedMs = 0;
    Dir dir = 0;
    int speed = 0;
    int moveAcc = 0;
    int arrivedFrom = 0;  // 0 = own fuse; otherwise direction + 1 pointing back at the trigger
    bool stopRequested = false;
    int createdTick = 0;
};

struct Flame {
    bool active = false;
    bool burningBrick = false;
    int owner = -1;
    int ageMs = 0;
};

enum class PowerupState : std::uint8_t { None = 0, Hidden = 1, Revealed = 2 };

struct Powerup {
    PowerupState state = PowerupState::None;
    int type = 0;
};

// Layout of solid / brick / blank cells plus start cells; the gameplay part
// of an original scheme file.
struct Scheme {
    std::array<std::array<Tile, kGridW>, kGridH> tiles{};
    int brickDensity = 100;  // percent of brick cells kept
    std::array<Cell, kMaxPlayers> start{};

    // Blank field with solid pillars at odd column and odd row.
    static Scheme pillars();
    // Rows of 15 characters: '#' solid, ':' brick, '.' blank.
    static Scheme fromRows(const std::array<const char*, kGridH>& rows);
};

class World {
public:
    World(const Values& values, std::uint32_t seed);

    // Builds the tile grid (applying brick density) and, if requested, hides
    // powerups under bricks. Removes all players, bombs and flames.
    void startRound(const Scheme& scheme, bool generatePowerups);
    void addPlayer(int index);

    // Advances the simulation by dtMs milliseconds (clamped to value 31).
    void tick(int dtMs, const std::array<PlayerInput, kMaxPlayers>& input);

    // --- state access ---
    Tile tile(Cell c) const;  // outside the grid: Solid
    void setTile(Cell c, Tile t);
    const Player& player(int i) const { return players_[static_cast<std::size_t>(i)]; }
    Player& player(int i) { return players_[static_cast<std::size_t>(i)]; }
    const std::vector<Bomb>& bombs() const { return bombs_; }
    const Flame& flame(Cell c) const { return flames_[index(c)]; }
    const Powerup& powerup(Cell c) const { return powerups_[index(c)]; }
    void placePowerup(Cell c, int type, PowerupState state);
    int tickCount() const { return tickCount_; }
    int roundMs() const { return roundMs_; }
    int alivePlayers() const;
    // Living players plus players who died less than value 25 frames ago.
    int contenders() const { return contenders_; }
    int activeBombs() const;
    int bombsOwnedBy(int playerIndex) const;
    const Bomb* bombAt(Cell c) const;

    // Places a bomb for a player as if the bomb button had been accepted.
    // Returns false when no slot is free.
    bool createBomb(int owner, Cell c, BombType type, int range, int fuseFrames);

    // Skips the start-of-round input freeze (for tests and tools).
    void endStartFreeze() { startFreezeMs_ = 0; }

private:
    static std::size_t index(Cell c) { return static_cast<std::size_t>(c.y * kGridW + c.x); }

    bool playerPassable(Cell c) const;
    Bomb* findBomb(Cell c);
    bool anyPlayerAt(Cell c) const;

    void updateBombs(int dt);
    void slideBomb(Bomb& b, int dt);
    bool bombCanSlideInto(Cell c);
    void queueDetonation(Bomb& b, int arrivedFrom);
    void detonate(Bomb& b);
    void createFlame(Cell c, int owner, bool burningBrick);
    void destroyPowerup(Cell c);
    void revealPowerup(Cell c);
    void scatterPowerup(int type);
    void updateFlames(int dt);

    void updatePlayers(int dt, const std::array<PlayerInput, kMaxPlayers>& input);
    void updatePlayer(int i, int dt, const PlayerInput& in);
    Dir chooseDirection(const Player& p, const PlayerInput& in) const;
    int playerSpeed(const Player& p) const;
    bool movePlayer(int i, Dir requested);  // returns true if the player died
    bool checkFlameDeath(int i);
    void checkPickup(int i);
    void pickUp(Player& p, int type);
    void removeFromInventory(Player& p, int type);
    void kickBomb(Bomb& b, Dir d);
    void handleButtons(int i, const PlayerInput& in);
    void dropBomb(int i);

    Values values_;
    Rng rng_;
    int frameMs_ = kFrameMs;
    int tickCount_ = 0;
    int roundMs_ = 0;
    int startFreezeMs_ = 0;
    int contenders_ = 0;

    std::array<Tile, kGridW * kGridH> tiles_{};
    std::array<Flame, kGridW * kGridH> flames_{};
    std::array<Powerup, kGridW * kGridH> powerups_{};
    std::array<Player, kMaxPlayers> players_{};
    std::array<Cell, kMaxPlayers> startCells_{};
    std::vector<Bomb> bombs_;

    struct Pending {
        int bomb;
        int arrivedFrom;
    };
    std::vector<Pending> pending_;
};

}  // namespace ab
