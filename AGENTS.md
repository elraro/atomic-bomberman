# AGENTS.md

# Atomic Bomberman — Reverse Engineering & Modernization

## 1. Mission

You are an autonomous reverse-engineering and software-engineering agent working on a modernization of **Atomic Bomberman**.

The user has provided a complete local copy of the original game, including its executable(s), DLLs, game data, assets, and configuration files.

A **Ghidra project is configured and available through Ghidra MCP**.

Your job is to:

1. Reverse engineer the original game.
2. Understand its architecture and observable behaviour.
3. Document the discoveries systematically.
4. Extract/decode its assets and data formats where practical.
5. Create a behavioural specification independent of the original implementation.
6. Build a clean modern implementation.
7. Use **SDL3 + OpenGL** as the initial modern platform/rendering stack.
8. Validate the modern implementation against the original.

The objective is **not** to mechanically translate the decompiled executable.

The objective is:

> Understand the original game deeply enough to reproduce its behaviour in a clean modern implementation.

---

# 2. Core Philosophy

The original game is **evidence**.

The modern implementation is **a new implementation**.

The desired workflow is:

```text
Original Game
     |
     v
Static Reverse Engineering
     |
     v
Dynamic Analysis
     |
     v
Behavioural Understanding
     |
     v
Documentation
     |
     v
Behavioural Specification
     |
     v
Modern Implementation
     |
     v
Comparison / Validation
     |
     v
Regression Tests
```

Never use this workflow:

```text
Decompiler
   |
   v
Rename FUN_XXXXXXXX
   |
   v
Translate to C++
   |
   v
Hope it works
```

---

# 3. Technology Target

The modern implementation should preferably use:

* C++20 or newer
* SDL3
* OpenGL 3.3+
* CMake
* standard C++ library
* Dear ImGui for optional developer/debug tooling

The architecture must not depend on the original Windows implementation.

The original game may use legacy APIs such as:

* DirectDraw
* DirectSound
* DirectInput
* Win32
* Winsock
* GDI
* legacy multimedia APIs

These should be **understood**, not necessarily reproduced.

---

# 4. Source of Truth

The supplied original game directory is the primary source of truth.

It may contain:

```text
*.exe
*.dll
*.dat
*.bin
*.res
*.pak
*.map
*.cfg
*.ini
*.wav
*.mid
*.mp3
*.avi
*.bmp
*.pcx
*.tga
*.raw
*.spr
```

Do not trust file extensions.

Determine formats from:

* file signatures
* binary structure
* PE resources
* strings
* cross-references
* runtime behaviour
* Ghidra
* debugger observations

---

# 5. Preserve the Original

The original game files must be treated as **read-only evidence**.

Never modify the only copy.

If modification is necessary, create a working copy.

Record hashes of important files.

Create:

```text
docs/original-files.md
```

Example:

```markdown
| File | Size | SHA-256 | Type | Notes |
|------|------|---------|------|-------|
| atomic.exe | ... | ... | PE32 | Main executable |
| foo.dll | ... | ... | PE32 DLL | ... |
| data.dat | ... | ... | Custom | ... |
```

Do not commit copyrighted original binaries or assets unless the project explicitly permits doing so.

---

# 6. Repository Structure

Use approximately:

