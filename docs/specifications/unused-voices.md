# Unused voice clips

**Not behaviour of the original.** The original never plays these recordings. This is an option of this implementation, off unless the player switches it on.

## What is on the disc

`data/sound/` of the original holds 2027 sound files (`.rss`). The game plays a sound by a number, and `data/res/soundlst.res` gives each number a file name; that list names 974 files (971 of them are on the disc). The other **1056 files are never played**: no number leads to them, and the executable holds none of their names (checked for `asskill` and `yoassgrs`; it reads sound names from the list only). Confidence: CONFIRMED for "not in the list" (counted from the user's copy); HIGH for "never played".

Most of them are more takes of the same kinds of voice lines the game does play. Some are coarse (file names such as `asskill`, `bustass`, `yoassgrs`), which is the likely reason they were left out. **Nobody has listened to them for this document**: what follows goes by file names and by the original's own list, not by what is said in each clip.

## The option

*Options, Adjust Audio, "Unused voice clips: No / Yes"* (setting `unused_voices`, 0 by default).

With it on, 844 of the unused clips are added to the series of sounds the game chooses from for six events. From then on they are treated exactly as the listed ones: the original thins each series at random to a few lines per session (7 taunts, 7 pickup lines and so on; one each in the normal memory model) and picks a new selection between rounds after a time; the unused clips are candidates in that draw like any other. Nothing else changes: when a line is spoken, and how often, is as before.

The sounds are only heard on the machine that has the option on. It changes nothing in a match, a network game or a match record.

## Which clip goes where

A clip's file name starts with the letters of its family of recordings, then a number (`add36a`, `zafp13aa`). The rule: **a family goes to the event for which the original's own list uses that family in at least two thirds of the lines that name it.**

| Event (first sound number) | Families | Uses in the original's list | Unused clips added | Confidence |
|---|---|---|---|---|
| Taunt after a death (700) | `add` | 127 of 127 | 127 | HIGH |
| | `jdd` | 21 of 21 | 49 | HIGH |
| | `zaa` | 119 of 172 (others: draw, match won, quitting) | 138 | MEDIUM |
| | the nine with names of their own: `asskill`, `bustass`, `myass1`, `yoassgrs`, `brakhead`, `kisswook`, `jesus`, `godhere`, `hailgon` | never named | 9 | LOW: by their names only |
| Powerup picked up (400) | `pgapu` | 28 of 36 (others: one disease) | 14 | MEDIUM |
| | `zahpu` | 22 of 33 (others: one disease) | 51 | MEDIUM |
| | `jgap` | never named | 21 | LOW: taken as the `pgapu` lines by the third voice (as `jdd` is to `add`, `jgad` to `pgad`) |
| "Awesome" (1400) | `pupp` | 46 of 48 | 90 | HIGH |
| | `jupp` | 24 of 24 | 8 | HIGH |
| | `zafp` | 29 of 31 | 130 | HIGH |
| | `sofm` | 3 of 3 | 20 | MEDIUM (three lines only) |
| | `jofm` | never named | 2 | LOW: taken as the `sofm` lines by the third voice |
| A long string of bombs (1200) | `dmb` | 23 of 23 | 15 | HIGH |
| | `jmb` | 8 of 8 | 7 | HIGH |
| | `zae` | 42 of 42 | 22 | HIGH |
| A disease caught (2300) | `pgad` | 44 of 46 | 58 | HIGH |
| | `zahd` | 29 of 34 | 70 | HIGH |
| | `jgad` | 15 of 19 | 9 | HIGH |
| The match is won (2000) | `jjfm` | 3 of 3 | 4 | MEDIUM (three lines only) |

Confidence here is about the family belonging to the event. Whether a particular unused take was left out because it does not fit is not known.

## Not placed

212 unused files have no place and are not played by the option. They can be heard in *Options, Sound Test* when all sounds were imported (`atomic --import-assets PATH --all-sounds`) or the game runs from an original folder.

| Files | Why |
|---|---|
| `dis`, `zad`, `disz` (81) | Lines for one particular disease; the name does not say which, and the wrong one would be said for a disease. |
| `gen`, `zai`, `eofm`, `zacm` (50) | The list uses these families for several events (hurry, draw, match won, quitting, the title) and none for two thirds. |
| `zabm` (16) | Never named, and no family corresponds to it. |
| The rest (65) | Not voices by their names: `bomb…`, `alarm…`, `boing…`, `beep…`, `sqrdrps…`, `bmdrop1`. |

## Importing

The 844 placed clips are converted with the listed sounds at every import (about 143 MB beside the 262 MB of the listed ones), so the option works without importing again with another setting. An asset folder imported by an earlier release has none of them: the option then says so, and the original has to be imported once more. The free sounds have none.

## Implementation and tests

`src/resources/unused_voices.{hpp,cpp}` (the table), `src/audio/audio.cpp` (`setUnusedVoices`, `findUnusedVoices`, `cull`), `src/resources/asset_import.cpp`. Unit tests (`ab_tests`): the table for a clip of every kind, the setting's default and reading, the import. Checked by hand against the user's copy: the option finds 844 clips. **Not checked: by ear.**
