# Initial Analysis

Date: 2026-10-04. Scope: reconnaissance only (AGENTS.md §12, §56). No modern code written.

Method: file inspection (`file`, `xxd`, `strings`, `objdump`), Ghidra via MCP (project `atomic-bomberman-claude-ghidra`, program `/bm95.exe`), and a local cross-reference index built from an `objdump` disassembly.
**Everything here is static analysis (validation Level 1).** The game was not executed: no Wine/Windows environment is available on this machine (UNKNOWN-001).

Confidence levels follow AGENTS.md §9.

## 1. Version

| Item | Value | Confidence |
|---|---|---|
| Product | Atomic Bomberman, Interplay Productions / Hudson Soft | CONFIRMED (readme, strings) |
| Release | "Version 1.0", readme modified 1997-07-08 | CONFIRMED (readme.txt) |
| `bm95.exe` link time | 1997-07-12 04:12:28 UTC | CONFIRMED (PE header) |
| Internal project name | "Delirium" (`Delirium, Copyright (C) 1997, Interplay Productions, Inc.`, `\delirium`) | HIGH |
| Lead programmer | Kurt W. Dekker (author header in `valuelst.res`, `soundlst.res`, `messages.txt`) | HIGH |

## 2. Main executable

`bm95.exe` is the game. Evidence: it is the only executable importing DDRAW + DSOUND + DINPUT + WSOCK32 together, it contains the gameplay source-file names and all data-path strings, and `autorun.inf` uses its icon. `setup.exe`/`autorun.exe` are installer/launcher, `bmintro.exe` is a movie player. Confidence: CONFIRMED.

## 3. PE structure of bm95.exe

| Field | Value |
|---|---|
| Format | PE32, Intel i386, Windows GUI subsystem 3.10 |
| Image base | `0x00400000`, size of image `0xB3000` |
| Entry point | `0x0044BDCC` |
| Linker version | 2.18 (Watcom `wlink`) |
| Exports | none |
| Checksum | 0 |

| Section | VA | Size | Notes |
|---|---|---:|---|
| `BEGTEXT` | `0x401000` | 0x57000 | Code. Watcom segment name |
| `DGROUP` | `0x458000` | 0x5200 | Initialised data, strings |
| `.bss` | `0x45E000` | 0x48200 | Uninitialised data: all game state lives here |
| `.idata` | `0x4A7000` | 0xE00 | Imports |
| `.reloc` | `0x4A8000` | 0x6000 | |
| `.rsrc` | `0x4AE000` | 0x4600 | Icon (id 99 loaded by `LoadIconA`) |

Ghidra: 1307 functions, 6514 symbols.

### Compiler / runtime

**Watcom C/C++32** — CONFIRMED by the string `WATCOM C/C++32 Run-Time system. (c) Copyright by WATCOM International Corp. 1988-1995` and the `BEGTEXT`/`DGROUP` section names.

Consequences for analysis:

- Functions use the **Watcom register calling convention** (arguments in EAX, EDX, EBX, ECX). Ghidra labels them `__fastcall` and loses arguments (`in_EAX`, `extraout_ECX…`). Decompiler output is noisy; the disassembly is often clearer.
- Game code is compiled **unoptimised** (every local lives at `[ebp-N]`, every function saves all registers). Library code (GNW, sound, runtime) is optimised. This makes the game modules easy to read in assembly.

### Imports

| DLL | Functions | Subsystem |
|---|---|---|
| `DDRAW.dll` | `DirectDrawCreate` | Graphics |
| `DSOUND.dll` | `DirectSoundCreate` | Audio |
| `DINPUT.dll` | `DirectInputCreateA` | Input |
| `WINMM.dll` | `timeGetTime`, `timeBeginPeriod`, `timeEndPeriod`, `timeGetDevCaps`, `timeSetEvent`, `timeKillEvent`, `joyGetNumDevs`, `joyGetDevCapsA`, `joyGetPosEx` | Timing, joystick |
| `WSOCK32.dll` | by ordinal: 2 `bind`, 3 `closesocket`, 6 `getsockname`, 9 `htons`, 12 `ioctlsocket`, 17 `recvfrom`, 20 `sendto`, 21 `setsockopt`, 23 `socket`, 111 `WSAGetLastError`, 115 `WSAStartup`, 116 `WSACleanup` | Network (datagram only: no `connect`/`listen`/`accept`) |
| `USER32.dll` | Window class/creation, `PeekMessageA`/`TranslateMessage`/`DispatchMessageA`, `SetWindowsHookExA`, `GetAsyncKeyState`, `GetKeyState`, `MessageBoxA`, cursor functions | Window, messages, keyboard |
| `KERNEL32.dll` | Comm API (`SetCommState`, `SetupComm`, `PurgeComm`, `GetCommModemStatus`, `EscapeCommFunction`, …), file I/O, threads, mutex, `GetTickCount`, `Sleep` | Serial/modem, files, runtime |
| `GDI32.dll` | `GetStockObject` | Window background brush only |

