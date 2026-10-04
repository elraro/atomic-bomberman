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
- Level music loops during play (sound id 1100 + level: `grnacres`, `generic`, `hockey`, `pyramid`, `mineshft`, `battle`, `gieger`, `haunted`, `ocean`, `swamp`, `sewer`), at half gain. No menu or results music, no per-death-animation sounds, no taunts.
- The throw range (172-175) and the exact ranges for death and hurry sounds are taken from file comments, not from the code.
- `--mute` disables audio (use it for automated runs).