```text
/
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── LICENSE
│
├── docs/
│   ├── original-files.md
│   │
│   ├── reverse-engineering/
│   │   ├── JOURNAL.md
│   │   ├── initial-analysis.md
│   │   ├── architecture.md
│   │   ├── functions.md
│   │   ├── structures.md
│   │   ├── globals.md
│   │   ├── game-loop.md
│   │   ├── game-state.md
│   │   ├── players.md
│   │   ├── bombs.md
│   │   ├── explosions.md
│   │   ├── maps.md
│   │   ├── rendering.md
│   │   ├── audio.md
│   │   ├── input.md
│   │   ├── networking.md
│   │   ├── resources.md
│   │   ├── file-formats.md
│   │   └── unknowns.md
│   │
│   ├── specifications/
│   │   ├── gameplay.md
│   │   ├── physics.md
│   │   ├── players.md
│   │   ├── bombs.md
│   │   ├── explosions.md
│   │   ├── powerups.md
│   │   ├── maps.md
│   │   ├── game-modes.md
│   │   └── networking.md
│   │
│   ├── testing/
│   │   └── original-behaviour.md
│   │
│   └── migration/
│       ├── architecture.md
│       ├── rendering.md
│       ├── audio.md
│       └── networking.md
│
├── reverse-engineering/
│   ├── ghidra/
│   ├── scripts/
│   └── notes/
│
├── original/
│   └── README.md
│
├── assets/
│   ├── original/
│   └── converted/
│
├── src/
│   ├── app/
│   ├── core/
│   ├── game/
│   ├── entities/
│   ├── map/
│   ├── physics/
│   ├── rendering/
│   ├── audio/
│   ├── input/
│   ├── networking/
│   ├── resources/
│   ├── platform/
│   └── tools/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   └── regression/
│
└── tools/
    ├── asset-inspector/
    ├── asset-extractor/
    ├── asset-converter/
    ├── map-converter/
    ├── replay/
    └── diagnostics/
```

Adjust this structure when useful.

---

# 7. Ghidra + MCP

Ghidra is a primary reverse-engineering tool.

The configured Ghidra MCP should be used extensively.

Use it to investigate:

* functions
* decompilation
* symbols
* strings
* references
* callers
* callees
* data
* structures
* memory
* imports
* exports
* control flow
* constants
* global variables

When useful, correlate Ghidra findings with:

* debugger observations
* runtime behaviour
* file inspection
* API tracing
* controlled experiments

Do not rely exclusively on static analysis.

---

# 8. Ghidra Naming

Do not leave important functions as:

```text
FUN_00412340
sub_4012A0
```

Rename functions when their purpose is sufficiently understood.

Examples:

```text
Game_Init
Game_Shutdown
Game_Update
Game_Render

Player_Init
Player_Update
Player_Move
Player_Die

Bomb_Create
Bomb_Update
Bomb_Explode

Explosion_Create
Explosion_Update

Map_Load
Map_GetTile

Network_Init
Network_Send
Network_Receive

Resource_Load
Resource_Free
```

Do not rename a function merely because its name seems plausible.

If uncertain:

```text
Unknown_PlayerFunction
Maybe_UpdateBomb
Unknown_NetworkHandler
```

is preferable.

---

# 9. Reverse-Engineering Confidence

Use these confidence levels:

```text
CONFIRMED
HIGH
MEDIUM
LOW
HYPOTHESIS
UNKNOWN
```

Every major conclusion should have a confidence level.

Example:

```markdown
## Game_Update

Confidence: HIGH

Evidence:
- Called repeatedly by the main loop.
- Updates player state.
- Updates bombs.
- Updates explosions.
- Advances gameplay timers.

Ghidra:
- Address: 0x00412340
- Caller: MainLoop
```

Never silently turn a hypothesis into a fact.

---

# 10. Reverse-Engineering Journal

Maintain:

```text
docs/reverse-engineering/JOURNAL.md
```

This should be an append-only record of important discoveries.

Example:

```markdown
# Reverse Engineering Journal

## 2026-10-03

### Discovery: Main game loop

Ghidra function:
0x00412340

Evidence:
- Called repeatedly after Windows message processing.
- Calls player update.
- Calls bomb update.
- Calls explosion update.
- Calls rendering.

Confidence:
HIGH

Next:
Determine timing mechanism and simulation frequency.
```

The journal is important because knowledge must survive changes in task, agent context, and implementation.

---

# 11. Unknowns Register

Maintain:

```text
docs/reverse-engineering/unknowns.md
```

Example:

