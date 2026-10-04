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

## Install

See [INSTALL.md](INSTALL.md): download a build (or build from source), import the data from your own copy of the original once with `atomic --import-assets PATH`, then start the program.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler. The front end needs SDL3: either installed on the system (`libsdl3-dev`), or downloaded and linked in with `-DAB_FETCH_SDL3=ON`. Without SDL3 only the core and the tests are built. `.github/workflows/build.yml` builds Linux and Windows packages.

## Run

```sh
./build/atomic
```

The program needs the original game's data for the menu, graphics and sound. It looks in: `--game-dir PATH`, the `ATOMIC_GAME_DIR` environment variable, an imported `assets` folder (next to the executable, in the current folder, or in the per-user data folder), or a `game` folder with a copy of the original. The terminal prints `INFO  Game files: …` when it is found; if it is not, the window title says so and the program falls back to placeholder shapes with no menu.

With game files the program opens on the main menu. Choose **Start Game**, set up the player list (Up/Down select a slot, Right cycles AI → KEY 0 → KEY 1 → OFF, Left switches a slot off), press Enter, then choose level, scheme and wins (Left/Right) and press Enter to play. By default player 1 uses the first key set and player 2 is a computer player, as in the original. The other menu items are not implemented.

| | Move | Bomb | Action |
|---|---|---|---|
| KEY 0 | arrow keys | Space or Right Ctrl | Enter or Right Shift |
| KEY 1 | W A S D | Tab or Left Ctrl | Q or Left Shift |

Connected gamepads appear in the player list as `JOY n` (left stick or d-pad to move, A / cross to bomb, B or X / circle or square for the action). Gamepad support is untested: no controller was available.

The action button punches the bomb ahead (punch), stops your kicked bombs (kicker) and detonates trigger bombs. Pressing the bomb button while standing on your own bomb picks it up (grab; release to throw) or lays a line (spooge).

In a match: `Esc` returns to the menu, `R` restarts the round, `P` pauses, `N` advances one step while paused. A decided round restarts after three seconds; the first player to `--wins` rounds takes the match.

Options: `--scheme NAME` (file in `data/schemes`, default `basic`), `--level N` (0-10: graphics, music and extras), `--wins N` (default 2), `--players N --humans H` (preset the player list; `--humans 0` or `--demo` is all computer players), `--start` (skip the menu), `--seed N`, `--mute`, `--shapes`, `--native` (640×480 window), `--frames N --screenshot out.ppm` (automated capture), `--result-shot` (capture the first result screen), `--script up,down,left,right,enter,esc` (scripted menu keys, one every 10 frames).

## Status

All knowledge of the original comes from static analysis; nothing has been confirmed against the running game yet. See `docs/reverse-engineering/unknowns.md`.

This repository contains only new code and documentation. The original game is copyrighted by its owners and is not included.
