# Gameplay: time model and round flow

See `README.md` for the meaning of [S], [D], [M], [?] and `V(n)`.

## Units

- **Frame**: the authoring unit of time, 1000 / V(30) [20] = **50 ms**. [S]
- **Speed**: hundredths of a pixel per frame. [D]
- **Tick**: one simulation step. The original runs one tick per rendered frame with a variable duration `dt` in milliseconds, at most V(31) [150]. [S]

## Advancing time

All game objects use two rules. [S]

**Frame counters** (animation, fuses, lifetimes):

```text
acc += dt
while acc > 0:
    counter += 1
    acc -= 50
```

Durations compared in milliseconds use `elapsed += dt` and are tested against `frames × 50`.

**Movement**:

```text
acc += speed * dt / 50          (integer division, truncating)
while acc > 0:
    perform one 1-pixel movement step
    acc -= 100
```

The accumulators persist between ticks and may be negative after a step.

A small set of behaviours counts **ticks**, not time: the stun after a bomb lands on a player (16 ticks), the disease pass cooldown (V(129) [10] ticks), and chain-reaction delay (one tick per link). [S] The original has no frame limiter and no vertical-sync wait, so its tick rate was whatever the machine reached; the typical rate on period hardware is unknown. [S][?]

## Tick order

Within one tick: [S]

1. Level extras (arrows, warps, conveyors, trampolines).
2. Bombs not held by a player: movement, fuse, detonation.
3. Powerups.
4. Flames: aging and removal.
5. Closing walls and tile regeneration.
6. Players, in slot order 0…9: death check, pickup, input, movement, actions.
7. Campaign enemies (campaign mode only).
8. Bombs held by players.

## Round flow

- A round starts with all players frozen for V(30) frames = 1000 ms. [S]
- Round length: V(100) [150] seconds; configurable (`playtime`); an unlimited setting exists. [D][S]
- The round clock runs on real elapsed time, not on the clamped tick time, so a long stall still consumes round time. [S]
- A "hurry" warning is shown for 5 seconds starting when V(101) [60] seconds remain; the closing walls start when it ends, at 55 seconds (`maps.md`). [S]
- The round ends when at most one contender remains: one living player, or one living team in team play. [S] Bomb fuses stop running at that moment. [S]
- If the clock reaches 0:00 while more than one contender is alive, the round ends immediately as a draw ("DRAW GAME"). [observed]
- A player who has just died still counts as a contender for V(25) [20] frames (1000 ms), counted from the tick of death. If the last opponents die within that window of each other, nobody is left and the round is a draw. [S]
- A match is won by the first to V(310) [2] round wins; configurable (`num_to_win_match`). [D]
- Scoring by kills instead of wins exists (`win_by_kills`). [?]
