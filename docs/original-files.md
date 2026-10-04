# Original Files

Inventory of the user-supplied original game under `game/` (read-only evidence; never modified, never committed).

- Product: **Atomic Bomberman** (Interplay Productions / Hudson Soft), readme says *Version 1.0, modified July 8, 1997*.
- Total: 2758 files, ~532 MB. This looks like a full CD image (installer, DirectX redistributable, demos, tools) rather than only an installed game.
- Hashes below are SHA-256, computed 2026-10-04 with `sha256sum`.

## Files per top-level folder

| Folder | Files | Bytes | Contents |
|---|---:|---:|---|
| (root) | 44 | – | Executables, config, fonts, palette, help text, installer archives |
| `data/ani` | 96 | 7 894 009 | 95 `.ani` sprite animation files + `master.ali` list |
| `data/res` | 71 | 10 728 542 | `.pcx` backgrounds, `.res` text resources, `.cam` campaign files |
| `data/schemes` | 68 | 98 277 | `.sch` map scheme text files |
| `data/sound` | 2037 | 444 735 576 | 2027 `.rss` raw PCM sounds + 10 stray `.gif` images |
| `directx` | 391 | 16 486 849 | DirectX redistributable (drivers, d3d/ddraw/dsound DLLs). Not game code |
| `demos` | 22 | 28 507 421 | Unrelated bundled software (`engage`, `prodigy`) |
| `winereg` | 11 | 403 249 | Electronic registration program. Not game code |
| `tools` | 15 | 1 231 529 | Interplay's unsupported modding tools + documentation |
| `intro` | 1 | 15 840 470 | `bmintro.exe` – self-contained MVE intro movie player |
| `theme` | 2 | 3 090 189 | Windows desktop theme (`theme.zip`) |

Combined SHA-256 of the `sha256sum` listing of `data/ani/*`, `data/res/*`, `data/schemes/*` (sorted glob order):
`1ac5f007a29ca45cbf4296d2a85440f6f2e5f32e1e363cc455e144c2c2a6aadd`

## Executables

| File | Size | SHA-256 | Type | Linked | Notes |
|---|---:|---|---|---|---|
| `bm95.exe` | 424 448 | `d1ff5f6fba03f20b4a161af1c75165b841e536cd9398ed2692202cee6d6b3857` | PE32 i386 GUI | 1997-07-12 | **Main game executable** (Watcom C/C++32) |
| `makecfg.exe` | 23 040 | `52f670b04eeb61ada0bf752bea5f099f00f34c9aac1c092a032b543977b5c306` | PE32 i386 console | 1997-07-10 | Writes `cfg.ini` (`Usage: %s hdhome cdhome`) |
| `setup.exe` | 371 889 | `5d3b760bdc52ee7108cf96797f10ae944659cc9e3651715d996c10851e18eae0` | PE32 i386 GUI | 1997-07-10 | Installer (uses `install.dat`) |
| `autorun.exe` | 908 288 | `447459f67d0f5cff84734e80557076c3be21df95f280c3100140ad78e85c9851` | PE32 i386 GUI | 1997-07-08 | CD autorun launcher (uses `autorun.dat`, DSETUP) |
| `sfadmo95.exe` | 77 824 | `51b8ea024ce14121e52e1779c2b3507c85c2a5dba1c84c85c8c664ffd378258c` | PE32 i386 GUI | 1997-03-13 | Player for `trailer.sfa` (HYPOTHESIS from name) |
| `intro/bmintro.exe` | 15 840 470 | `945c24ae5e6ee9d39e6bc73ac522a5b2ae58b7dd1890d2f5ef340408d03749ad` | PE32 i386 GUI | 1997-04-30 | "Win95 Interplay MVE File Player" with movie embedded |
| `tools/fredspit.exe` | – | `f66f4d0f3f69183582b7dbc9212be55a008a145281fd598276aa5306621acd2a` | PE32 i386 console | 1997-01-02 | Animation tool (see `tools/fredspit.txt`) |
| `tools/fredit.exe` | – | `9a253f556fed4402564416d180223b02bb7a963c3a2b1a5d58039619c0d7b278` | DOS LE (CauseWay) | – | Animation editor "FREDIT" (see `tools/fredit.doc`) |
| `tools/pss.exe` | – | `97c8b78b2ba221ca96c09d007d177db08faa7bf013ef964a8cd32d47838c6732` | DOS MZ | – | Sound tool (see `tools/pss.txt`) |
| `tools/extpss.exe` | – | `9459548d8f640a097514cdd4497791bd3020c40bca5d6987c88b7f91e4072c66` | DOS MZ | – | Sound tool |
| `tools/playsh.exe` | – | `daba9376f1be8e9f6fdeff0c577a3215f4d89e8b2d3d316eee410c241ed3d99a` | DOS MZ | – | Sound player (see `tools/playsh.txt`) |
| `tools/number.exe` | – | `bd195b301c8d2ae4850d9c3b26d37cd5d7cf7553b4f0d61b021c56babe159f81` | DOS MZ | – | Unknown helper |
| `winereg/winereg.exe` | – | – | PE32 i386 GUI | – | Registration; not analysed |

