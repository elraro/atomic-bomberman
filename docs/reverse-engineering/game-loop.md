# Game Loop

Static analysis (Level 1). Addresses are VAs in `bm95.exe`.

## Structure

Confidence: HIGH.

There is no dedicated main loop. `Match_Run` (`0x42A3F6`) registers `Game_Tick` (`0x42A191`) as a GNW background process and then loops on `GNW_GetInput` (`0x43A508`). Every call to `GNW_GetInput`:

1. pumps the Windows message queue (`GNW_PumpMessages 0x43B5EC`),
2. runs all background processes (`GNW_RunBkProcesses 0x43A6A4`), which runs one `Game_Tick`,
3. returns a key code for the outer loop (ESC, pause, F-keys).

`Game_SetTickPaused` (`0x42A16F`) sets `0x4646AC`; while set, `Game_Tick` returns immediately. It is used around menus, help, and round transitions.

## Timing

Confidence: HIGH.

- Clock: `timeGetTime` only.
- `frame_ms = now − previous`, clamped to `Value_Get(31)` = 150, stored at `0x464958`.
- `ms_per_frame` at `0x46494C` = `1000 / Value_Get(30)` = 50.
- The step is **variable**. No sleep or frame limiter was found in the tick path (the present call has not been read: UNKNOWN-013).

Two idioms convert `frame_ms` into game progress. Both appear in players, bombs and flames:

```text
Timers / animation:
    acc_ms += frame_ms
    while acc_ms > 0:            # note: > 0, so the first tick always advances one frame
        frame += 1
        acc_ms -= 50

Movement:
    acc += speed * frame_ms / 50          # integer division; speed in 1/100 px per 50 ms
    while acc > 0:
        move exactly one pixel (with collision logic)
        acc -= 100
```

Consequences:

- Positions are **integer pixels**. All movement is a sequence of single-pixel steps, each with its own collision check. Frame rate changes how many steps run per tick, not the rules of a step.
- Durations are compared in milliseconds: a value of N frames in `valuelst.res` means N × 50 ms (bomb fuse: `fuse_ms = frames × 50`, set in `Bomb_Create`).
- `speed * frame_ms / 50` truncates each tick, so the distance covered depends slightly on frame rate. Example at speed 923: one 50 ms tick adds 923; two 25 ms ticks add 461 + 461 = 922.
- The accumulator loops use `> 0`, not `>= step`: an object moves one pixel as soon as any positive amount has accumulated, and the accumulator then goes negative.

Implication for the modern port (not yet a decision): the simulation can be made deterministic by running a fixed step and feeding that step's milliseconds into the same accumulator rules. Which fixed step best matches how the original was played (20 Hz nominal vs. real machine frame rates) needs runtime measurement (UNKNOWN-001, UNKNOWN-013).

## Order of one tick

Confidence: HIGH for the order (read from the call sequence), HIGH for roles marked ✔ (function body read), MEDIUM otherwise.

| # | Function | Role |
|---|---|---|
| 1 | `Time_GetMs`, clamp | compute `frame_ms` |
| 2 | `0x40E765` | network receive |
| 3 | `0x4105D2` | draw round timer |
| 4 | `0x415CA4` | restore background into the frame buffer |
| 5 | `0x42641F` | map draw / dirty tiles |
| 6 | `Extras_Update 0x4056CA` | arrows, warps, conveyors, trampolines |
| 7 | `Bombs_UpdateFree 0x4245B9` ✔ | all bombs not held by a player: movement, fuse, detonation |
| 8 | `Powerups_Update 0x424F89` | |
| 9 | `0x41B961` | sound service |
| 10 | `Flames_Update 0x426D06` ✔ | age and draw flames |
| 11 | `Map_UpdateEnclosement 0x426818` ✔ | end-of-round closing walls |
| 12 | `Players_Update 0x420F07` ✔ | input, movement, death, pickup, bomb drop, draw |
| 13 | `0x4016DA` | campaign enemies (campaign mode only) |
| 14 | `Bombs_UpdateHeld 0x42459A` ✔ | bombs currently carried (they follow the player's new position) |
| 15 | hurry banner | when time left is within 5 s below `Value_Get(101)` |
| 16 | `0x429F1A`, `0x415ED1`, `0x415C1F` | overlay text, sprite flush, present |
| 17 | `Net_Pump 0x40EA1E` | network send |

Update and drawing are interleaved: each subsystem queues its sprites while it updates.

Ordering facts that affect behaviour:

- Bombs explode (step 7) **before** players move (step 12). A flame created this tick kills a player standing in that cell during the same tick.
- Flames are aged (step 10) before players are checked, so a flame that expires this tick no longer kills.
- Chain detonations are queued and resolved at the start of the **next** tick's bomb update (see `bombs.md`).
