# Explosions

See `README.md` for tags.

## Model

An explosion is a set of **flames**, at most one per cell. A flame has an owner (for kill credit), an age, and a visual piece. Placing a flame on a cell that already has one replaces it and restarts its age. [S]

## Propagation

When a bomb with range `R` detonates: [S]

```text
for each direction (north, east, south, west), except the one a triggering blast arrived from:
    flame on the bomb's cell
    a collectable powerup on the bomb's cell is destroyed
    for step = 1 … R:
        c = the cell `step` cells away
        1. resting or sliding bomb in c  → queue it for detonation, give it this bomb's owner; stop
        2. collectable powerup in c      → destroy it; stop (no flame on c)
        3. c is solid or outside the grid → stop
        4. c is a brick                  → brick-burning flame on c; reveal its powerup; stop
        5. otherwise                     → flame on c; continue
```

## Interaction matrix

| In the cell | Flame placed | Blast continues | Effect |
|---|---|---|---|
| Blank | yes | yes | – |
| Solid / outside | no | no | – |
| Brick | burning-brick flame | no | Brick removed when that flame ends; hidden powerup revealed |
| Resting or sliding bomb | no | no | Detonates next tick |
| Flying or held bomb | per other rules | yes | None |
| Collectable powerup | no | no | Destroyed (a disease may respawn elsewhere, `powerups.md`) |
| Player | yes | yes | Dies (`players.md`) |
| Flame | replaced | yes | Age restarts |

All rows [S].

## Lifetime

- Regular flame: V(10) [10] frames = 500 ms. [S]
- Burning brick: V(20) [10] frames = 500 ms; the brick becomes blank when it ends. [S]
- Flames age every tick regardless of the round state. [S]

## Lethality

A flame is lethal for its whole lifetime, to any player whose position is in its cell (`players.md`). There is no partial-overlap rule. [S]

Whether a brick-burning flame can kill is unspecified. [?]