## DLLs in the game root

`dsetup.dll`, `dsetupe.dll`, `dsetupj.dll` (PE32) and `dsetup6e.dll`, `dsetup6j.dll` (16-bit NE) are Microsoft DirectX Setup libraries.
`bm95.exe` does **not** import any of them; it imports only system DLLs (see `reverse-engineering/initial-analysis.md`).

## Game data in the root

| File | Size | SHA-256 | Format (determined by content) | Notes |
|---|---:|---|---|---|
| `cfg.ini` | 66 | `10ebab0cc1ca0c951ed65ce7d99765eb41b0beab4c2e732765b82c2d6e274625` | Text `key=value`, CRLF, ends with `^Z` | `hdhome`, `cdhome`, `soundonoff`, `netonoff`, commented `debug` |
| `messages.txt` | 9 517 | `d18535387fdfc59b5d9facc39a1f740bdd5aef47d19c059e526b2f248bcd8af9` | Text, `;` comments | Numbered UI strings |
| `color.pal` | 33 536 | `50d3b5ef329142fe32a1980ccfc06d21d209ef240bf5561bc541dee6f372ff42` | Binary: 768 + 32 768 bytes | Palette + lookup table (see file-format notes) |
| `0.rmp` … `9.rmp` | 259 each | – | Binary: 256 + 3 bytes | Per-player colour remap tables |
| `font0.fon`, `font1.fon`, `font6.fon` | 2 863 / 7 012 / 3 940 | `6faff192…` / `ed107928…` / `ef947418…` | Binary, custom (not Windows FON) | Loaded as `font%d.fon` |
| `levels.dat` | 4 | `15d0fa7bcf9d3b8e32ed4d6d416d810cfce6a1edb12561d5b9149a7e95edac42` | 4 bytes `81 fb bf 33` | Purpose unknown (UNKNOWN-006) |
| `bm95.res` | 17 418 | `7279fb64c6d0541c6c51d3950ab1b1d077b8fabceff086caf27533b3eba71e11` | Watcom resource file (icon) | Not a game data file |
| `bm95.ico` | 17 274 | – | Windows icon | |
| `*.bm` (10 files) | – | – | Text help pages (`file` reports "data" for most of them; cause not checked) | In-game manual/help (`credits.bm`, `options.bm`, …) |
| `install.dat` | 353 885 | `fb329c5744687c00d6ccc81b159de2b02cf4a447bf8b7502874387150f89588f` | Archive with embedded file table (`_BACKUP_.PCX`, `COLOR.PAL`, …) | Installer data, not used by `bm95.exe` (not referenced in its strings) |
| `autorun.dat` | 573 459 | `2c0310b78624c4353542ca13ed52a3439c74f9b428fe21cd7a32096192b9c58a` | Same archive format | Autorun data |
| `trailer.sfa` | 19 786 542 | `f1e89b3c99e3272925e0c08a8308ab4a2b48134aa9a37f380759a2f90cbb0089` | Interplay MVE movie | Trailer |
| `readme.txt`, `readme.bm` | – | – | Text | Version 1.0 readme / licence |

## Key data files

| File | SHA-256 |
|---|---|
| `data/res/valuelst.res` | `99b525920f801ba6c4092f6d4e6ad46963389f2d07f37eae1b9ae6314c69636d` |
| `data/res/soundlst.res` | `36abf9a917fa88f886cb0c854fd3d4b0daa5f4aa97bd21fe14cc6066620a7a2f` |
| `data/ani/master.ali` | `44eb82504d6f2ae2ee39db3ab17cfe35aa306a8f72323dc36630b9c168e7f7ca` |
| `theme/theme.zip` | `b0ddcaf21a94daa656bb98ede31756080ac0af53d70bf49a6e00e76fe6a3d9e3` |

## Developer documentation shipped on the disc

`tools/*.txt` and `tools/fredit.doc` are Interplay's own (unsupported) modding notes: `anims.txt`, `game.txt`, `stages.txt`, `pss.txt`, `playsh.txt`, `fredspit.txt`, `toolhelp.txt`.
Together with the heavily commented `valuelst.res`, `soundlst.res`, `messages.txt`, `.sch`, `.cam` and `extra*.res` files, these are first-party evidence about game rules and data formats.

## Repository hygiene

`game/` is copyrighted material. If this directory becomes a git repository, `game/` and any extracted assets must be ignored.
