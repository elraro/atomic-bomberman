# Validation Status

Levels as defined in AGENTS.md §53: 1 static (read from the original's code or data), 2 dynamic (observed on the running original), 3 reproduced by the modern implementation, 4 covered by an automated test.

"Observed" refers to the numbered observations in `../reverse-engineering/dynamic-analysis.md`. "Test" names a test in `tests/unit/test_core.cpp`. A mechanic is at Level 4 only when it has been observed on the original **and** a test checks the same behaviour in the modern core; where only one of the two exists, that is stated.

## Observed on the original and tested in the modern core (Level 4)

| Mechanic | Observed | Test |
|---|---|---|
| Start freeze of about one second | D9 (consistent) | start freeze |
| Walking speed | D17 | speed |
| Speed with one skate | D48 | speed |
| Field edge stops movement | D39 | wall stop |
| Lane alignment | D36 | lane alignment |
| Corner slide | D35 | corner slide |
| Direction priority | D24 | direction priority |
| Bomb fuse 2.000 s | D8, D18 | fuse |
| Flame lifetime about 500 ms | D19 | flame lifetime |
| Blast shape, edge stop | D20 | blast propagation |
| Blast stops at the first brick; powerup revealed | D47 | blast propagation, blast and powerups |
| Death decided by cell | D20, D41 | death is cell based |
| Chain reaction on the next tick | D25 | chain reaction |
| Capacity limits bombs | D40 | drop rules |
| A bomb blocks the way back | D41 | drop rules |
| Kick: speed, stop at the last free cell, fuse keeps running | D26 | kick |
| Jelly bomb bounces back | D43 | jelly bounce |
| Trigger bombs: no fuse, detonate on button 2, supply = capacity | D42 | trigger bombs |
| Goldflame | D44 | goldflame |
| Punch: three cells, 0.46 s, fuse paused | D31 | punch |
| Flying bomb bounces over a player | D50 | punch bounce and wrap |
| Grab, carry, throw; fuse restarts | D32 | grab and throw |
| Spooge line | D33 | spooge |
| Hurry below 60 s; closing walls from 55 s, one cell per 250 ms, clockwise spiral, repeated corner cell | D27, D28 | clock and hurry, closing walls |
| Time up is a draw | D29 | round result |
| Both players dead is a draw; kill +1, suicide −1 | D51 | round result, death is cell based |
| Team colours | D38 | team play |
| Arrow turns a sliding bomb | D49 | arrow |
| Conveyor carries an idle player at the belt speed | D37 | conveyor |
| Warp to the linked hole | D45 | warp |
| Trampoline: airborne, lands nearby | D46 | trampoline |
| Pickup by cell | D48 | pickup and caps |
| Scheme born-with and override | D23, D47 | (scheme reader only: scheme file) |

## Tested in the modern core, not yet observed on the original (Level 1 + test)

| Mechanic | Test | Why not observed |
|---|---|---|
| Long-tick clamp at 150 ms | long tick clamp | needs a controlled stall |
| Wall stop against a pillar from a centred position | wall stop | not scripted yet |
| Sliding bomb stops at a flame and explodes | sliding bomb meets flame | not scripted yet |
| Contender window (draw if the last two die up to one second apart) | outsurvive window, round result | only the simultaneous case was observed (D51) |
| Flying bomb wraps around the field | punch bounce and wrap | not scripted yet |
| Stun and powerup loss after a head hit | punch bounce and wrap | not visible in screenshots |
| Powerup caps and exclusive pairs | pickup and caps | inventory is not shown on screen |
| Early-round swap of punch / grab / super disease | blast and powerups | random placement |
| Diseases: effects, 15 s, spreading | diseases | random on the original |
| Dud bombs | dud | rare and random on the original |
| Powerup generation counts | powerup generation | would need many rounds |
| Start area cleared | start area | not scripted yet |
| Closing wall kills a player / detonates a bomb | closing walls | not scripted yet |
| Team play ends when one team is gone | team play | not scripted yet |
| Conveyor: walking with/against, carrying bombs | conveyor | not scripted yet |
| Spooge 50 ms stagger | spooge | sound loads are cached, cannot be timed |

## In the modern game without a counterpart check

- Computer players: structure and probabilities follow the original's code; two search routines and playing strength are unverified (`../reverse-engineering/ai.md`).
- Rendering: one scene compared pixel by pixel (99.46 % identical); animations not compared.
- Audio: event-to-sound mapping from the sound list and observed file names; not listened to.
- Menus and result screens: built from the original pictures; layout only partly compared.

## Not implemented

Campaign mode, networking, options and manual screens. Gamepad input is implemented but has never been run with a controller.