No GDI drawing, no MCI/MIDI, no registry access.

## 4. Strings

About 3400 strings. Categories and what they revealed:

| Category | Examples | Use |
|---|---|---|
| **Source file names** | `campaign.c`, `aliens.c`, `scheme.c`, `extra.c`, `options.c`, `search.c`, `ai.c`, `net1.c`, `misc.c`, `graf.c`, `datafile.c`, `ani.c`, `you.c`, `bombs.c`, `power.c`, `map.c`, `flame.c`, `tunes.c`, `pcx.c`, `memory.c`, `sound.c`, `color.c` | Passed to a debug allocator as (file, line). Gives a module → address-range map (see `architecture.md`) |
| Data paths | `data/res`, `data/ani`, `data/sound`, `data/schemes`, `%s/%s/%s.pcx` `.ani` `.ali` `.rss` `.res` `.cam` `.sch` | Resource system |
| Named files | `valuelst.res`, `soundlst.res`, `messages.txt`, `master.ali`, `color.pal`, `%u.rmp`, `font%d.fon`, `options.ini`, `nodename.ini`, `sound.ini`, `bmstats.dat`, `levels.dat`, `cfg.ini`, `field%u.plt`, `glue%u.plt`, `mainmenu.plt`, `results.plt` | Resource system, config, save data |
| Animation names | `walk %s`, `stand %s`, `kick %s`, `punch %s`, `pickup %s`, `die green %d`, `cornerhead %u`, `bomb %s green`, `flame %s %u`, `tile %d solid/brick/blank`, `power %s`, `tipnorth/mideast/center` | Sequence lookup by name inside `.ani` files |
| Scheme file grammar | `-V,%u`, `-N,%s`, `-B,%u`, `-R,%2u,%s`, `-S,%u,%d,%d,%d`, `-P,%2u,%2d,%d,%2d,%2d,%s` | Map format (the game can write schemes: it has an editor) |
| Options keys | `levelno`, `num_to_win_match`, `enclosement_depth`, `conveyor_speed`, `team_play`, `random_start`, `stomped_bombs_detonate`, `win_by_kills`, `goldman`, `schemefilename`, `playtime`, `diseases_destroyable`, `lost_net_revert_ai`, `netprotocol`, `modemport/baud/irq/dial`, `keydef` | `options.ini` |
| Powerup names | `disease`, `kicker`, `spooge`, `goldflame`, `trigger`, `disease3`, `random`, … | Powerups |
| ANI chunk tags | `CHFILE`, `FRAM`, `CIMG`, `SEQ `, `STAT` | `.ani` format |
| Network | `net1.c`, `Deleting node %u…`, `RETRANSMITTED A %u TYPE (crit was %d)!`, `*invalid packet*`, `netstats.txt`, `critlog.txt`, `Setting modem to dial '%s'` | Custom reliable-datagram layer with "critical" packets |
| Sound | `soundInit: Setting primary buffer to: %d bit, %d channels, %d rate`, `DSERR_*`, `sound.c: SOS …`, `tunes_sound_stageup()`, `%s.cds`, `%s.hds` | DirectSound streaming library with HMI-SOS-style error names; "stage up" = copy sounds from CD to HD cache |
| Engine | `GNW95 Class`, `GNW95MUTEX`, `win_init() returned %u` | Interplay's GNW windowing library (Win95 port) |
| Debug | `sizeof(OBJ) = %u`, `Mainmenu:  Current memory usage:  %u (audio is %u)`, `DEBUGACTIVE`, `debug.log`, `scr%.5d.bmp` | Debug log, screenshots |
| Other | `Software expired.  Contact Interplay.` | Time-bomb check, probably tied to `levels.dat` (HYPOTHESIS) |

## 5. Entry point and startup path

