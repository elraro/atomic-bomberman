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
./build/atomic --game-dir game     # main menu, original graphics and sound from your copy
./build/atomic                     # no game files: placeholder shapes, straight into a match
```

With game files the program opens on the main menu. Choose **Start Game**, set up the player list (Up/Down select a slot, Right cycles AI → KEY 0 → KEY 1 → OFF, Left switches a slot off), press Enter to play. By default player 1 uses the first key set and player 2 is a computer player, as in the original. The other menu items are not implemented.

| | Move | Bomb | Action |
|---|---|---|---|
| KEY 0 | arrow keys | Space or Right Ctrl | Enter or Right Shift |
| KEY 1 | W A S D | Tab or Left Ctrl | Q or Left Shift |

The action button punches the bomb ahead (punch), stops your kicked bombs (kicker) and detonates trigger bombs. Pressing the bomb button while standing on your own bomb picks it up (grab; release to throw) or lays a line (spooge).

In a match: `Esc` returns to the menu, `R` restarts the round, `P` pauses, `N` advances one step while paused. A decided round restarts after three seconds; the first player to `--wins` rounds takes the match.

Options: `--scheme NAME` (file in `data/schemes`, default `basic`), `--level N` (0-10: graphics, music and extras), `--wins N` (default 2), `--players N --humans H` (preset the player list; `--humans 0` or `--demo` is all computer players), `--start` (skip the menu), `--seed N`, `--mute`, `--shapes`, `--native` (640×480 window), `--frames N --screenshot out.ppm` (automated capture).

## Status

All knowledge of the original comes from static analysis; nothing has been confirmed against the running game yet. See `docs/reverse-engineering/unknowns.md`.

This repository contains only new code and documentation. The original game is copyrighted by its owners and is not included.
