# Installing the modern Atomic Bomberman

The modern game is a new program. It contains none of the original game's graphics, sounds or level data.

**You can play straight away.** With no original game on the machine the program uses its own free set of graphics, sounds, music and arenas (it writes them to its per-user data folder on first start). Steps 2 and 3 below are only for people who own Atomic Bomberman (Interplay, 1997) and want the original look and sound: those are imported once from **your own copy**.

For the import you need:

- the modern game (a download, or built from source: see the end of this page),
- a copy of the original game: an installed folder or the CD. It is the folder that contains `BM95.EXE`, `COLOR.PAL` and a `DATA` folder.

## 1. Get the program

**Download.** Each push to the repository builds seventeen packages with GitHub Actions (workflow "build"):

| Package | For |
|---|---|
| `atomic-bomberman-modern-linux-x86_64.tar.gz` | 64-bit Linux with X11 or Wayland, Intel or AMD processor |
| `atomic-bomberman-modern-linux-arm64.tar.gz` | The same for 64-bit ARM (for example a Raspberry Pi 4 or 5 with a 64-bit system) |
| `atomic-bomberman-modern-linux-i386.tar.gz` | 32-bit Linux on a PC (i686 with SSE2; Debian 12 or another system with glibc 2.36 or later) |
| `atomic-bomberman-modern-linux-armhf.tar.gz` | 32-bit ARM Linux (ARMv7 or later with hardware floating point, for example Raspberry Pi OS 32-bit on a Pi 2 to 5; glibc 2.36 or later, as in Raspberry Pi OS 12 "bookworm") |
| `atomic-bomberman-modern-windows-x64.zip` | 64-bit Windows 10 or later, Intel or AMD processor |
| `atomic-bomberman-modern-windows-arm64.zip` | Windows 11 on ARM |
| `atomic-bomberman-modern-windows-x86.zip` | 32-bit Windows (it runs on 64-bit Windows too) |
| `atomic-bomberman-modern-macos-universal.zip` | macOS 11 or later: the application **Atomic Bomberman**, for Apple silicon and Intel alike (see the note below) |
| `atomic-bomberman-modern-android.apk` | Android 7.0 or later (new; see `android/README.md`) |
| `atomic-bomberman-server-linux-x86_64.tar.gz`, `…-linux-arm64.tar.gz`, `…-linux-i386.tar.gz`, `…-linux-armhf.tar.gz` | Only the dedicated network server, Linux (no graphics needed) |
| `atomic-bomberman-server-windows-x64.zip`, `…-windows-arm64.zip`, `…-windows-x86.zip` | Only the dedicated network server, Windows |
| `atomic-bomberman-server-macos-universal.tar.gz` | Only the dedicated network server, macOS |

They are attached to the workflow run (Actions → build → latest run → Artifacts) and, for tagged versions (`v*`), to the release page. Unpack the package anywhere; it holds the game (`atomic` or `atomic.exe`), the dedicated network server (`atomic_server` or `atomic_server.exe`), the README, the license and this file. Nothing else needs installing: SDL3 is built into the program. A graphics driver with OpenGL 3.3 is required.

**macOS.** Unpack the zip and move **Atomic Bomberman** (the `.app`) where you like, for instance to Applications; start it with a double click. The original game's data, if you have it, is chosen from inside the game (step 2).

The app is not signed with an Apple developer certificate, so macOS refuses the first start after a download with a browser ("Apple could not verify..."). Either allow it once under **System Settings → Privacy & Security → Open Anyway** (the button appears there after the refused start), or remove the download mark in Terminal:

```sh
xattr -dr com.apple.quarantine "Atomic Bomberman.app"
```

The program inside the app still takes the command-line options of this page, as `"Atomic Bomberman.app/Contents/MacOS/Atomic Bomberman" --import-assets ...` where the page says `./atomic`. The package also holds `atomic_server`, a plain command-line program (the same `xattr` command, with its name, lets it start).

Otherwise the Linux commands on this page work the same on macOS.

## 2. Import the original game data (optional, once)

**In the game.** The first time the game starts without original data it asks: **CHOOSE FOLDER** or **NOT NOW**. Choose the folder of your copy (the one that holds `COLOR.PAL` and `DATA`, or the one above it: an installed game or the CD). The game converts the data, showing its progress, and then plays with it.