```text
entry 0x0044BDCC                Watcom CRT startup
  WinMain 0x00443C70            CONFIRMED
    CreateMutexA("GNW95MUTEX")  single-instance guard
    ShowCursor(0)
    0x00443D58                  RegisterClassA("GNW95 Class", wndproc 0x00443E3C, icon 99)
    0x00443DC8                  GetVersionExA: require Win95 / NT4+
    0x0044B4B4                  build argv from command line
    0x0044B85C x3               install signal handlers (handler 0x00443E34)
    Game_Main 0x0042BE22
      Game_InitAll      0x0041095A   subsystem init
      0x0042B060                     intro logo screens (MEDIUM)
      Menu_MainMenuLoop 0x0042B9CE   never returns normally
      GNW_Shutdown      0x0043C668
```

`Game_InitAll` calls, in order, about 30 init routines: GNW/window init (`0x0043E5CC`, `0x0043ADC0`), graphics (`0x00414DF4`: loads `color.pal`, creates window "win1", sets 640×480), joystick (`0x0042971F`), animation loading (`0x0041D695`, `master.ali`), sound list (`0x0042896E`), then per-module inits for players, AI, map, bombs, powerups, extras, flames, schemes, options, net, campaign, aliens. It then caches about a dozen values from `valuelst.res` into globals and computes `ms_per_standard_frame = 1000 / value[30]`. It also logs `sizeof(OBJ) = 152`.

## 6. Main loop and timing

See `architecture.md` for the full description. Summary:

- There is **no single `while(running)` loop**. Each screen (main menu, player setup, match, results) has its own loop that calls `GNW_GetInput` (`0x0043A508`).
- `GNW_GetInput` pumps Windows messages (`PeekMessageA`), then runs every registered **background process**, then returns a key code (or −1).
- A match registers `Game_Tick` (`0x0042A191`) as a background process. `Game_Tick` is the whole simulation + render step.
- `Game_Tick` measures elapsed wall time with `timeGetTime`, clamps it to `value[31]` = 150 ms, stores it in a global, and every subsystem advances by that many milliseconds. **Variable timestep, uncapped.** Confidence: HIGH (static).
- Tuning values in `valuelst.res` are expressed in "standard frames" of 1000 / 20 = 50 ms.

## 7. APIs per subsystem

| Subsystem | API | Evidence | Confidence |
|---|---|---|---|
| Graphics | **DirectDraw** (`DirectDrawCreate` → `SetCooperativeLevel` → `SetDisplayMode` → `CreateSurface` → `CreatePalette`), 640×480, 8-bit palettised | `DDraw_Init 0x00443038` vtable calls at +0x50/+0x54/+0x18/+0x14; constants 0x280/0x1E0 stored to `0x464A70`/`0x464A6C`; `cmp esi,8`; `color.pal` | HIGH (resolution), MEDIUM (bit depth, exclusive mode) |
| Audio | **DirectSound**, sounds are raw PCM streamed/cached from `.rss`; no separate music API | `Sound_InitDirectSound 0x00418FF7`, `soundlst.res` header comment, `timeSetEvent` at `0x0041B03B` (service timer) | HIGH |
| Input – keyboard | `WH_KEYBOARD` hook + `GetAsyncKeyState`; **DirectInput** object also created | `0x0043B44C` (hook install), `0x0043B518` (hook proc), `DInput_Init 0x00444760` | HIGH that all three exist; which one feeds gameplay is UNKNOWN-004 |
| Input – joystick | WinMM `joyGetPosEx` | `Joy_Poll 0x00429790`, log string `joy_initonce(): reporting %u joysticks attached` | HIGH |
| Network | **IPX datagrams through Winsock**: `socket(AF_IPX=6, SOCK_DGRAM=2, NSPROTO_IPX=1000)`; plus **serial / modem** through the Win32 Comm API | `Net_IpxOpenSocket 0x0043BDDE`; `valuelst.res` 1100-1104 lists IPX, modem, serial, TCP/IP | HIGH for IPX and serial; TCP/IP is UNKNOWN-005 (no `AF_INET` socket call found) |
| Timing | `timeGetTime` (wrapper `Time_GetMs 0x0043ACF8`); `timeBeginPeriod` | 24 callers of the wrapper | HIGH |

## 8. Resource / data loading

All game data are **loose files**; there is no archive in use by `bm95.exe` (`install.dat`/`autorun.dat` belong to the installer).

