// Deterministic gameplay core. No platform, rendering or audio dependencies.
//
// Rules implemented here follow docs/specifications/. Each rule was derived
// from the original game by static analysis; see docs/reverse-engineering/.
//
// Not implemented yet: duds, diseases, level extras (arrows, warps,
// conveyors, trampolines), team play, campaign, AI, networking.
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

// Diseases, by the original's number.
enum Disease : int {
    kDisSlow = 0,        // speed / 3
    kDisFast = 1,        // speed x 1.5
    kDisNoBombs = 2,     // cannot drop bombs
    kDisDropBombs = 3,   // drops a bomb whenever it can
    kDisShortFlame = 4,  // blast range 1
    kDisFastDrop = 5,    // fast and drops bombs
    kDisShortFuse = 6,   // fuse / 3
    kDisSwap = 7,        // swap places with another player (instant)
    kDisReverse = 8,     // reversed controls
    kDiseaseCount = 9,
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

// Fixed features of a level theme (original extraN.res).
enum class ExtraType : std::uint8_t { Arrow = 0, Warp = 1, Conveyor = 2, Trampoline = 3 };

struct Extra {
    ExtraType type = ExtraType::Arrow;
    Cell cell{};
    Dir dir = 0;          // arrows and conveyors
    int id = 0;           // warps
    int linkTo = 0;       // warps: id of the destination warp
    bool prepared = false;
    int animFrame = 0;    // trampolines: > 0 while the spring animation runs
};

// Campaign enemies (original aliens.c).
enum class AlienType : std::uint8_t { Rover = 1, Ghost = 2 };

struct Alien {
    bool active = false;
    AlienType type = AlienType::Rover;
    int x = 0;
    int y = 0;
    Dir dir = 0;
    int speed = 0;        // 100ths of a pixel per 50 ms frame, from the campaign file
    int moveAcc = 0;
    int anim = 0;         // +1 per pixel
    bool dead = false;    // burnt: removed on its next update
    bool clearedStart = false;
};

inline constexpr int kActionCornerhead = 20;
inline constexpr int kCornerheadFrames = 50;  // length of every cornerhead sequence

enum class Special : std::uint8_t { None, Trampoline, WarpOut, WarpIn };

// Things that happened during a tick, for the front end (sound, effects).
// The core never depends on what is done with them.
enum class EventKind : std::uint8_t {
    BombDropped,
    BombExploded,
    BombKicked,
    BombStopped,
    BombPunched,
    BombBounced,
    BombGrabbed,
    BombThrown,
    WallBlock,
    Hurry,
    PlayerDied,
    HeadHit,
    Pickup,
};

struct Event {
    EventKind kind;
    int player = -1;
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
    int animCounter = 0;  // +1 per pixel walked (the walk animation advances every 3)
    bool moving = false;  // moved during the last tick
    int triggerBombsLaid = 0;
    int holding = -1;     // index of the bomb being carried, or -1
    int stunTicks = 0;    // no input while > 0 (after a bomb lands on the player)
    std::array<bool, kDiseaseCount> disease{};
    int diseaseMs = 0;         // 0 = healthy; otherwise time since infection
    int diseaseDurationMs = 0;
    int diseaseCooldown = 0;   // ticks before the disease can be passed on
    int deathAnim = 0;         // 1..value 105, chosen at death
    // 0 none, 1 kicking, 2 punching, 3 picking up a bomb, kActionCornerhead + n: trapped animation n
    int action = 0;
    PlayerInput lastInput{};   // what the player last acted on (kept during the pickup pause)
    int actionFrames = 0;
    int actionAcc = 0;
    Special special = Special::None;  // no input and no death while not None
    int specialFrames = 0;
    int specialAcc = 0;
    int warpX = 0;                    // destination while warping
    int warpY = 0;
    bool prevButton1 = false;
    bool prevButton2 = false;
    int team = 0;       // 0 or 1; only meaningful in team play
    int kills = 0;
    int killedBy = -1;  // player index, or -1
    bool human = true;   // campaign: only human players respawn and are hurt by enemies
    int lives = 0;       // campaign: respawns left
    int score = 0;       // campaign points
    bool gold = false;   // won the roulette before this round (twinkles at the start)
    bool dying = false;  // death animation running
    int dyingFrames = 0;
    int dyingAcc = 0;
};

enum class BombType : std::uint8_t { Regular = 0, Trigger = 1, Jelly = 2 };
enum class BombMode : std::uint8_t { Resting = 0, Sliding = 1, Flying = 2, Held = 3 };

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
    int hops = 0;        // cell centres passed while flying; landing is tried from the third
    int flightPx = 0;    // pixels travelled since the last bounce (for drawing the arc)
    int holder = -1;     // player carrying the bomb
    bool dud = false;    // fizzles for a while before its fuse starts
    int dudFrames = 0;
    int dudAcc = 0;
};

