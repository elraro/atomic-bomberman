# Atomic Bomberman — reverse engineering and modern reimplementation

A study of the original Atomic Bomberman (Interplay, 1997) and a clean new implementation of its behaviour.

- `AGENTS.md` — project rules and workflow.
- `docs/reverse-engineering/` — what was learned from the original and how (start with `JOURNAL.md`).
- `docs/specifications/` — the game's behaviour, written independently of the original code.
- `docs/testing/original-behaviour.md` — scenarios to check against the original.
- `src/game/` — gameplay core (C++20, no platform dependencies).
- `src/app/`, `src/rendering/`, `src/audio/`, `src/resources/` — SDL3 window and input, OpenGL 3.3 renderer, SDL3 audio, readers for original data files.
- `tools/diagnostics/` — scripts to run and measure the original under Wine.
- `tests/unit/` — tests that encode the specified rules.
- `tools/asset-extractor/` — reader/extractor for the original `.ani` files.
- `game/` — the user's copy of the original game. Read-only, never committed.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler. The front end additionally needs SDL3 and OpenGL development files (`libsdl3-dev`, `libgl-dev`); without them only the core and tests are built.

## Run

```sh
./build/atomic --game-dir game            # values and scheme read from your copy of the original
./build/atomic                            # built-in defaults, empty arena
```

Options: `--scheme NAME` (file in `data/schemes`, default `basic`), `--players N` (1-10), `--seed N`, `--level N` (0-10, graphics theme), `--wins N` (round wins per match, default 2), `--mute`, `--shapes`, `--native`, `--demo` (scripted input), `--frames N --screenshot out.ppm` (automated capture).

Player 1 with the default keys can also punch, grab and throw once the matching powerups are collected: Enter punches the bomb ahead; pressing Space while standing on your own bomb picks it up (grab) or lays a line (spooge); releasing Space throws.

| | Move | Bomb | Action |
|---|---|---|---|
| Player 1 | arrow keys | Space or Right Ctrl | Enter or Right Shift |
| Player 2 | W A S D | Tab or Left Ctrl | Q or Left Shift |

`R` restarts the round, `P` pauses, `N` advances one step while paused, `Esc` quits.

With `--game-dir`, the original backgrounds and sprites are loaded from your copy at run time; without it (or with `--shapes`) placeholder shapes are drawn. `--native` opens a 640×480 window. A decided round restarts after three seconds.

## Status

All knowledge of the original comes from static analysis; nothing has been confirmed against the running game yet. See `docs/reverse-engineering/unknowns.md`.

This repository contains only new code and documentation. The original game is copyrighted by its owners and is not included.
