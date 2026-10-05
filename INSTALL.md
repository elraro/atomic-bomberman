# Installing the modern Atomic Bomberman

The modern game is a new program. It contains none of the original game's graphics, sounds or level data.

**You can play straight away.** With no original game on the machine the program uses its own free set of graphics, sounds, music and arenas (it writes them to its per-user data folder on first start). Steps 2 and 3 below are only for people who own Atomic Bomberman (Interplay, 1997) and want the original look and sound: those are imported once from **your own copy**.

For the import you need:

- the modern game (a download, or built from source: see the end of this page),
- a copy of the original game: an installed folder or the CD. It is the folder that contains `BM95.EXE`, `COLOR.PAL` and a `DATA` folder.

## 1. Get the program

**Download.** Each push to the repository builds five packages with GitHub Actions (workflow "build"):

| Package | For |
|---|---|
| `atomic-bomberman-modern-linux-x86_64.tar.gz` | 64-bit Linux with X11 or Wayland |
| `atomic-bomberman-modern-windows-x64.zip` | 64-bit Windows 10 or later |
| `atomic-bomberman-modern-android.apk` | Android 7.0 or later (new; see `android/README.md`) |
| `atomic-bomberman-server-linux-x86_64.tar.gz` | Only the dedicated network server, Linux (no graphics needed) |
| `atomic-bomberman-server-windows-x64.zip` | Only the dedicated network server, Windows |

They are attached to the workflow run (Actions → build → latest run → Artifacts) and, for tagged versions (`v*`), to the release page. Unpack the package anywhere; it holds the game (`atomic` or `atomic.exe`), the dedicated network server (`atomic_server` or `atomic_server.exe`), the README, the license and this file. Nothing else needs installing: SDL3 is built into the program. A graphics driver with OpenGL 3.3 is required.

## 2. Import the original game data (optional, once)

Run the program with `--import-assets` and the path of the original game.

Linux:

```sh
./atomic --import-assets /path/to/original/game
```

Windows (Command Prompt, in the folder where you unpacked the zip):

```bat
atomic.exe --import-assets "D:\"
atomic.exe --import-assets "C:\Games\Atomic Bomberman"
```

The import reads the original folder and never changes it. It writes about 280 MB:

| What | From the original | In the asset folder |
|---|---|---|
| Palette, colour remaps, fonts, help pages | `COLOR.PAL`, `0.RMP`…`9.RMP`, `FONT*.FON`, `*.BM` | same names, lower case |
| Pictures, lists and campaigns | `DATA\RES\*.PCX`, `*.RES`, `*.CAM` | `data/res/` |
| Schemes (maps) | `DATA\SCHEMES\*.SCH` | `data/schemes/` |
| Sprites | `DATA\ANI\*.ANI`, `MASTER.ALI` | `data/ani/` |
| Intro movie | `INTRO\BMINTRO.EXE` (only the movie at its end, not the program) | `intro.mve` |
| Sounds and music | the `.RSS` files named in `SOUNDLST.RES` (raw audio) | `data/sound/*.wav` (standard WAV, 22 050 Hz stereo 16-bit) |

File and folder names in the original may be in any letter case. Only the sounds change format; the other files are copied as they are and are read by the game's own loaders. The installer, DirectX files, demos, movies and unused sounds of the original are not copied.

By default the data goes to your per-user data folder:

| System | Asset folder |
|---|---|
| Linux | `~/.local/share/atomic-bomberman-modern/atomic/assets` |
| Windows | `%APPDATA%\atomic-bomberman-modern\atomic\assets` |

To keep everything in one place instead (for example on a USB stick), put the data next to the program:

```sh
./atomic --import-assets /path/to/original/game --assets-dir ./assets
```

When it finishes it prints how many files and sounds were imported. A few sounds listed by the original but missing from its own disc are reported as "not found"; that is normal (3 in version 1.0).

## 3. Play

Start the program with no arguments. It looks for the game data in this order and uses the first place that has it:

1. `--game-dir PATH` on the command line,
2. the `ATOMIC_GAME_DIR` environment variable,
3. an `assets` folder next to the program, then in the current folder, then in the per-user data folder (step 2),
4. a `game` folder holding a copy of the original itself (current folder, next to the program, or one level above it).

The terminal prints `INFO  Game files: …` with the folder in use. If none of these has the original data, the free asset set is used (`--free` chooses it on purpose).

Controls and options are in `README.md`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Flat coloured shapes, no menu, title says "no game files found" | The data was not imported, or was imported somewhere the program does not look. Run step 2 again, or pass `--game-dir`. |
| "not an Atomic Bomberman folder" during import | The path is not the folder that contains `COLOR.PAL` and `DATA`. On the CD it is the top folder. |
| "OpenGL 3.3 is not available" | Graphics driver too old, or a remote/virtual display without 3D. |
| No sound | Run from a terminal and look for `WARN` lines; `--mute` disables sound on purpose. |

## Legal

The modern game is free software under the GNU General Public License, version 3 or later (see `LICENSE`). It is an unofficial fan project, not affiliated with Interplay, Hudson Soft, Konami or any other rights holder; see the disclaimer in `README.md`.

The original game's data is copyrighted by its owners. Import it only from a copy you own, and do not share the resulting asset folder. The packages built from this repository contain no original material.

## Building from source

Requirements: CMake 3.20 or newer and a C++20 compiler (GCC, Clang or Visual Studio 2022).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAB_FETCH_SDL3=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DAB_FETCH_SDL3=ON` downloads SDL3 and links it into the program. Leave it out to use an SDL3 already installed on the system (for example `libsdl3-dev`). On Linux, building SDL3 needs the X11, Wayland, OpenGL and audio development packages; the exact list is in `.github/workflows/build.yml`.

The program is `build/atomic` (Linux) or `build\Release\atomic.exe` (Windows).

## Network games and the dedicated server

To play over a network, one player chooses **Start Network Game** in the menu and the others **Join Network Game** (see the README's "Network play"). Nothing extra needs installing.

`atomic_server` runs the same server without a window, for a machine that is always on:

```sh
./atomic_server --name "My server" --game-dir /path/to/assets
```

- It looks for the game data like the game does: `--game-dir`, the `ATOMIC_GAME_DIR` environment variable, an `assets` folder in the current folder, the per-user folder, or a `game` folder. It only needs the schemes, `valuelst.res` and the level extras; without any original game data it serves the eight arenas of the free asset set.
- Port **27410**, TCP and UDP, unless `--port` says otherwise. Open it in the firewall and, behind a router, forward both.
- `--password TEXT` keeps strangers out. `--hidden` stops it answering searches on the local network.
- `atomic_server --help` lists everything. `Ctrl-C` or `quit` stops it.

## Status of these instructions

- The dedicated server and network play were run on Linux on one machine only (their automated tests also pass on Windows in CI); see the README for what that means.

- The disc holds about a thousand more sounds than the game ever plays (alternate takes and unused lines). They are left out unless you add `--all-sounds` to the import (about 180 MB more); **Options → Sound Test** then lists and plays them.
- Three sounds are always reported as not found: `xxx` and `0`, which the original's own sound list marks as dummies, and `zahpu111`, a voice line the list names but the disc does not contain. Nothing is wrong with your copy.
- The Linux steps were run as written: import (246 data files, 971 sounds, 281 MB), then starting the game from the imported folder with graphics, level extras, sounds and music loading.
- The GitHub Actions workflow builds and tests both packages (first run: Linux and Windows both succeeded). The Windows package itself has **not** been started on a Windows machine by the author of these instructions.
