# Powerups

See `README.md` for tags.

## Types

| Id | Name | On level V(400+id) | Cap V(550+id) | Effect |
|---:|---|---:|---:|---|
| 0 | Bomb | 10 | 8 | +1 bomb capacity |
| 1 | Flame | 10 | 8 | +1 blast range |
| 2 | Disease | 3 | – | One random disease |
| 3 | Kicker | 4 | 1 | Kick bombs; button 2 stops them |
| 4 | Skate | 8 | 4 | +V(90) [150] speed |
| 5 | Punch | 2 | 1 | Button 2 punches the bomb ahead. Removes trigger |
| 6 | Grab | 2 | 1 | Button 1 picks up own bomb. Removes spooge |
| 7 | Spooge | 1 | 1 | Button 1 on own bomb lays a line of bombs. Removes grab |
| 8 | Goldflame | −2 | 1 | Blast range 15 |
| 9 | Trigger | −4 | 1 | The next *capacity* bombs laid are trigger bombs, detonated by button 2. Removes punch and jelly |
| 10 | Jelly | 1 | 1 | Bouncing bombs. Removes trigger |
| 11 | Super disease | −4 | – | Three random diseases |
| 12 | Random | −2 | – | Turns into a random non-forbidden type 0-11 |

Table values [D]; effects [S]. A cap of 0 in the data means unlimited. Removed powerups return to the map on random free blank cells. [S]

## Generation

For each type in id order: [S]

- count `n = V(400 + id)`; in a network game type 12 is not generated;
- if `n ≥ 0`, place `n`; if `n < 0`, make `|n|` attempts that each succeed with probability 1/10;
- placing: choose random cells (up to 200 tries) until one is a brick with no powerup; hide the powerup there.

The count comes from the scheme's override when it has one (`maps.md`). [S]

## Reveal

A hidden powerup becomes collectable when its brick is hit by a blast. [S]

During the first V(102) [40] seconds of the round, a revealed punch, grab or super disease is swapped with the hidden powerup of another brick that is none of those three; if none is found it is moved under a brick without a powerup. Host or local game only. [S]

## Pickup

A player collects a collectable powerup when the player's position is in its cell. [S]

1. If diseases are curable (V(124) [1]): with probability 1 / V(125) [10] all the player's diseases end. [S]
2. Apply the effect; clamp to the cap. [S]

## Destruction

Blasts, sliding bombs entering the cell, and the closing walls destroy collectable powerups. [S] A destroyed disease reappears on a random free blank cell unless diseases are set to be destroyable (V(120) [1]). [M]

## Diseases

Chosen uniformly from 9. Each lasts V(130+i) [300] frames = 15 s; a player's diseases share one timer and all end together. [S]

| # | Effect |
|---:|---|
| 0 | Speed ÷ 3 |
| 1 | Speed × 1.5 |
| 2 | Cannot drop bombs |
| 3 | Drops a bomb every tick it can |
| 4 | Blast range 1 |
| 5 | Speed × 1.5 and drops a bomb every tick it can |
| 6 | Fuse ÷ 3 |
| 7 | Immediately swaps position with a random other living player (not in network games) |
| 8 | Reversed directions |

Effects [S]; the mapping of numbers to the names used in the game's text is not established. [?]
