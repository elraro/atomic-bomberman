# Architecture of the Original (bm95.exe)

First subsystem map. Static analysis only (Level 1). Addresses are virtual addresses in `bm95.exe` (image base `0x00400000`). Names in `Code_Style` are names applied in the Ghidra project.

## 1. Layers

```text
+--------------------------------------------------------------+
| Game ("Delirium", Kurt Dekker)        0x401000 - ~0x42C000   |
|   unoptimised C; one .c file per subsystem                   |
+--------------------------------------------------------------+
| Interplay libraries                   ~0x42C000 - ~0x44B000  |
|   GNW95: window, input queue, background processes, DDraw,   |
|          DInput, fonts, palette (color.c), memory (memory.c) |
|   sound: DirectSound streaming (in the middle of game range) |
|   net transport: IPX via Winsock                             |
+--------------------------------------------------------------+
| Third-party / runtime                 ~0x44B000 - 0x458000   |
|   serial comm library (Win32 Comm API), Watcom C runtime     |
+--------------------------------------------------------------+
| Windows: DDRAW, DSOUND, DINPUT, WINMM, WSOCK32, USER32       |
+--------------------------------------------------------------+
```

Confidence in the layer split: HIGH for the game/library boundary (source-file strings stop at `pcx.c` ≈ `0x429DE7`; GNW strings and optimised code follow), MEDIUM for exact boundaries.

## 2. Module map

Each game `.c` file passes its own name to a debug allocator. Functions referencing that string therefore belong to that file, and because the linker keeps object files contiguous, the references bracket each module. Ranges are "first to last function that references the name", so true module bounds are slightly wider.

| Source file | Referencing functions span | Role | Confidence |
|---|---|---|---|
| `campaign.c` | `0x401010` – `0x401085` | Campaign (`.cam`) loading | HIGH |
| `aliens.c` | `0x40179F` – `0x401886` | Campaign enemies: "ghost", "rover" | HIGH |
| `scheme.c` | `0x403A9C` – `0x4046CC` | `.sch` load/save, scheme selection | HIGH |
| `extra.c` | `0x404BE9` – `0x404D16` | Level extras: arrows, warp holes, conveyors, trampolines | HIGH |
| `options.c` | `0x406AA3` – `0x407582` | `options.ini`, options/key-config screens | HIGH |
| `search.c` | `0x4091C9` – `0x409264` | AI path search | MEDIUM |
| `ai.c` | `0x409C1F` – `0x40A1C6` | Computer players | HIGH |
| `net1.c` | `0x40C22F` – `0x40F623` | Game-level network protocol, nodes, critical packets | HIGH |
| `misc.c` | `0x4121FF` – `0x414173` | Value/message tables, path builder, help viewer, fatal exit | HIGH |
| `graf.c` | `0x4148E5` – `0x41696C` | Screen, sprites, text printing | HIGH |
| `sound.c` (library) | ≈ `0x418A10` – `0x41B961` | DirectSound streaming | HIGH |
| `datafile.c` | `0x41BE63` – `0x41BFF5` | Chunked file reading | MEDIUM |
| `ani.c` | `0x41C1AB` – `0x41D826` | `.ani` loading, sequence lookup | HIGH |
| `you.c` | `0x41DBFE`? – `0x4213E0` | Players ("you") | HIGH |
| `bombs.c` | `0x422B0F` – `0x422C7A` (code to ≈ `0x424D00`) | Bombs | HIGH |
| `power.c` | `0x424DFE` – `0x424F0B` | Powerups | HIGH |
| `map.c` | `0x42641F` – `0x42647A` | Playfield grid, bricks, closing-in walls | HIGH |
| `flame.c` | `0x426818` – `0x426C88` | Explosions | HIGH |
| `tunes.c` | `0x42717D` – `0x428CE3` | Sound list, sound cache, music selection | HIGH |
| joystick | ≈ `0x429520` – `0x429A61` | WinMM joystick | HIGH |
| `pcx.c` | `0x429DE7` | PCX decode | HIGH |
| main/menus | `0x42A015` – `0x42BE22` | Match loop, main menu, `Game_Main` | HIGH |

## 3. Control flow