struct Flame {
    bool active = false;
    bool burningBrick = false;
    int owner = -1;
    int ageMs = 0;
    Dir dir = kNoDir;  // kNoDir: centre of the blast; otherwise the arm's direction
    bool tip = false;  // last cell of an arm at full range
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
    // Roulette prize for the winner of the previous round: one more of a powerup
    // type in the starting inventory (no cap is applied, as in the original).
    void grantPrize(int index, int powerupType);
    // True while the gold player's sparkles are still spawned (value 1010 seconds; 0 = always).
    bool goldTwinkling() const;
    // Team play: the round is decided when one team is left. Teams come from the
    // scheme (two teams, 0 and 1). Set before adding players.
    void setTeamPlay(bool on, const std::array<int, kMaxPlayers>& teams = {});
    bool teamPlay() const { return teamPlay_; }
    // Colour a player is drawn in: own colour, or the team's (white / red) in team play
    // once the opening "true colours" period (value 32) is over.
    int displayColour(int playerIndex) const;
    int winningTeam() const;  // team play: the surviving team, or -1
    // Level extras for this round (call after startRound). Speed setting 0-2 selects the conveyor speed.
    void setExtras(const std::vector<Extra>& extras, int conveyorSpeedSetting = 1);
    const std::vector<Extra>& extras() const { return extras_; }
    const Extra* extraAt(Cell c) const;

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

    // Events since the last call; the list is cleared.
    std::vector<Event> takeEvents();

    // Skips the start-of-round input freeze (for tests and tools).
    void endStartFreeze() { startFreezeMs_ = 0; }

    // --- campaign mode (original campaign.c / aliens.c) ---
    // Set before the players are added. The last-player-standing rule is off; the stage
    // is cleared two seconds after the last enemy dies and failed when the clock runs
    // out or no human player is left.
    void setCampaign(bool on) { campaign_ = on; }
    bool campaign() const { return campaign_; }
    void setHuman(int index, bool human);
    // Places enemies on random non-solid cells more than three cells from every player.
    void spawnAliens(AlienType type, int count, int speed);
    const std::vector<Alien>& aliens() const { return aliens_; }
    // 0 while the stage runs, 1 cleared, 2 failed.
    int campaignResult() const { return campaignResult_; }
    // After a failure: true if the same stage is to be played again (no human was left).
    bool campaignRetry() const { return campaignRetry_; }

    // --- round clock and result ---
    // Seconds left on the round clock (whole seconds, as displayed); -1 when unlimited.
    int secondsLeft() const;
    void setRoundSeconds(int seconds) { roundLimitMs_ = seconds < 0 ? -1 : seconds * 1000; }
    void setEnclosementDepth(int depth) { enclosementDepth_ = depth; }
    // The next cells the closing walls will reach, nearest first (empty while they are
    // not running). The computer players treat these as dangerous.
    std::vector<Cell> upcomingWallCells(int count) const;
    // Changes one tuning value (options that the original stores as values); takes
    // effect where the value is next read, for most of them at the next startRound.
    void setValue(int id, int value) { values_.set(id, value); }
    // "Win matches by kill total": a suicide no longer costs a kill.
    void setWinByKills(bool on) { winByKills_ = on; }
    bool hurry() const;                  // the "hurry" warning is showing
    bool timeUp() const { return secondsLeft() == 0; }
    // The round ends when at most one contender is left, or when the clock reads 0:00 (a draw).
    bool roundOver() const {
        if (campaign_) return campaignResult_ != 0;
        return tickCount_ > 0 && (contenders_ <= 1 || timeUp());
    }
    int winner() const;                  // index of the surviving player, or -1 for a draw
    int closedCells() const { return wallsClosed_; }

private:
    static std::size_t index(Cell c) { return static_cast<std::size_t>(c.y * kGridW + c.x); }