- `cfg.ini` gives two roots: `hdhome` (hard disk) and `cdhome` (CD). Paths are built as `<home>/<dir>/<name>.<ext>` by `Path_BuildResourceName` (`0x00411D17`), which switches on a resource type to choose the directory and extension. Callers pass names such as `mainmenu.plt` or `field%u.plt`; the function maps them onto `.pcx` files (MEDIUM; exact rule UNKNOWN-008).
- `valuelst.res` → integer table read by `Value_Get(id)` (`0x00412135`, >40 callers). Nearly every gameplay constant comes from here.
- `messages.txt` → string table read by `0x004124A4` (MEDIUM).
- `soundlst.res` → sound id → `.rss` file name.
- `master.ali` → list of `.ani` files to load at startup; sequences are then looked up **by name** (`walk north`, `bomb regular green`, …).

### File formats (first pass)

| Ext | Kind | Structure | Confidence |
|---|---|---|---|
| `.sch` | Text | `-V` version, `-N` name, `-B` brick density %, `-R,row,cells` 11 rows × 15 columns (`#` solid, `:` brick, `.` blank), `-S,player,x,y,team` ×10, `-P,powerup,bornwith,has_override,override,forbidden,comment` | HIGH (self-documenting, matches writer strings in exe) |
| `.res` | Text | `;` comments; `id,value[,value…]` (`valuelst`), `id,name` (`soundlst`), `-A/-W/-C/-T,…` arrows, warps, conveyors, trampolines (`extraN.res`) | HIGH |
| `.cam` | Text | `-C,name,levelno,scheme,rovers,rover speed,ghosts,ghost speed,AIs,AI difficulty` | HIGH |
| `.ali` | Text | `-file.ani` per line, `;` comments | HIGH |
| `.ani` | Binary, chunked | `"CHFILEANI "` (10 bytes), u32 payload length (= file size − 16), u16; then chunks of `tag[4], u32 length, u16 id, payload`. Top-level tags: `HEAD`, `PAL ` (8192 bytes), `TPAL` (1028), `CBOX` (4), `FRAM` (contains `HEAD`, `FNAM`, `CIMG`), `SEQ ` (contains `HEAD`, `STAT`…) | HIGH for container, pixel encoding not yet decoded (UNKNOWN-007) |
| `.pcx` | Binary | Standard ZSoft PCX v5, 8-bit, 640×480 (`0A 05 01 08`) | HIGH |
| `.rss` | Binary | Headerless PCM: 22 kHz, stereo, 16-bit signed little-endian (stated in `soundlst.res`) | HIGH (first-party statement; not yet verified by listening) |
| `.pal` | Binary | 768 bytes RGB (values mostly ≤ 63: 6-bit VGA) + 32 768-byte table (probably RGB555 → palette index) | MEDIUM / HYPOTHESIS for the table |
| `.rmp` | Binary | 256-byte palette remap + 3 bytes that equal the RGB percentages in `valuelst.res` 200-247 (`0.rmp` ends `64 64 64` = 100,100,100) | MEDIUM |
| `.fon` | Binary | Custom bitmap font (GNW font format, HYPOTHESIS) | LOW |
| `.bm` | Text | Help/manual pages with `<IMG` tags | MEDIUM |
| `.sfa` | Binary | Interplay MVE movie | HIGH (`file`) |

## 9. First-party gameplay constants (from `valuelst.res`)

These are developer-written values, CONFIRMED as data; how the code applies each is still to be traced.

| Id | Value | Meaning (developer comment) |
|---|---|---|
| 25 | 20 | Nominal frame rate used as reference for frame-count values |
| 30 | 20 | Target frames per second |
| 31 | 150 | Maximum milliseconds the game may advance in one frame |
| 35 | 11 | Number of levels (themes) |
| 41 | 40 | Bomb fuse length in frames (= 2.0 s at 20 fps) |
| 42 | 923 | Starting speed, 1/100 pixel per frame |
| 50, 51 | 1, 2 | Starting bombs, starting flame length |
| 90 | 150 | Speed added per skate |
| 100 | 150 | Default round length, seconds |
| 101 | 60 | Seconds remaining when "hurry" starts |
| 300, 301 | 1000, 1300 | Kicked / punched bomb speed |
| 310 | 2 | Wins needed for a match |
| 400-412 | … | Count of each of 13 powerup types per level |
| 550-564 | … | Per-player cap for each powerup |
| 600-618 | … | Default start cells for 10 players |
| 1100-1104 | 200-750 | Critical-packet resend timeout (ms) per protocol |

## 10. Known unknowns and next step

See `unknowns.md`. Recommended next task: trace `Game_Tick`'s callees to confirm the per-subsystem update functions and recover the `OBJ` (152-byte) structure, starting with `Players_Update`.
