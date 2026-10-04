# Modern Architecture

Status: the gameplay core exists, with a first SDL3 + OpenGL 3.3 front end that draws it with placeholder shapes.

## Layout

```text
src/game/geometry.hpp   grid constants, directions, pixel <-> cell conversions
src/game/values.*       value table (ids as in the original valuelst.res), defaults + text parser
src/game/world.*        World: tiles, players, bombs, flames, powerups; tick()
src/resources/scheme_file.*   reader for original .sch files -> ab::Scheme
src/rendering/renderer.*      OpenGL 3.3: one shader, one VAO/VBO, batched quads, 640x480 logical screen letterboxed
src/app/main.cpp              SDL3 window, GL context, keyboard, fixed-step loop
tests/unit/test_core.cpp
```

Front-end loop: real time is accumulated and consumed in fixed 50 ms simulation steps (the original's nominal frame; see `docs/reverse-engineering/dynamic-analysis.md`). The renderer interpolates player and bomb positions between the previous and the current step, so display rate and simulation rate are independent. Gameplay code never includes SDL or OpenGL headers.

GL functions are taken from the system `GL/gl.h` + `GL/glext.h` prototypes (Mesa). A loader (glad or similar) will be needed for Windows and macOS.

`ab_game` is a static library with no dependency beyond the C++ standard library. The planned SDL3/OpenGL front end will sit on top of it and feed `World::tick` with `PlayerInput`.

## Decisions

| Decision | Reason |
|---|---|
| Integer pixel positions on a 15 × 11 grid of 40 × 36 cells | This is the original's own model; speeds in the data are pixels per frame. Rendering can scale freely |
| `World::tick(dtMs, inputs)` with the original's accumulator rules | Reproduces the original for any tick length, so both a fixed step and a replay of original timing are possible. The front end will choose a fixed step; which one is open (UNKNOWN-020) |
| All tuning read through `Values` by the original ids | Keeps the modern game data-compatible with `valuelst.res` and with scheme overrides |
| One flame and one powerup slot per cell; bombs in a fixed pool of 100 | Matches the original's observable limits (e.g. the detonation queue) without copying its memory layout |
| Injected seedable `Rng` | Determinism for tests and replays. The original's exact random sequence is not reproduced |
| Plain structs, no ECS, no inheritance | AGENTS.md §50 |

## What the core covers

Round generation (brick density, hidden powerups, start-area clearing), start freeze, player movement and collision, direction priority, bomb dropping, fuses, blast propagation, chain reactions, flames and death, the contender window, kicking, kick-stop, jelly bounce, trigger bombs, goldflame, powerup reveal (with early-round protection), pickup, caps and exclusive pairs, the round clock, the hurry warning, closing walls, the round result (winner or draw), punched, thrown and carried bombs, spooge lines, and the head-hit stun.

## Not covered yet

Duds, diseases, level extras, match flow across rounds, team play, campaign, AI, networking. What happens when the round clock reaches zero is not known yet (the modern clock simply stops at 0). Each is specified to some degree in `docs/specifications/` except game modes, AI and networking.

## Validation level

The tests check the core against the **specification**, which itself is a prediction from static analysis. Per AGENTS.md §53 these mechanics are at Level 1 for the original and have a tested modern implementation; they reach Level 3/4 only once the same scenarios have been observed on the original.