```markdown
# UNKNOWN-007

Question:
What determines bomb fuse duration?

Evidence:
Bomb appears to explode after approximately 2 seconds.

Possible function:
0x0048XXXX

Status:
OPEN

Next action:
Instrument timer function and inspect values during gameplay.
```

Every significant unknown should have a possible next investigation step.

---

# 12. Initial Reconnaissance

Before writing substantial code, perform an inventory of the supplied game.

Determine:

* all executables
* all DLLs
* architecture
* PE headers
* compiler/toolchain where possible
* imports
* exports
* resources
* strings
* dependencies
* data files
* likely archives
* likely map files
* likely sprite files
* audio files
* configuration
* save data

Create:

```text
docs/reverse-engineering/initial-analysis.md
```

---

# 13. Identify the Main Executable

Do not assume the largest EXE is the main game executable.

Trace:

```text
PE entry point
      |
      v
CRT initialization
      |
      v
Application initialization
      |
      v
Window creation
      |
      v
Subsystem initialization
      |
      v
Main loop
```

Identify:

* entry point
* startup function
* initialization
* shutdown
* main loop

---

# 14. Find the Main Loop

The game loop is a top-priority discovery.

Determine:

* Windows message processing
* input processing
* simulation
* rendering
* audio
* networking
* timing
* sleep/yield behaviour

Document in:

```text
docs/reverse-engineering/game-loop.md
```

Determine whether the game uses:

```text
fixed timestep
variable timestep
frame-dependent simulation
timer callbacks
```

Do not assume.

---

# 15. Timing Analysis

Determine:

* simulation frequency
* frame timing
* animation timing
* bomb fuse duration
* explosion duration
* player animation timing
* round timer
* lobby timer
* network update frequency

Investigate APIs such as:

```text
QueryPerformanceCounter
GetTickCount
timeGetTime
Sleep
WaitForSingleObject
multimedia timers
```

or other timing mechanisms actually found in the executable.

Record the evidence.

---

# 16. Main Game State

Identify the logical game state.

Look for:

* player count
* current map
* game mode
* round state
* timers
* scores
* winner
* match state
* lobby state

Document in:

```text
docs/reverse-engineering/game-state.md
```

Do not copy the original memory layout blindly.

Translate it into logical modern structures.

For example:

```cpp
struct GameState
{
    MatchState state;
    std::vector<Player> players;
    Map map;
    uint32_t round;
};
```

---

# 17. Global State

Identify important globals.

Document:

| Address    | Type    | Purpose      | Confidence |
| ---------- | ------- | ------------ | ---------- |
| 0xXXXXXXXX | uint32  | Player count | HIGH       |
| 0xXXXXXXXX | pointer | Current map  | MEDIUM     |
| 0xXXXXXXXX | struct  | Game state   | LOW        |

Do not automatically reproduce globals in the modern engine.

Determine logical ownership.

---

# 18. Player Reverse Engineering

Determine:

```text
position
velocity
direction
animation
state
alive/dead
bomb capacity
active bombs
blast radius
powerups
score
team
spawn position
input
```

Investigate:

* movement speed
* collision dimensions
* acceleration/deceleration
* tile transitions
* bomb collision
* explosion collision
* death behaviour
* spawn behaviour
* animation state machine

Document in:

```text
docs/reverse-engineering/players.md
```

---

# 19. Map Reverse Engineering

Determine:

* map dimensions
* grid dimensions
* tile dimensions
* coordinate system
* tile types
* indestructible blocks
* destructible blocks
* empty tiles
* spawn points
* hidden powerups
* decorations
* map metadata

Identify the actual map file format.

Document in:

```text
docs/reverse-engineering/maps.md
```

and:

```text
docs/reverse-engineering/file-formats.md
```

---

# 20. Bomb System

Determine:

* bomb creation
* placement
* ownership
* capacity
* fuse
* collision
* rendering
* animation
* chain reactions
* simultaneous explosions