```text
WinMain 0x443C70
 └─ Game_Main 0x42BE22
     ├─ Game_InitAll 0x41095A
     ├─ intro screens 0x42B060                      (MEDIUM)
     └─ Menu_MainMenuLoop 0x42B9CE  (7 items, loops forever)
          0 → Match_Run          local game
          1 → 0x42B0CE → Match_Run   net game A (start or join; which is UNKNOWN-009)
          2 → 0x42B47D → Match_Run   net game B
          3 → 0x4080DC           options
          4 → 0x41302D           online manual / help viewer
          5 → 0x41431C           (help page)
          6 → 0x412987           exit
          idle timeout → attract/demo mode (sets 0x464938, then runs a match)
```

`Match_Run` (`0x42A3F6`):

```text
Match_PlayerSetupScreen 0x410F81      choose input type per player (10 slots)
Round_Init 0x410B6E                   pick level, load field, init all subsystems
GNW_AddBkProcess(Game_Tick)
loop:
    key = GNW_GetInput()              ← pumps messages AND runs Game_Tick
    handle ESC / pause / F-keys
    if round over:
        draw-game or winner screen, results screen ("results.plt"),
        match-victory screen ("victory%u"), then Round_Init again
    until exit state 0x464A68 != 0
GNW_RemoveBkProcess(Game_Tick)
```

## 4. Main loop and timing

Confidence: HIGH (read directly from disassembly), not yet observed at runtime.

`GNW_GetInput` (`0x43A508`):

1. `GNW_PumpMessages` (`0x43B5EC`): `PeekMessageA` / `TranslateMessage` / `DispatchMessageA`.
2. `0x43A56C`: `GNW_RunBkProcesses` (`0x43A6A4`) walks a linked list at `0x4A37A8` of `{flags, fn, next}` nodes and calls each `fn`; then polls keyboard/mouse into a 40-entry ring buffer.
3. Returns the next queued key, or −1.

`Game_Tick` (`0x42A191`), executed once per `GNW_GetInput` call while not paused:

```text
if paused (0x4646AC) return
now = timeGetTime()
frame_ms (0x464958) = now - last (0x46496C);  last = now
frame_ms = min(frame_ms, Value_Get(31))        ; 150 ms cap
net: detect lost connection; receive (0x40E765)
frame_counter (0x464994) ++
0x4105D2   draw round timer ("infinity", "numeric font")
0x415CA4   graf: begin frame / restore background      (MEDIUM)
0x42641F   map update
0x4056CA   extras update (conveyors, warps, ...)
0x4245B9   bombs update                                (MEDIUM)
0x424F89   powerups update
0x41B961   sound service
0x426D06   flames update                               (MEDIUM)
0x426818   flames (second pass)                        (MEDIUM)
0x420F07   Players_Update (10 × Player update 0x41F29B)
0x4016DA   aliens update (campaign only)
0x42459A   bombs (second pass)                         (MEDIUM)
"hurry" banner when time left < Value_Get(101)
0x429F1A, 0x415ED1   draw sprites                      (MEDIUM)
0x415C1F   present frame
Net_Pump 0x40EA1E   send / retransmit
```

Timing model:

- **Variable timestep.** Each subsystem scales by `frame_ms` (the global is read at 34 sites). Simulation and rendering are one step; there is no interpolation and no frame limiter in this path.
- Gameplay values are authored in **standard frames**: `0x46494C = 1000 / Value_Get(30) = 50 ms`. A value of N frames means N × 50 ms; a speed of S means S/100 pixels per 50 ms.
- A single step never exceeds 150 ms of game time.
- Clock source: `timeGetTime` only. `timeSetEvent` is used by the sound library, not by gameplay.

The per-tick order with confirmed roles, and the exact accumulator rules, are now in `game-loop.md` (session 2). The callee roles marked MEDIUM in the listing above have since been confirmed for bombs, flames, enclosement and players.

## 5. Subsystems

