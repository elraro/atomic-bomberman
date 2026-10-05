# Audio

## Original

DirectSound. Every sound, including music, is a headerless `.rss` file: 22 050 Hz, stereo, 16-bit signed little-endian (2027 files, 429 MB). `data/res/soundlst.res` maps numeric sound ids to file names; the game plays a sound by id, usually choosing at random inside an id range reserved for one kind of event. It loads a file the first time the sound is needed and caches it (observed: one file open per sound, `dynamic-analysis.md` D34), with at most 5 sounds at once (value 8).

Id ranges, from the comments in `soundlst.res` and the sounds observed in scripted runs:

| Event | Ids | Observed file |
|---|---|---|
| Bomb dropped | 100-109 | `bmdrop2`, `bmdrop3` |
| Bomb kicked | 120-129 | `kicker3` |
| Sliding bomb stops | 130-134 | `bmbstop1` |
| Jelly bomb bounce | 135-139 | – |
| Closing-wall block | 140-142 | `sqrdrop4` |
| Bomb punched | 150-159 | `kbomb1`, `kbomb2` |
| Flying bomb bounces / lands | 160-169 | `bmdrop3` |
| Bomb grabbed | 170-171 | `grab2` |
| Bomb explosion | 200-299 | `explo1`, `explode2`, `bomb_04`, … |
| Player dies | 300-… | – |
| Hit on the head | 360-369 | – |
| Powerup collected | 400-499 | – |
| Hurry | 2700-2799 | `zai01c` (not in this range: the hurry voice comes from another table) |

## Modern

`src/audio/audio.*` opens the default SDL3 playback device, parses `soundlst.res`, and plays a sound by creating an `SDL_AudioStream` (source format: S16LE, 2 channels, 22 050 Hz) bound to the device. Files are read on first use and cached; up to 8 voices, the oldest is dropped when full.

The gameplay core does not know about audio. It records `Event`s during a tick (`World::takeEvents`), and the application maps each event kind to an id range (`playEvents` in `src/app/main.cpp`).

## Status and gaps

- Verified only by running: the device opens, 1051 sound names are read (the same count the original logs), and a scripted round plays without missing-file warnings. **Nobody has listened to the result yet.**
- Level music loops during play (sound id 1100 + level: `grnacres`, `generic`, `hockey`, `pyramid`, `mineshft`, `battle`, `gieger`, `haunted`, `ocean`, `swamp`, `sewer`), at half gain. The menu tune (1010), the pre-game screens tune (1020) and the end-of-round tune (1130, then a random voice line: series 1700-1999 for a drawn round, 2000-2299 when the match has a winner) play as in the original's `Match_Run`. No no per-death-animation sounds, no taunts.
- The throw range (172-175) and the exact ranges for death and hurry sounds are taken from file comments, not from the code.
- `--mute` disables audio (use it for automated runs).

## Leaving the game

The original's quit routine (`0x412987`) asks for confirmation (text 10), stops the music, plays a random sound of the 2600 series ("exiting game", 49 voice lines in `soundlst.res`) and waits 4000 ms before exiting. The modern game does the same when leaving through the main menu (Exit or Esc), after the same Yes/No question; any key skips the wait. Confidence: HIGH (static).

## Intro

The original's intro (`0x42B060`): tune 1000 (`title`), pictures `iplogo` and `hslogo`, then a random sound of the 2800 series (the "Atomic Bomberman!" voice, 11 lines) with the `title` picture. Each picture (`0x42A088`) stays until Enter, Space or Esc, or for value 12 (7) seconds; any key plays sound 20, a continuing key sound 10. Then the main menu and tune 1010. Confidence: HIGH (static). The modern game does the same when started at the menu; `--no-intro` skips it.

## Sound series and the full event table

The original's sound call (`0x427961`) takes one id and plays one sound out of the run of consecutive ids that starts there, preferring one not played recently; if that first id is not defined in `soundlst.res` nothing plays. `Audio::playSeries` does the same. Ids used by the original's game code, all implemented:

| Event | First id | Run in `soundlst.res` |
|---|---:|---|
| Bomb dropped | 100 | 100-101 |
| Bomb dropped by a "drops bombs" disease | 550 | 550-554 |
| Last bomb of a capacity ≥ value 651 laid, 1 in value 650 | 1200 | 1200-1279 |
| Kick / stop / punch (and throw) / bounce / grab | 120 / 130 / 150 / 160 / 170 | |
| Explosion | 200 | 200-219 |
| Closing wall cell | 140-142 | fixed three |
| Hurry | 2700 | 2700-2738 |
| Death | 300 | 300-301 |
| Bomb on the head | 360 | 360-363 |
| Powerup picked up | 400 | 400-450 (451 is missing, so 452-484 are never chosen) |
| Jelly picked up | 135 | 135-137 |
| 7th good pickup of the round and every 5th after | 1400 | 1400-1404 |
| Disease caught: 1 in 3 | 3000 + 50 × disease | per disease |
| Disease caught: otherwise | 2300 | 2300-2387 |
| Warp | 1330 | 1330-1332 |
| Trampoline | 350 | 350-353 |

The original also asks for sound 700 as a "post-death taunt" (1 in value 95, 25 frames into a death animation, `0x41F4EB`), but id 700 is not defined (the taunts start at 701), so by the rule above nothing plays; this implementation plays nothing there either. Confidence: HIGH for the ids and conditions (static); the "not played recently" preference is simplified to "not the previous one".