Test edge cases:

```text
Bomb next to wall
Bomb next to destructible block
Bomb next to another bomb
Bomb placed while moving
Player trapped by bomb
Multiple bombs detonating simultaneously
Chain reaction timing
```

Document in:

```text
docs/reverse-engineering/bombs.md
```

---

# 21. Explosion System

Determine exactly how explosions propagate.

Investigate:

* origin
* radius
* propagation
* walls
* destructible blocks
* indestructible blocks
* bombs
* players
* powerups
* simultaneous explosions
* animation
* lifetime

Create a behaviour matrix.

Example:

| Object            | Explosion interaction |
| ----------------- | --------------------- |
| Solid wall        | Stops                 |
| Destructible wall | Destroys / stops      |
| Bomb              | Triggers              |
| Player            | Kills                 |
| Powerup           | ...                   |

Do not fill values without evidence.

---

# 22. Powerups

Identify all powerups.

For every powerup determine:

* identifier
* visual representation
* spawn condition
* effect
* maximum value
* stacking behaviour
* persistence
* death behaviour
* round behaviour

Document in:

```text
docs/reverse-engineering/powerups.md
```

---

# 23. Rendering Reverse Engineering

Determine the original rendering system.

Investigate:

* graphics API
* backbuffer
* resolution
* viewport
* coordinate system
* sprites
* sprite sheets
* textures
* transparency
* palette
* scaling
* filtering
* animation
* z-order
* UI
* fonts
* transitions
* particles/effects

If the original uses DirectDraw or another legacy API, understand its logical behaviour but do not reproduce the legacy API in the modern engine unless required for compatibility.

---

# 24. Asset Reverse Engineering

Identify all resource formats.

For each format document:

```text
Extension:
Magic:
Header:
Version:
Count:
Offsets:
Sizes:
Compression:
Encoding:
Dimensions:
Palette:
Payload:
```

Do not trust the extension.

Build tools where useful.

Examples:

```text
tools/asset-inspector/
tools/asset-extractor/
tools/asset-converter/
tools/map-converter/
```

Prefer automated extraction over manual processing.

---

# 25. Asset Preservation

Keep original and converted data separate:

```text
assets/
├── original/
└── converted/
```

Do not destroy original files.

If an original asset can be decoded directly, prefer a loader or conversion tool over manually recreating the asset.

---

# 26. Resource System

Reverse engineer:

* resource IDs
* resource loading
* caching
* unloading
* memory ownership
* archives
* lazy loading
* reference counting

Determine relationships such as:

```text
Map
 ├── Tileset
 ├── Sprites
 ├── Sounds
 └── Music
```

Document in:

```text
docs/reverse-engineering/resources.md
```

---

# 27. Audio

Determine:

* audio API
* sound effects
* music
* formats
* channels
* looping
* volume
* transitions
* spatialization if any

Map gameplay events to sounds where possible.

---

# 28. Input

Determine:

* keyboard
* joystick
* controller
* DirectInput
* Windows messages
* polling
* events
* mappings

The modern game should expose a platform-independent `InputState`.

Gameplay must not directly depend on SDL input calls.

---

# 29. Networking

Treat networking as a major independent subsystem.

Determine:

* transport
* client/server architecture
* peer-to-peer architecture if applicable
* ports
* lobby discovery
* connection establishment
* packet structure
* packet types
* byte ordering
* sequence numbers
* reliability
* synchronization
* player join
* player leave
* game start
* game end
* timeout
* disconnect handling
* chat if present

Document in:

```text
docs/reverse-engineering/networking.md
```

Do not implement guessed packet structures.

---

# 30. Dynamic Analysis

Static analysis is not enough.

Run the original game and observe:

```text
Startup
Main menu
Options
Player configuration
Lobby
Match start
Player movement
Bomb placement
Explosion
Death
Powerup
Round end
Match end
Network connection
Player join
Player leave
Game exit
```