- "Not now" is not final: **Options → Original Game Data** opens the same folder chooser at any later time, also to change the folder. After the choice the game starts anew by itself, converts and uses the data.
- **Options → Graphics And Sound** switches between the original data and the free set; nothing is deleted either way. Settings, key layouts and your own schemes are kept.
- The folder is remembered. **Each new release converts the data again** from it by itself, so leave the original where it is. If it is not there then (the CD is out, the folder was removed), the data converted before stays in use and a note says so once.
- The folder chooser is the system's own (on Linux it needs a desktop portal or `zenity`). Where there is none the game says so; use the command below.

**With a command.** Run the program with `--import-assets` and the path of the original game. This does the same, and remembers the folder the same way.

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

File and folder names in the original may be in any letter case. Only the sounds change format; the other files are copied as they are and are read by the game's own loaders. The installer, DirectX files, demos, movies and most unused sounds of the original are not copied.

By default the data goes to your per-user data folder:

| System | Asset folder |
|---|---|
| Linux | `~/.local/share/atomic-bomberman-modern/atomic/assets` |
| macOS | `~/Library/Application Support/atomic-bomberman-modern/atomic/assets` |
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

## 4. Sharper graphics at a large window (optional)

The original's pictures are 640 × 480. Once the data is imported (step 2), they can be enlarged four times with an AI upscaling program, for a clean picture in a Full HD window. This is done on your computer, from your imported data, and can be repeated at any time. You need a graphics card with Vulkan support, and, for a full run, a few hours of time (the computer is loaded all the time; it can be stopped and goes on later).

1. **Get the upscaling program.** It is **Real-ESRGAN ncnn Vulkan** (`realesrgan-ncnn-vulkan`, project `xinntao/Real-ESRGAN-ncnn-vulkan` on GitHub, release zip for your system). Unpack it into a folder called `realesrgan` next to the game, or name its place with `--esrgan PATH` or the `ATOMIC_ESRGAN` environment variable. Check that its `models` folder holds `realesrgan-x4plus.param` and `realesrgan-x4plus.bin`; a release zip without these files has been seen, and then they must be taken from the `models` folder of the project's source. Files ending in `.pth` cannot be used. (`--esrgan-model` names another model of that folder.)
2. **Find out which graphics chip it uses.** If the computer has two (a laptop with an integrated chip and a graphics card), the first lines of a run list them as `[0 …]`, `[1 …]`. Use the strong one with `--esrgan-gpu N`. Without it the program may take the weak one, which can give noise and empty pictures, or hang the computer under the long load (a Windows blue screen `DPC_WATCHDOG_VIOLATION` was seen). `--esrgan-tile N` makes the program work on smaller tiles, which loads the chip less.
3. **Try a little first.** This enlarges only the bombs (about a minute):

   ```sh
   ./atomic --upscale-assets --esrgan-gpu 1 --upscale-only bombs
   ```

   (`--assets-dir DIR` if the data is not where the game finds it.) Start the game and look at the bombs. `--no-hires` draws the originals again, to compare.
4. **Run it all.**

   ```sh
   ./atomic --upscale-assets --esrgan-gpu 1
   ```

   It lists the part it is working on (`[n/156] ani/walk`): sprites first, then the full-screen pictures. Ctrl+C stops it; the same command goes on where it stopped, losing at most the part that was being made. At the end it prints a summary.
5. **Check it.** This looks at every picture that is there (the model sometimes gives noise or nothing) and makes the bad ones again; it needs a few minutes if nothing is wrong:

   ```sh
   ./atomic --upscale-assets --esrgan-gpu 1 --hires-verify
   ```

   If it says that parts "could not be made", run it once more; the lines starting with `WARN` name them.
6. **Play.** Start the game as usual. It finds the enlarged pictures by itself, and it logs `INFO  Enlarged graphics: …`.

The result goes into a folder `hires` **next to** the asset folder (so that a new import, which replaces the asset folder, does not remove it; with the per-user asset folder of step 2 it is the folder `atomic/hires` beside `atomic/assets`) and is as copyrighted as the data it comes from: do not share it. A part that does not exist in `hires` is drawn from the original. The frames of each sprite file are kept in one container, `hires/ani/<file>.hani` (a small directory followed by the PNG files as they are), not as thousands of single files; the pictures (`hires/res/*.png`) stay single files. A folder of PNGs that an earlier run made (`hires/ani/<file>/`) is still read by the game, and the next `--upscale-assets` run puts it into a container (nothing is enlarged again) and removes the folder.

**Doing it again.** The command can be run again at any time. A `manifest.txt` in `hires` records what each part was made from, by which model and at which scale, so only what is missing or changed is made, and a run that was stopped (Ctrl+C, a crash of the computer) goes on where it stopped. The manifest is written so that a crash leaves either the old or the new one. If it is lost anyway, `--hires-adopt` takes over the pictures that are there and complete and of the right size instead of making them again. More options for a run:

