# Atomic Bomberman — reverse engineering and modern reimplementation

A study of the original Atomic Bomberman (Interplay, 1997) and a clean new implementation of its behaviour.

- `AGENTS.md` — project rules and workflow.
- `docs/reverse-engineering/` — what was learned from the original and how (start with `JOURNAL.md`).
- `docs/specifications/` — the game's behaviour, written independently of the original code.
- `docs/testing/original-behaviour.md` — scenarios to check against the original.
- `src/game/` — gameplay core (C++20, no platform dependencies).
- `tests/unit/` — tests that encode the specified rules.
- `tools/asset-extractor/` — reader/extractor for the original `.ani` files.
- `game/` — the user's copy of the original game. Read-only, never committed.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler. Nothing else yet: SDL3 and OpenGL are only needed for the front end, which does not exist yet.

## Status

All knowledge of the original comes from static analysis; nothing has been confirmed against the running game yet. See `docs/reverse-engineering/unknowns.md`.

This repository contains only new code and documentation. The original game is copyrighted by its owners and is not included.