Use a debugger where useful.

Correlate runtime behaviour with Ghidra functions.

---

# 31. Behavioural Test Matrix

Create:

```text
docs/testing/original-behaviour.md
```

Each important mechanic should have reproducible scenarios.

Example:

```markdown
## Player Movement

Initial:
Player at tile (5,5).

Input:
RIGHT for 1 second.

Expected:
Player moves according to observed original speed.

Collision:
Player stops against solid wall.

Confidence:
HIGH
```

The test matrix becomes the reference for the modern implementation.

---

# 32. Behavioural Specification

Before substantial modernization, create:

```text
docs/specifications/
```

The specification should describe **what the game does**, not how the original executable implements it.

At minimum:

```text
gameplay.md
physics.md
players.md
bombs.md
explosions.md
powerups.md
maps.md
game-modes.md
networking.md
```

---

# 33. Do Not Start the Port Too Early

Do not immediately create a large SDL/OpenGL implementation.

First understand enough of:

* game loop
* timing
* state
* player
* map
* bombs
* explosions
* collision
* resources

to create a meaningful behavioural specification.

The first major milestone is:

> The game can be described independently of the original executable's implementation.

---

# 34. Modern Architecture

The modern implementation should approximately use:

```text
Application
    |
    +-- Game
    |    |
    |    +-- GameState
    |    +-- Match
    |    +-- Player
    |    +-- Bomb
    |    +-- Explosion
    |    +-- PowerUp
    |    +-- Map
    |
    +-- Input
    |
    +-- Rendering
    |
    +-- Audio
    |
    +-- Networking
    |
    +-- Resources
    |
    +-- Platform
```

Gameplay must remain independent of SDL/OpenGL.

---

# 35. Modern Source Structure

Use approximately:

```text
src/
├── app/
├── core/
├── game/
├── entities/
├── map/
├── physics/
├── rendering/
├── audio/
├── input/
├── networking/
├── resources/
├── platform/
└── tools/
```

Do not over-engineer the architecture.

---

# 36. SDL3

Use SDL3 for:

* window creation
* platform events
* keyboard
* game controllers
* timing
* audio where appropriate
* platform integration

SDL-specific implementation belongs primarily under:

```text
src/platform/
src/input/
src/audio/
```

Gameplay code must not directly call SDL APIs.

---

# 37. OpenGL

Use OpenGL 3.3+ initially.

Use modern OpenGL:

```text
VAO
VBO
EBO
Shaders
Textures
Texture atlases
Orthographic projection
Framebuffers where useful
```

Do not use deprecated fixed-function rendering.

The renderer should own OpenGL resources.

---

# 38. Renderer Architecture

Keep rendering separate from gameplay.

Bad:

```cpp
void Player::update()
{
    SDL_GetKeyboardState(...);
    glDrawArrays(...);
}
```

Good:

```cpp
void Player::update(
    const InputState& input,
    FixedDeltaTime dt);
```

Then:

```cpp
renderer.draw(player);
```

---

# 39. Game Loop

Prefer a deterministic fixed simulation timestep unless reverse engineering proves that the original requires another model.

Conceptually:

```cpp
while (running)
{
    processEvents();

    accumulator += frameDelta;

    while (accumulator >= fixedDelta)
    {
        updateInput();
        updateGame(fixedDelta);
        updateNetworking(fixedDelta);

        accumulator -= fixedDelta;
    }

    render(interpolation);
}
```

The actual timestep must be derived from the original where possible.

---

# 40. Determinism

Make gameplay deterministic where practical.

This is especially useful for:

* networking
* replays
* debugging
* regression tests

Do not let rendering frame rate determine gameplay.

---

# 41. Modern Data Structures

Prefer meaningful domain types.

Example:

```cpp
struct TilePosition
{
    int x;
    int y;
};

struct WorldPosition
{
    float x;
    float y;
};

struct Player
{
    PlayerId id;

    WorldPosition position;
    Direction direction;

    int bombCapacity;
    int activeBombs;
    int blastRadius;

    PlayerState state;
};
```