| Subsystem | Entry points | Notes |
|---|---|---|
| Window / platform | `WinMain`, wndproc `0x443E3C`, GNW init `0x43E5CC`, `GNW_Shutdown 0x43C668` | Class "GNW95 Class", title "Atomic Bomberman" |
| Graphics | `DDraw_Init 0x443038`, graf init `0x414DF4`, present `0x415C1F`, print `0x41696C` | 640×480, 8-bit, software sprite blits into a back buffer (MEDIUM) |
| Animation | ani load `0x41D695`, find sequence by name `0x41D957`, sequence → frame `0x41DAA7`, draw frame `0x415920` | All sprites come from `.ani` |
| Audio | `Sound_InitDirectSound 0x418FF7`, play sound by id `0x427961`, play tune `0x42741E` / `0x427BFB` | Music is also `.rss` (strings: `ACTUALLY PLAYING TUNE '%s'`) (MEDIUM) |
| Input | `GNW_GetInput`, per-player input `0x4102B7` (menu keys incl. joystick), `Joy_Poll 0x429790` | Player input type per slot: OFF / keyboard / joystick / AI / NET (strings) |
| Network | `net1.c`, `Net_Pump 0x40EA1E`, `Maybe_Net_GetRole 0x40C06A` (returns 0, 1 or 2), `Net_IpxOpenSocket 0x43BDDE`, serial library ≈ `0x44C9xx–0x44Dxxx` | Reliable "critical" packets with resend timeout from `valuelst.res` 1100-1104 |
| Game data tables | `Value_LoadList 0x4121FF`, `Value_Get 0x412135`, messages `0x4124A4` | |
| Players | `Players_Update 0x420F07`, per-player `0x41F29B`, init `0x4214BC` | 10 players |
| Bombs | init `0x422D3B`, update `0x4245B9` | regular / trigger / jelly / dud |
| Flames | init `0x426CDB`, update `0x426D06` | pieces: center, mid*, tip* × 4 directions |
| Powerups | init `0x424F5E`, update `0x424F89` | 13 types |
| Map | init `0x4258E5` / `0x4260F5`, update `0x42641F` | 15 × 11 cells |
| Extras | init `0x40551F`, update `0x4056CA` | arrows, warps, conveyors, trampolines |
| AI | `ai.c`, `search.c` | |
| Campaign | `campaign.c`, `aliens.c` | ghosts and rovers |

## 6. Game state and globals (first pass)

All in `.bss`. Do not reproduce this layout in the modern code; it is recorded as evidence.

| Address | Type | Purpose | Confidence |
|---|---|---|---|
| `0x461BC4` | `OBJ[10]`, 152 bytes each | Player array (`imul idx,0x98` + base, loop to 10) | HIGH |
| `0x464958` | u32 | Milliseconds advanced this frame (clamped) | HIGH |
| `0x46496C` | u32 | `timeGetTime` at previous tick | HIGH |
| `0x46494C` | u32 | Milliseconds per standard frame (1000 / value 30 = 50) | HIGH |
| `0x464994` | u32 | Tick counter | HIGH |
| `0x4646AC` | bool | Tick paused | HIGH |
| `0x464A68` | int | Match exit state (0 running, 2 abort, 10 …) | MEDIUM |
| `0x464A70`, `0x464A6C` | int | Screen width 640, height 480 | HIGH |
| `0x464938` | bool | Attract/demo mode active | MEDIUM |
| `0x46499C` | int | Current level (theme) number, used in `field%u` | MEDIUM |
| `0x464998` | int | Chosen level, negative = random | MEDIUM |
| `0x4642A0` | u32 | Rounds played counter | LOW |
| `0x46492C` | int | Local/focus player index (−1 = none) | LOW |
| `0x46489C` | bool | Campaign mode | LOW |
| `0x464984` | bool | "Hurry" sound already played | MEDIUM |
| `0x4A37A8` | ptr | GNW background-process list head | HIGH |
| `0x45C4B0` | ptr | `IDirectDraw*` | HIGH |
| `0x4A37F4` | SOCKET | IPX socket | HIGH |
| `0x45C4D4` | HINSTANCE | From WinMain | HIGH |

### OBJ structure

`sizeof(OBJ) = 152` (logged by the game itself). The structure is shared by players, bombs and flames. The layout recovered so far is in `structures.md`.

Correction (session 2): the first pass described `+0x68`/`+0x6A` as a 16.16 fixed-point position. That was wrong. Positions are integer pixels at `+0x1C`/`+0x20`; the "read dword, shift right 16" pattern is how the compiler sign-extends a 16-bit field located two bytes later.

## 7. Logical model for the modern implementation (preliminary)

```text
Match  ── rounds, wins needed, team play, options
  Round ── level theme (0-10), scheme (15×11 grid, spawns, powerup rules), timer, hurry/enclosement
    Players[10]  input source: keyboard / joystick / AI / network
    Bombs        regular, trigger, jelly, dud; kicked / punched / grabbed
    Flames
    Powerups[13 types]
    Extras       arrows, warps, conveyors, trampolines
    Campaign enemies
```

This is a reading aid, not a specification. Nothing here should be implemented before the behaviour is specified.
