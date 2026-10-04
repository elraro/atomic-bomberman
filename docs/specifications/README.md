# Behavioural Specifications

These documents describe **what Atomic Bomberman does**, independent of how `bm95.exe` implements it. They are the reference for the modern implementation and its tests.

## Status

Every rule here was derived from static analysis of the original and from the developer-authored data files. **None has yet been observed in the running game** (validation Level 1; see `../reverse-engineering/unknowns.md`, UNKNOWN-001). Rules are tagged:

- **[S]** read directly from the original's code; high confidence.
- **[D]** stated by the original's data files or their developer comments.
- **[M]** medium confidence: partly inferred, or an argument could not be recovered.
- **[?]** open question; do not implement a guess.

Numbers written as `V(n)` are entries of the original `data/res/valuelst.res`; the default is given in brackets. A faithful implementation reads them from that file rather than hard-coding them.

## Documents

| File | Content |
|---|---|
| `gameplay.md` | Time model, tick order, round flow |
| `maps.md` | Grid, tiles, schemes, round generation, closing walls |
| `physics.md` | Positions, movement stepping, collision |
| `players.md` | Player state, input, actions, death |
| `bombs.md` | Bomb lifecycle, fuse, kicked/punched/held bombs |
| `explosions.md` | Blast propagation, flames, interactions |
| `powerups.md` | Types, generation, pickup, diseases |

Not written yet (insufficient knowledge): `game-modes.md` (team play, campaign, match scoring), `networking.md`, AI, level extras (arrows, warps, conveyors, trampolines are only described where they touch the rules above).