Do not reproduce arbitrary original memory offsets.

---

# 42. Legacy Compatibility Layer

Where practical, isolate original data formats.

Example:

```text
Original Map
     |
     v
LegacyMapReader
     |
     v
Modern Map
```

and:

```text
Original Sprite
     |
     v
LegacySpriteLoader
     |
     v
Modern Texture
```

Legacy compatibility code should not leak into gameplay.

---

# 43. Tests

Create tests for:

```text
Map loading
Player movement
Player collision
Bomb placement
Bomb fuse
Bomb chain reaction
Explosion propagation
Solid walls
Destructible walls
Powerups
Player death
Round completion
Scoring
```

Every recovered gameplay rule should eventually have a test where practical.

---

# 44. Golden Behaviour Tests

Create deterministic scenarios based on observations of the original.

Example:

```text
Map: Arena01

Initial:
Player 1 = (1,1)
Bomb capacity = 1
Blast radius = 2

Action:
Place bomb

Expected:
Bomb exists at (1,1)

After fuse:
Explosion origin = (1,1)

Expected:
Explosion reaches observed tiles.
```

Do not invent expected values.

Record them from the original.

---

# 45. Original vs Modern Comparison

Where practical, compare the original and modern game using:

* screenshots
* video
* deterministic scenarios
* event logs
* state dumps
* timing measurements

Compare:

```text
player position
bomb timing
explosion timing
animation
map state
powerups
scores
round state
network state
```

Pixel-perfect rendering is less important initially than behavioural correctness.

---

# 46. Debug Mode

Provide a developer/debug mode.

Useful overlays:

```text
FPS
Simulation tick
Player IDs
Entity IDs
Tile coordinates
Collision boxes
Bomb timers
Explosion radius
Network statistics
Resource IDs
```

Useful controls:

```text
Pause
Single-step simulation
Toggle collision
Toggle grid
Toggle hitboxes
Reload map
Reload resources
```

Do not interfere with normal gameplay controls.

---

# 47. Logging

Use structured logging.

At minimum:

```text
TRACE
DEBUG
INFO
WARN
ERROR
```

Example:

```text
INFO  Match started map=arena01 players=4
DEBUG Bomb placed player=2 tile=(5,7)
DEBUG Bomb exploded tile=(5,7) radius=3
DEBUG Explosion destroyed tile=(5,9)
INFO  Player eliminated id=3
```

Do not log every frame at normal log levels.

---

# 48. Git Strategy

Make small logical commits.

Examples:

```text
reverse: identify executable entry point
reverse: identify main loop
reverse: identify player structure
reverse: document map format
reverse: identify bomb update routine

tools: add resource inspector
tools: add sprite extractor

engine: initialize SDL3
engine: initialize OpenGL
engine: add fixed timestep

game: implement map
game: implement player movement
game: implement bombs
game: implement explosions
```

Avoid huge commits.

---

# 49. Build Discipline

After every significant implementation change:

1. Build.
2. Run tests.
3. Fix warnings.
4. Run relevant regression tests.
5. Update documentation.

Do not accumulate large amounts of untested code.

---

# 50. No Premature Optimization

Do not optimize before behaviour is correct.

Avoid introducing without evidence:

* ECS
* job systems
* custom allocators
* multithreading
* GPU compute
* complex dependency injection
* scripting engines

Prefer straightforward modern C++.

Optimize only after profiling.

---

# 51. Reverse Engineering Workflow

For each subsystem:

```text
1. Locate
2. Identify
3. Trace
4. Observe
5. Hypothesize
6. Test
7. Confirm
8. Document
9. Specify
10. Implement
11. Test again
```

Never skip directly from:

```text
Locate
```

to:

```text
Implement
```

---

# 52. Evidence Requirements