    bool playerPassable(Cell c) const;
    Bomb* findBomb(Cell c);
    bool anyPlayerAt(Cell c) const;

    void updateBombs(int dt);
    void slideBomb(Bomb& b, int dt);
    void flyBomb(Bomb& b, int dt);
    void launchBomb(Bomb& b, Dir d);
    void hitOnHead(int playerIndex);
    void giveDisease(int playerIndex);
    void cureDiseases(Player& p);
    void updateDisease(int playerIndex, int dt);
    int playerAt(Cell c) const;  // index of a living player in the cell, or -1
    bool bombCanSlideInto(Cell c);
    void queueDetonation(Bomb& b, int arrivedFrom);
    void detonate(Bomb& b);
    void createFlame(Cell c, int owner, bool burningBrick, Dir dir = kNoDir, bool tip = false);
    void destroyPowerup(Cell c);
    void revealPowerup(Cell c);
    void scatterPowerup(int type);
    void updateFlames(int dt);
    void updateEnclosement(int dt);
    void closeCell(Cell c);
    void clearStartArea(Cell c);
    void updateAliens(int dt);
    void updateCampaign(int dt);
    bool alienPassable(AlienType type, Cell c) const;
    void killPlayer(int i, int killer);

    void updatePlayers(int dt, const std::array<PlayerInput, kMaxPlayers>& input);
    void updatePlayer(int i, int dt, const PlayerInput& in);
    Dir chooseDirection(const Player& p, const PlayerInput& in) const;
    int playerSpeed(const Player& p) const;
    bool movePlayer(int i, Dir requested);  // returns true if the player died
    bool checkFlameDeath(int i);
    void checkPickup(int i);
    void pickUp(int playerIndex, int type);
    void removeFromInventory(Player& p, int type);
    void kickBomb(Bomb& b, Dir d);
    void handleButtons(int i, const PlayerInput& in);
    void dropBomb(int i, Cell cell, int delayFrames);

    Values values_;
    Rng rng_;
    int frameMs_ = kFrameMs;
    int tickCount_ = 0;
    int roundMs_ = 0;
    int startFreezeMs_ = 0;
    int contenders_ = 0;
    long long totalMs_ = 0;    // game time across rounds
    long long nextDudMs_ = 0;  // earliest time the next dud may occur
    bool teamPlay_ = false;
    std::array<int, kMaxPlayers> teams_{};
    int roundLimitMs_ = -1;
    int enclosementDepth_ = 0;
    bool winByKills_ = false;
    bool campaign_ = false;
    int campaignResult_ = 0;
    bool campaignRetry_ = false;
    int campaignClearMs_ = 0;
    std::vector<Alien> aliens_;

    // Closing walls ("enclosement"): a cursor walking an inward clockwise spiral.
    struct Walls {
        bool armed = false;
        int timerMs = 0;
        Cell cursor{};
        Dir dir = 1;
        int ring = 0;
    } walls_;
    int wallsClosed_ = 0;
    bool hurryAnnounced_ = false;
    std::vector<Extra> extras_;
    int conveyorSpeed_ = 0;
    void updateExtras();
    void updateSpecial(int i, int dt);
    std::vector<Event> events_;
    void emit(EventKind kind, int player = -1) { events_.push_back({kind, player}); }

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
