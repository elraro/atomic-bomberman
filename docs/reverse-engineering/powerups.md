# Powerups

Static analysis (Level 1). Sources: `Powerups_GenerateForRound` (`0x4258E5`), `Powerup_RevealAtCell` (`0x425107`), `Player_PickupPowerup` (`0x41E21E`), `Maybe_Player_GiveDisease` (`0x41DFB6`), `Player_HitOnHead` (`0x421F7E`), `valuelst.res`.

## Types

Ids and names come from the name table at `0x45A31C` (used to build animation names `power %s`) and match the order of values 50-64, 400-412 and 550-564. Confidence: HIGH.

| Id | Name in exe | Start (50+id) | Per level (400+id) | Cap (550+id) | Effect | Confidence |
|---:|---|---:|---:|---:|---|---|
| 0 | bomb | 1 | 10 | 8 | +1 bomb capacity | HIGH |
| 1 | flame | 2 | 10 | 8 | +1 blast range | HIGH |
| 2 | disease | 0 | 3 | – | One random disease | HIGH |
| 3 | kicker | 0 | 4 | 1 | Kick bombs by walking into them; button 2 stops own sliding bombs | HIGH |
| 4 | skate | 0 | 8 | 4 | +150 speed each | HIGH |
| 5 | punch | 0 | 2 | 1 | Button 2 punches the bomb in the next cell. Removes trigger | HIGH |
| 6 | grab | 0 | 2 | 1 | Button 1 on own bomb picks it up; releasing throws it. Removes spooge | HIGH |
| 7 | spooge | 0 | 1 | 1 | Button 1 on own bomb lays a line of bombs ahead. Removes grab | HIGH |
| 8 | goldflame | 0 | −2 | 1 | Blast range = 15 | HIGH |
| 9 | trigger | 0 | −4 | 1 | The next *capacity* bombs laid have no fuse; button 2 detonates the oldest. Removes punch and jelly | HIGH |
| 10 | jelly | 0 | 1 | 1 | Bombs bounce off obstacles when kicked, veer randomly when punched. Removes trigger | HIGH |
| 11 | disease3 ("super bad disease") | 0 | −4 | – | Three random diseases | HIGH |
| 12 | random | 0 | −2 | – | Becomes a random type 0-11 that the scheme does not forbid | HIGH |
| 13 | clog | – | – | – | Speed −150 each. Not placed on the map; it is one of the six roulette prizes (see `JOURNAL.md`, "roulette") | HIGH |

A cap of 0 means no limit. "Removes X" means the conflicting powerup is taken away and scattered back onto the map (`Maybe_Player_RemovePowerup 0x41E16A` → `0x425BED`).

## Storage and states

One slot per cell (heap array at `0x462214`, same `OBJ` structure): state 0 none, 1 hidden under a brick, 2 revealed and collectable. Confidence: HIGH.

## Generation at round start

Confidence: HIGH. Scheme overrides are written into values 400-412 (and born-with into 50-62) when the scheme is applied, so this code sees them through `Value_Get`.

For each type 0…12, in order:

```text
n = value(400 + type)
if network game and type == 12: n = 0
if n >= 0: place n powerups
else:      make |n| attempts, each succeeding with probability 1/10
place: up to 200 tries: pick a random cell; accept if it is a brick with no powerup yet
```

Powerups are only ever generated under bricks. On a network client the layout is received from the server instead.

## Reveal

When a blast burns a brick, `Powerup_RevealAtCell`:

- In a local game or on the host, if the round is still in its early period (comparison against value 102 = 40; the unit and direction are not confirmed) and the hidden powerup is punch, grab or disease3, it is **swapped** with the hidden powerup of another random brick that is not one of those three; failing 200 tries, it is moved under an empty brick and nothing appears. This keeps "over-powerful" items out of the opening (developer's comment on value 102).
- A hidden powerup (state 1) becomes collectable (state 2).

## Pickup

Confidence: HIGH.

A player picks up a collectable powerup when the player's cell equals the powerup's cell; checked at the start of the player's update and after every pixel moved. Network-controlled players do not process pickups locally.

On pickup:

1. If diseases are curable (value 124) there is a 1-in-N chance (value 125 = 10) that all the player's diseases are cured. This roll happens for every pickup, including a disease.
2. The type's effect is applied (table above); inventory is clamped to the cap.
3. A pickup sound plays; from the 7th non-disease pickup onward, every 5th one plays an extra sound.

## Destruction

- A blast reaching a collectable powerup destroys it and stops there.
- A sliding bomb entering a cell with a collectable powerup destroys it.
- The closing walls remove powerups.
- If the destroyed powerup is a **disease** and the "diseases destroyable" option (`0x464990`) is off, a replacement is placed on a random blank cell free of bombs, powerups and players (`Powerup_RespawnOnRandomCell 0x4255B2`). The same routine is used to scatter powerups a player loses.

## Losing powerups

`Player_HitOnHead` (a flying bomb lands on a player; local games only): the player is stunned for 16 ticks, and loses `value 670 + rand(value 671)` = 1 + 0…2 powerups. Each is chosen at random among inventory entries above the starting amount and is scattered back onto the map.

Note the stun is counted in **ticks**, not milliseconds (`OBJ+0x3A` is decremented once per update), so its real duration depends on frame rate. Confidence: HIGH that it is tick-based.

## Diseases

Confidence: MEDIUM-HIGH for effects (each flag was seen at its point of use).

A disease is chosen with `rand() % 9`. Duration = value(130 + index) frames × 50 ms = 300 frames = **15 s** for all nine. All active diseases share one timer; when it runs out, all are cured.

| Index | Effect |
|---:|---|
| 0 | Speed ÷ 3 |
| 1 | Speed × 1.5 |
| 2 | Cannot drop bombs |
| 3 | Drops bombs continuously (button 1 forced every tick) |
| 4 | Blast range forced to 1 |
| 5 | Speed × 1.5 and drops bombs continuously |
| 6 | Fuse ÷ 3 |
| 7 | Instant: swap position with a random other living player. Not chosen in network games |
| 8 | Reversed direction controls (human players only) |

Spreading by proximity is described in `players.md`.