Important discoveries should record:

```text
What was discovered
Where it was discovered
How it was discovered
Evidence
Confidence
Implications
```

Example:

```markdown
## Bomb Timer

Address:
0x004A1234

Observation:
Value initialized to 120.

Dynamic observation:
Bomb explodes after approximately 120 simulation ticks.

Conclusion:
Likely 120-tick fuse.

Confidence:
HIGH

Modern implication:
Bomb fuse should use simulation ticks rather than render frames.
```

---

# 53. Validation Levels

Classify recovered features:

### Level 0 — Unknown

No meaningful understanding.

### Level 1 — Static

Evidence from Ghidra/disassembly.

### Level 2 — Dynamic

Behaviour observed during execution.

### Level 3 — Reproduced

Modern implementation reproduces observed behaviour.

### Level 4 — Regression Tested

Automated tests verify the behaviour.

Important systems should eventually reach Level 4.

---

# 54. Definition of Done

A subsystem is complete only when:

1. Original behaviour has been investigated.
2. Relevant Ghidra evidence has been documented.
3. Important functions/data structures have been identified.
4. A behavioural specification exists.
5. Modern implementation exists.
6. It builds successfully.
7. Tests exist.
8. Original and modern behaviour have been compared.
9. Known differences are documented.

---

# 55. Migration Milestones

## Milestone 1 — Reconnaissance

Deliver:

```text
Executable inventory
DLL inventory
File inventory
PE analysis
Import analysis
String analysis
Initial Ghidra map
```

---

## Milestone 2 — Core Reverse Engineering

Identify:

```text
Entry point
Main loop
Timing
Game state
Player state
Map state
Bomb state
Explosion state
```

---

## Milestone 3 — Behavioural Specification

Produce:

```text
Gameplay specification
Physics specification
Player specification
Bomb specification
Explosion specification
Powerup specification
Map specification
Timing specification
```

---

## Milestone 4 — SDL/OpenGL Prototype

Implement:

```text
SDL3
OpenGL
Window
Renderer
Texture loading
Sprite rendering
Map rendering
```

---

## Milestone 5 — Playable Core

Implement:

```text
Map
Player
Movement
Collision
Bombs
Explosions
Powerups
Round state
```

A local playable match should now exist.

---

## Milestone 6 — Original Assets

Load converted/original assets.

---

## Milestone 7 — UI and Audio

Implement:

```text
Main menu
Options
Player configuration
Match setup
Sound effects
Music
```

---

## Milestone 8 — Multiplayer

Implement networking after sufficiently understanding the original protocol.

---

## Milestone 9 — Regression

Create automated comparison/regression tests.

---

## Milestone 10 — Polish

Only after behaviour is correct:

```text
Performance
Rendering quality
Cross-platform support
Controller support
Debug tooling
Packaging
```

---

# 56. First Session Instructions

When the agent first starts this project, it MUST NOT immediately start writing the SDL/OpenGL game.

Perform the following sequence.

## Step 1 — Inventory

Inspect the entire supplied game directory.

Identify all files.

---

## Step 2 — Hash

Calculate hashes for important original files.

---

## Step 3 — Executables

Identify:

* EXEs
* DLLs
* architecture
* dependencies

---

## Step 4 — Main Executable

Determine which executable contains the game.

---

## Step 5 — PE Analysis

Inspect:

* sections
* imports
* exports
* resources
* entry point
* compiler indicators

---

## Step 6 — Strings

Extract and categorize strings.

Look specifically for:

```text
file names
map names
resource names
Windows APIs
network messages
error messages
debug strings
menu text
player text
configuration keys
```

---

## Step 7 — Ghidra

Inspect the configured Ghidra project.

Identify:

```text
entry point
startup
initialization
main loop candidates
rendering
input
audio
networking
resource loading
```

---

## Step 8 — Main Loop

Trace the actual main loop.

Determine timing.

---

## Step 9 — Architecture