| Option | Meaning |
|---|---|
| `--hires-verify` | also checks every picture that is there (it is shrunk back and compared with the original) and makes noise, empty or wrongly sized ones again. A picture that fails is tried again as it is, then with an edge around it (the model cannot do very small pictures), and as the last way out it is enlarged without the model (soft, but never wrong); the summary counts those. |
| `--upscale-only A,B` | only these parts: `bombs`, `walk`, `res/mainmenu`, … |
| `--hires-adopt` | see above |
| `--hires-clean-edges` | replaces dark dither pixels at the edge of sprites by their neighbours before enlarging. Little gain, and it changes what every part is made from, so all of them are made again. |
| `--hires-scale N` | 2 to 4 (default 4) |

**What the game adds when it loads the pictures** (nothing of this is stored in `hires`, so it needs no new run): sprites get a smooth outline taken from the original's mask, so the edge of a sprite is not ragged and has no dark rim; the bombs of the original (`bombs`, `bombanim`, `bomanim2`, every size and player colour) are drawn again as a ball with its shadow, with the shape and the shading learned from the original frame, and with the fuse cap enlarged from the original's pixels. `--hires-raw-edges` switches both off and draws the pictures as the model made them. `--window WxH` sets the size of the window at the start (for screenshots and tests).

Not yet enlarged: the fonts (`*.fon`) and the menu text; they are drawn from the original.

### Pictures for Steam (optional)

To add the game to Steam as a non-Steam game (Library → Add a game), `atomic --steam-art` makes the pictures Steam asks for under *Manage → Customize artwork*, and an icon:

| File | In Steam |
|---|---|
| `capsule_600x900.png` | main capsule (*Hauptkapsel*) |
| `hero_3840x1240.png` | background (*Hintergrund*) |
| `logo_1280x720.png` | logo (*Logo*, transparent) |
| `wide_capsule_920x430.png` | wide main capsule (*Weite Hauptkapsel*) |
| `icon_256.png`, `icon.ico` | icon of the shortcut (*Properties → Shortcut*, click the icon) |

They go into a folder `steam` beside the asset folder (`--steam-out DIR` names another). They are drawn from the game's own graphics (the level background, the players, a bomb, the font) and use the enlarged sprites of step 4 where there are some. With the imported original data they show the original's artwork, so keep them to yourself; `--free` makes them from the free graphics set, with nothing of the original in them. The command can be run again at any time and overwrites the pictures.

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

The program is `build/atomic` (Linux, macOS; with `-DAB_MACOS_APP=ON` on macOS it is the application `build/Atomic Bomberman.app`) or `build\Release\atomic.exe` (Windows).

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
- The import from inside the game was run on Linux with the real data in every case but one: first-start offer, conversion from a remembered folder (260 data files, 971 sounds, 48 s), no conversion at the next start, a new release with the folder gone (note once, old data kept) and with the folder back, the game starting anew after a change under Options. **The folder dialog itself was not operated** (it cannot be driven unattended), and none of this was run on Windows or macOS.
- The macOS package and the ARM packages for Linux and Windows are built by GitHub Actions, where the unit tests pass on Apple silicon and on ARM Linux and Windows machines and the programs answer `--version` and `--help`. None of them has been started on a real machine by the author. On macOS the build machine also unpacks the zip, checks the app's signature and starts the game from inside the app: a window with OpenGL 4.1 opened and the main menu was drawn (free asset set, Apple silicon). **Not tried:** opening the app with a double click, the refusal and "Open Anyway" after a download, the folder dialog, sound, an Intel Mac (that half is built, its tests are not run), and the game's window on ARM Linux and Windows.

- The disc holds about a thousand more sounds than the game ever plays (alternate takes and unused lines). 844 of them, voice lines the option **Options → Adjust Audio → Unused voice clips** can play, are imported with the rest (about 140 MB); the others are left out unless you add `--all-sounds` to the import. **Options → Sound Test** lists and plays whatever was imported.
- Three sounds are always reported as not found: `xxx` and `0`, which the original's own sound list marks as dummies, and `zahpu111`, a voice line the list names but the disc does not contain. Nothing is wrong with your copy.
- The Linux steps were run as written: import (246 data files, 971 sounds, 281 MB), then starting the game from the imported folder with graphics, level extras, sounds and music loading.
- The GitHub Actions workflow builds and tests both packages (first run: Linux and Windows both succeeded). The Windows package itself has **not** been started on a Windows machine by the author of these instructions.