Create:

```text
docs/reverse-engineering/architecture.md
```

with the first subsystem map.

---

## Step 10 — Journal

Create:

```text
docs/reverse-engineering/JOURNAL.md
```

and record all important discoveries.

---

## Step 11 — Unknowns

Create:

```text
docs/reverse-engineering/unknowns.md
```

and record unresolved questions.

---

## Step 12 — Report

Produce an initial analysis report containing:

```text
Original version
Executable architecture
Compiler/runtime if known
Main executable
Important DLLs
Graphics API
Audio API
Input API
Networking API
Resource system
Main loop
Initial game architecture
Known subsystems
Known unknowns
Recommended next investigation
```

---

# 57. What NOT To Do

Never:

* blindly translate decompiler output
* assume variable types without evidence
* infer behaviour from function names alone
* modify the original executable unnecessarily
* overwrite original files
* delete unknown code because it appears unused
* invent gameplay mechanics
* invent network protocol structures
* invent file formats
* claim uncertain discoveries are confirmed
* write a huge modern engine before understanding the game
* optimize prematurely
* create unnecessary abstractions
* allow SDL/OpenGL dependencies into gameplay logic

---

# 58. When Something Is Unknown

Do this:

```text
UNKNOWN
   |
   +-- Ghidra
   |
   +-- Cross references
   |
   +-- Strings
   |
   +-- Debugger
   |
   +-- Runtime observation
   |
   +-- Controlled experiment
   |
   v
Conclusion
   |
   v
Document
```

Do not guess when investigation is possible.

---

# 59. Legal and Repository Hygiene

The original game may be copyrighted.

Treat supplied original binaries and assets as user-provided material.

Do not:

* download copyrighted copies merely to replace missing files
* redistribute original game binaries
* upload original game assets to public repositories
* remove copyright notices
* claim ownership of original assets

The modernization code should clearly distinguish:

```text
Original material
```

from:

```text
New code
```

Document provenance where relevant.

---

# 60. Final Architecture Goal

The final system should conceptually look like:

```text
                 Atomic Bomberman
                 Original Behaviour
                         |
                         v
              +---------------------+
              | Reverse Engineering |
              +---------------------+
                         |
                         v
              +---------------------+
              | Behavioural Spec    |
              +---------------------+
                         |
                         v
              +---------------------+
              | Modern Game Core    |
              +---------------------+
                  /       |       \
                 /        |        \
                v         v         v
          Rendering     Audio    Networking
              |           |          |
              v           v          v
          OpenGL        SDL3     Modern API
              |
              v
            SDL3
```

The original executable should **not** be required at runtime for the modern game.

---

# 61. Priority Order

When deciding what to investigate next, generally prioritize:

```text
1. Main loop
2. Timing
3. Game state
4. Player state
5. Map representation
6. Bombs
7. Explosions
8. Collision
9. Resource system
10. Rendering
11. Input
12. Audio
13. Networking
14. Menus/UI
15. Polish
```

Change this order when evidence suggests another subsystem is blocking progress.

---

# 62. Ultimate Goal

The finished project should be a **clean modern implementation of Atomic Bomberman's behaviour**, not a decompiled source-code dump.

The desired outcome is:

```text
             Original Atomic Bomberman
                       |
                       v
              Reverse Engineering
                       |
                       v
              Behavioural Knowledge
                       |
                       v
                Clean Game Core
                       |
          +------------+------------+
          |            |            |
          v            v            v
       SDL3         OpenGL      Networking
          |            |
          +------------+
                |
                v
          Modern Atomic
           Bomberman
```

The priority order is:

```text
1. Preserve evidence
2. Understand the original
3. Verify observations
4. Document discoveries
5. Define behaviour
6. Implement cleanly
7. Validate against original
8. Add regression tests
9. Improve architecture
10. Optimize
```

**Do not sacrifice reverse-engineering accuracy for implementation speed.**

