# Reverse Engineering Journal

Append-only. Newest entries at the bottom. Addresses are VAs in `bm95.exe` (SHA-256 `d1ff5f6f…6b3857`).

## 2026-10-04 — Session 1: reconnaissance

All findings in this session are static (validation Level 1). The original was not executed.

### Tooling notes

- Ghidra MCP is connected to project `atomic-bomberman-claude-ghidra`, program `/bm95.exe` (1307 functions, already auto-analysed).
- `run_script_inline` is disabled on the MCP server (`GHIDRA_MCP_ALLOW_SCRIPTS` not set). Bulk export of decompilation was therefore not possible; work was done with individual MCP calls.
- Because Watcom's register calling convention makes Ghidra's decompiler output very noisy (hundreds of `extraout_ECX_n` locals), a plain `objdump -d -M intel` listing plus a small Python cross-reference index (call targets, string references, import users) was used alongside Ghidra. Game code is unoptimised, so the assembly reads almost like source.
- No Wine or Windows VM on this machine: dynamic analysis is blocked (UNKNOWN-001).

### Discovery: main executable and toolchain

- `bm95.exe`, PE32 i386 GUI, linked 1997-07-12, Watcom C/C++32 runtime (copyright string 1988-1995), sections `BEGTEXT`/`DGROUP`.
- Imports DDRAW, DSOUND, DINPUT, WINMM, WSOCK32 (ordinals), USER32, KERNEL32 (incl. Comm API), GDI32.
- Confidence: CONFIRMED.

### Discovery: source module map

- 22 source-file name strings (`bombs.c`, `flame.c`, `you.c`, `net1.c`, …) are passed to a debug allocator. Mapping each string's referencing functions gives an address range per source file. Table in `architecture.md` §2.
- Confidence: HIGH.
- Implication: any function can be attributed to a subsystem by address before it is understood.

### Discovery: startup path

- `entry 0x44BDCC` → `WinMain 0x443C70` → `Game_Main 0x42BE22` → `Game_InitAll 0x41095A`, intro `0x42B060`, `Menu_MainMenuLoop 0x42B9CE`, `GNW_Shutdown 0x43C668`.
- WinMain creates mutex `GNW95MUTEX`, registers class `GNW95 Class` (wndproc `0x443E3C`), checks for Win95/NT4.
- Confidence: CONFIRMED (decompiled).
- The engine layer is Interplay's GNW library (window class name, `win_init()` string).

### Discovery: main loop is a background process

- Screens call `GNW_GetInput 0x43A508`, which pumps Windows messages and then runs all registered background processes (`GNW_RunBkProcesses 0x43A6A4`, list head `0x4A37A8`).
- `Match_Run 0x42A3F6` registers `Game_Tick 0x42A191` with `GNW_AddBkProcess 0x43A6FC` (`mov eax,0x42a191; call 0x43a6fc` at `0x42A457`) and removes it at `0x42B04D`.
- `Game_Tick` performs the whole simulate + draw + present + network step.
- Confidence: HIGH.
- Next: confirm at runtime; confirm identity of each callee.

### Discovery: timing mechanism

- `Game_Tick` computes `frame_ms = timeGetTime() - last`, clamps to `Value_Get(31)` (= 150), stores in `0x464958`.
- `Game_InitAll` computes `0x46494C = 1000 / Value_Get(30)` (= 50 ms per standard frame) at `0x410B36`.
- `valuelst.res` comments (first-party): speeds are 1/100 pixel per frame; value 25 is the "nominal frame rate used as a reference"; value 31 "prevents a disk hit from moving everybody a whole huge distance".
- Conclusion: variable timestep in milliseconds, authored in 20 Hz frame units, 150 ms cap, no frame limiter found in the match path.
- Confidence: HIGH (static).
- Modern implication: bomb fuse (value 41 = 40 frames) is 2000 ms of game time, not 40 render frames.

### Discovery: valuelst.res is the tuning table

- `data/res/valuelst.res` is a commented text file of `id,value` pairs loaded by `Value_LoadList 0x4121FF` and read through `Value_Get 0x412135` (bounds-checked: `invalid valueno requested: %u`).
- Contains fuse length, speeds, starting inventory, powerup counts and caps, start positions, round time, hurry time, disease rules, net resend timeouts, UI coordinates.
- Confidence: CONFIRMED as data. How each value is applied in code: mostly untraced.

### Discovery: graphics

- `DDraw_Init 0x443038`: `DirectDrawCreate`, then vtable +0x50 (`SetCooperativeLevel`), +0x54 (`SetDisplayMode`), +0x18 (`CreateSurface`), +0x14 (`CreatePalette`).
- Graf init `0x414DF4` stores 640 and 480 to `0x464A70`/`0x464A6C`, loads `color.pal`, creates GNW window "win1".
- Confidence: HIGH for 640×480; MEDIUM for 8-bit palettised.

### Discovery: audio

- `Sound_InitDirectSound 0x418FF7` (`DirectSoundCreate`, primary buffer `SetFormat`). Error table uses HMI SOS wording, so the library is an Interplay DirectSound port of an older DOS sound layer.
- Sounds: 2027 `.rss` files, raw 22 kHz stereo 16-bit signed LE according to `soundlst.res`. `tunes.c` manages a cache (values 5-9) and can copy sounds from CD to disk ("stage up").
- Confidence: HIGH.

### Discovery: input

- Keyboard hook installed at `0x43B44C` (`SetWindowsHookExA`, also `timeBeginPeriod`), hook proc `0x43B518` uses `GetAsyncKeyState`.
- `DInput_Init 0x444760` calls `DirectInputCreateA`.
- Joystick: `joyGetNumDevs` `0x42971F`, `joyGetDevCapsA` `0x42965C`, `Joy_Poll 0x429790` (`joyGetPosEx`).
- Confidence: HIGH that these exist; which keyboard path gameplay uses is open (UNKNOWN-004).

### Discovery: networking

- `Net_IpxOpenSocket 0x43BDDE`: `socket(6, 2, 1000)` = AF_IPX, SOCK_DGRAM, NSPROTO_IPX; then `bind`, `getsockname`, `setsockopt`, `ioctlsocket`. Send/receive use `sendto`/`recvfrom` (`0x43B99C`, `0x43BFC9`).
- Serial and modem use the Win32 Comm API through a bundled comm library (`\\.\COM%d`, `ATS0=0`, baud table 9600-115200).
- `net1.c` implements nodes and "critical" packets that are retransmitted after a timeout (`valuelst.res` 1100-1104).
- `valuelst.res` lists four protocols: IPX, modem, serial, TCP/IP. No AF_INET socket creation was found.
- Confidence: HIGH (IPX, serial). TCP/IP: UNKNOWN-005.

### Discovery: resource system and formats

- Loose files under `data/`; path builder `Path_BuildResourceName 0x411D17`; roots from `cfg.ini` (`hdhome`, `cdhome`).
- Text formats (`.sch`, `.res`, `.cam`, `.ali`, `messages.txt`) are self-documenting.
- `.ani` is a chunk container (`CHFILEANI `, chunks `HEAD`/`PAL `/`TPAL`/`CBOX`/`FRAM`/`SEQ `).
- Details in `initial-analysis.md` §8.
- Confidence: HIGH for text formats and ANI container; pixel encoding not decoded.

### Discovery: players array and OBJ

- The game logs `sizeof(OBJ) = %u` with 0x98 (push at `0x410B23`).
- `Players_Update 0x420F07` loops 10 times over `0x461BC4 + i*0x98` and calls `0x41F29B` per player.
- Position appears to be 16.16 fixed point at +0x68 / +0x6A.
- Confidence: HIGH (array, size, count), MEDIUM (position fields).

### Ghidra renames applied (saved)

`Game_Main`, `Game_InitAll`, `Game_Tick`, `Game_SetTickPaused`, `Game_FatalExit`, `Menu_MainMenuLoop`, `Match_Run`, `Match_PlayerSetupScreen`, `Round_Init`, `Players_Update`, `Time_GetMs`, `Value_Get`, `Value_LoadList`, `Path_BuildResourceName`, `Debug_Printf`, `GNW_GetInput`, `GNW_PumpMessages`, `GNW_RunBkProcesses`, `GNW_AddBkProcess`, `GNW_RemoveBkProcess`, `GNW_Shutdown`, `DDraw_Init`, `DInput_Init`, `Sound_InitDirectSound`, `Joy_Poll`, `Net_Pump`, `Net_IpxOpenSocket`, `Maybe_Net_GetRole`.

Not renamed on purpose: the `Game_Tick` callees (bombs/flames/powerups/map/extras updates). Their identity rests on address range only; rename after reading them.

### Next

1. Read each `Game_Tick` callee and confirm its role; rename.
2. Recover the `OBJ` layout from `0x41F29B` (player update) and the bomb/flame code.
3. Set up a way to run the original (UNKNOWN-001).

## 2026-10-04 — Session 2: tick callees, OBJ layout, core mechanics

Still static only (Level 1).

### Tooling

- Ghidra was restarted with `GHIDRA_MCP_ALLOW_SCRIPTS=1`. A script dumped all 1178 function bodies (decompiled) and the function table to the session scratch directory; reading was done from that dump with the `extraout_*` noise filtered out, cross-checked against the disassembly where register arguments were lost (notably `Value_Get` ids).
- Inline scripts leave copies in `~/ghidra_scripts/` (`McpScriptCheck.java`, `DumpAllBm95.java`, `RenameSession2.java`).

### Correction: positions are integer pixels

- Session 1 recorded player position as 16.16 fixed point at `+0x68/+0x6A`. Wrong.
- Evidence: `Players_InitRound` writes `Map_CellToPixelX/Y` results to `+0x14/+0x18`; `Player_MoveSteps` adds ±1 to `+0x1C/+0x20`; the `mov eax,[obj+N]; sar eax,16` pattern is a sign-extending read of the i16 at `N+2`.
- Confidence: HIGH.

### Discovery: time-to-progress rules (resolves UNKNOWN-002)

- Timers: `acc += frame_ms; while acc > 0 { frame++; acc -= 50 }`.
- Movement: `acc += speed * frame_ms / 50; while acc > 0 { one pixel step; acc -= 100 }`.
- Seen identically in `Player_Update`, `Bombs_Update`, `Flames_Update`.
- Confidence: HIGH.
- Implication: behaviour is defined per pixel step and per millisecond, so a fixed-step modern simulation can reuse the same rules.

### Discovery: map geometry

- 15 × 11 cells of 40 × 36 px, origin (20, 68); tiles 0/1/2 = blank/solid/brick; outside = solid.
- Constants written at `0x426488`–`0x4264EB`; conversions `0x426524`, `0x42655F`, `0x42665C`, `0x4266A3`, `0x426599`, `0x4265EB`.
- Confidence: HIGH. Documented in `maps.md`.

### Discovery: bomb fuse, detonation, chain reactions

- `Bomb_Create 0x422EDE`: `fuse_ms = frames × 50`; dud chance; range byte; owner.
- `Bombs_Update 0x42331C`: fuse pauses for trigger bombs, duds, flying/held bombs, network bombs, and when ≤ 1 contender is left.
- Blast: per direction, up to `range` cells; stops at bomb (queued), powerup (destroyed), solid, brick (burns).
- Chain reactions go through a queue (`Bomb_QueueDetonation 0x423209`) and fire on the next tick; no flame is sent back toward the triggering bomb; kill credit moves to the triggering owner.
- Confidence: HIGH. Documented in `bombs.md`, `explosions.md`.

### Discovery: flames are per-cell and kill by cell

- One flame slot per cell (`0x46224C`). Lifetime 10 frames = 500 ms (values 10 and 20, ids confirmed in the disassembly at `0x426E62` and `0x426DDB`).
- A player dies when the cell under its position has an active flame; checked at the start of the player update and after every pixel moved.
- Confidence: HIGH.

### Discovery: player movement and collision

- `Player_MoveSteps 0x41EC84` fully read: point-based movement along cell centre lines with diagonal lane alignment, corner sliding when blocked off-centre, kick trigger at the cell centre, warp/trampoline one pixel before the centre.
- Speed formula with skates (value 90), a speed-down counter (value 91) and disease multipliers.
- Confidence: HIGH for the step rules. Documented in `players.md`.

### Discovery: enclosement

- `Map_UpdateEnclosement 0x426818`: inward clockwise spiral from (0,0), one cell per 250 ms of **wall-clock** time, max 4 per tick; turns tiles solid, kills players, removes powerups and flames, detonates or removes bombs.
- Confidence: MEDIUM-HIGH. Start condition MEDIUM.

### Ghidra renames applied (saved)

48 functions: `Bombs_Update`, `Bombs_UpdateFree`, `Bombs_UpdateHeld`, `Bomb_Create`, `Bomb_QueueDetonation`, `Bomb_FindAtCell`, `Maybe_Bomb_Kick`, `Maybe_Bomb_Remove`, `Map_GetTile`, `Maybe_Map_SetTile`, `Maybe_Map_StartBrickDestroy`, `Map_CellToPixelX/Y`, `Map_PixelToCellX/Y`, `Map_PixelOffsetInCellX/Y`, `Map_UpdateEnclosement`, `Flame_Create`, `Flame_FindAtCell`, `Flames_Update`, `Maybe_Flame_Remove`, `Powerup_FindAtCell`, `Maybe_Powerup_Remove`, `Maybe_Powerup_RevealAtCell`, `Powerups_Update`, `Player_Update`, `Player_MoveSteps`, `Player_IsCellPassable`, `Player_DropBomb`, `Player_Kill`, `Player_PickupPowerup`, `Maybe_Player_ReadInput`, `Player_FindAtCell`, `Players_InitRound`, `Players_GetContendersLeft`, `Maybe_Round_GetTimeLeft`, `Extras_Update`, `Extra_FindAtCell`, `Maybe_AI_MarkDangerCell`, `Ani_FindSequence`, `Ani_GetSequenceFrame`, `Ani_GetSequenceLength`, `Maybe_Graf_QueueSprite`, `Sound_PlayById`.

`Maybe_` marks names based on call context only; the body was not read.

### Next

1. Finish `Player_Update` (second half) and read `Maybe_Player_ReadInput 0x41E61E`: bomb drop conditions, punch, grab/throw, trigger detonation.
2. `power.c`: powerup placement under bricks, pickup effects, caps, diseases list.
3. Then write `docs/specifications/` for map, movement, bombs, explosions; these four are understood well enough.
4. Dynamic validation remains blocked (UNKNOWN-001).

## 2026-10-04 — Session 3: player input and actions, powerups, round generation

Static only (Level 1).

### Discovery: controller types and direction priority

- `Player_ReadInput 0x41E61E`: 0 off, 1 AI, 2 keyboard (two key sets), 3 joystick (thresholds 30/70 of 100), 4 network (state copied from packets).
- Several directions held: blocked ones are dropped if any is open; then the highest direction index wins (W > S > E > N).
- Confidence: HIGH.

### Discovery: action rules

- Button 1 (edge): grab own bomb underfoot → else spooge line → else drop if `active < capacity`, cell passable and no warp.
- Button 2 (edge): stop own sliding bombs (kicker), punch bomb ahead (punch, only if button 1 not held), detonate oldest trigger bomb.
- Active bombs are counted by scanning the bomb array by owner (`0x4245DA`).
- Held bomb is thrown when button 1 is released.
- Confidence: HIGH. Documented in `players.md`.

### Discovery: powerup table, generation, reveal, pickup, diseases

- 14 types named in the exe (`bomb … random, clog`); counts from values 400-412 (negative = that many 1-in-10 attempts); always under bricks.
- Early-round protection swaps punch/grab/disease3 away when revealed.
- Pickup is cell-based; caps from values 550-564; mutual exclusions (punch↔trigger, grab↔spooge, trigger↔jelly).
- 9 diseases, 15 s each, shared timer; effects identified at their points of use.
- Confidence: HIGH for mechanics, MEDIUM-HIGH for disease table. Documented in `powerups.md`.

### Discovery: round generation

- Brick kept if `rand() % 100 < density` (`0x4262FF`); start cell and its non-solid neighbours cleared.
- Confidence: HIGH. Documented in `maps.md`.

### Observation: tick-counted behaviours

- Stun (16), disease pass cooldown (value 129), chain-reaction links are per tick, not per millisecond. Recorded as UNKNOWN-020 because it ties gameplay feel to the original's real frame rate.

### Ghidra renames applied (saved)

23 functions, including `Player_ReadInput`, `Bombs_CountOwnedBy`, `Bombs_StopKickedBy`, `Player_PunchBombAhead`, `Bombs_DetonateOldestTrigger`, `Bomb_AttachToHolder`, `Bomb_Launch`, `Bomb_Kick`, `Bomb_CanSlideInto`, `Player_HitOnHead`, `Powerup_RevealAtCell`, `Powerup_Remove`, `Powerup_RespawnOnRandomCell`, `Powerup_PlaceRevealed`, `Powerups_GenerateForRound`, `Map_GenerateForRound`, `Map_ClearStartArea`, `Map_SetTileRaw`, `Scheme_GetCell`.

### Next

Write the behavioural specifications for map, physics, players, bombs, explosions and powerups from the reverse-engineering notes.

### Session 3 addendum: specifications written, three rules re-checked

- `docs/specifications/` created: `README.md`, `gameplay.md`, `maps.md`, `physics.md`, `players.md`, `bombs.md`, `explosions.md`, `powerups.md`. Each rule carries a tag ([S] code, [D] data, [M] medium, [?] open).
- Bomb type selection read at `0x41EB2A`: jelly takes precedence; trigger bombs are limited to "bomb capacity" bombs per trigger pickup (`OBJ+0x55`). Confidence: HIGH.
- Spooge: `Player_DropBomb` receives the line index in ECX and passes it as the fuse delay, so bomb n starts n frames behind (`0x420B48`-`0x420B5A`). Confidence: HIGH.
- Corner-slide direction verified for the eastward case in `Player_MoveSteps`. Confidence: HIGH.
- Dud: only value 323 is read in `Bombs_Update`; no reader of value 324 was found in the bomb code. Confidence: MEDIUM.
- Not specified yet: game modes, networking, AI, level extras.

### Session 3 addendum 2: four unknowns closed

- Round clock (`0x4105D2`): real-time milliseconds; `0x4601A4` seconds left, `0x4601BC` seconds elapsed, `0x4601A8` total (1001 = unlimited). Confidence: HIGH.
- Closing walls start at 55 s left (`0x42685D`). Confidence: HIGH.
- Early-round powerup protection lasts the first 40 s. Confidence: HIGH.
- Invulnerability and kill scoring are in `0x41DCB2`. Confidence: HIGH.
- No frame limiter or vsync anywhere in the present path; the DirectDraw layer only locks and unlocks the surface. Confidence: HIGH (static).

### Session 3 addendum 3: .ANI format decoded, extractor tool

- Container, FRAM/CIMG header, RLE (encoding 0x11) and SEQ/STAT frame references recovered from `0x41C837` and `0x41C0BA` and from the data.
- Implemented in `tools/asset-extractor/ani.py` and `ani_extract.py` (new code, Python + Pillow).
- Validation: 94 of 95 files decode (2299 frames, 235 sequences); bomb and walk sheets inspected visually and are correct. `classics.ani` uses a type the game itself rejects.
- Hot-spot of full-cell frames is (20, 35), matching the cell anchor used by the map code.
- Confidence: HIGH. This is the first finding validated by something other than reading code: the decoder's output is a recognisable image.
- Documented in `file-formats.md`.

### Session 3 addendum 4: scheme overrides, synced values

- Scheme application writes born-with into values 50+type and overrides into 400+type via `Value_Set 0x4121BF` (`0x404679`-`0x4046BC`). Forbidden is only consulted by the "random" powerup. Confidence: HIGH.
- In a network game the host sends 79 value ids (table at `0x45B7D4`, count 0x4F at `0x40478B`) to the clients. `valuelst.res` contains exactly 79 lines marked `; PGT`, so that marker almost certainly flags the values that are synchronised. Confidence: MEDIUM-HIGH (count match; table contents not compared).
- Renamed: `Value_Set`, `Player_StartDying`, `Round_GetTimeLeft`.

## 2026-10-04 — Session 4: gameplay core started

### Decision: begin the modern gameplay core

- The behavioural specification now covers loop, timing, map, movement, bombs, explosions and powerups (AGENTS.md §33 milestone). Implementation of the SDL-independent core began; no SDL/OpenGL code was written (SDL3 is not installed on this machine).
- `src/game/` (`geometry.hpp`, `values.*`, `world.*`), `tests/unit/test_core.cpp`, `CMakeLists.txt`. Builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` without warnings; 29 tests, 637 checks, all passing.

### Discovery made while implementing: the contender window

- `Player_Update` keeps counting a dying player as alive until its death animation frame reaches value 25 (20 frames). The developer comment on value 25 reads "how many frames do you have to out-survive the other guy".
- Consequence: bombs keep ticking for 1 s after the second-to-last player dies, and a draw results if the last player dies within that second.
- The frame counter advances in the tick of death itself (the code jumps to the dying branch immediately).
- Confidence: HIGH. Added to `docs/specifications/gameplay.md`.

### Correction: bomb type precedence

- `Player_DropBomb` sets jelly first and then overwrites with trigger, so trigger wins when both are held. The specification had it the other way round. Fixed.

## 2026-10-04 — Session 5: the original runs; first dynamic confirmations

- Wine 10 and SDL3 were installed by the user. The original runs under Wine from `work/run` (copy; `game/` untouched). Details and all observations: `dynamic-analysis.md`.
- Blocker found and solved: without `options.ini` the game waits at a memory-configuration screen (`0x406086`).
- CONFIRMED at runtime: fullscreen exclusive 640 × 480 × 8 DirectDraw with Lock/Unlock presentation; `sizeof(OBJ) = 152`; `.plt` → `.pcx` name mapping; logo timing (value 12); attract mode after 30 s (value 92).
- **CONFIRMED: bomb fuse is 2.000 s**, measured on 84 bombs in an AI demo match from sound-load timestamps. This is the first gameplay rule at validation Level 2, and with the existing unit test it reaches Level 4.
- CONFIRMED: no frame limiter. About 960 presents per second on this machine. Consequence for tick-counted rules recorded under UNKNOWN-020; recommendation is a fixed 50 ms simulation step.
- Next: scripted input and screen capture for the remaining test-matrix scenarios; SDL3/OpenGL front end.

### Session 5 addendum: first front end

- `src/app/main.cpp` (SDL3), `src/rendering/renderer.*` (OpenGL 3.3 core profile), `src/resources/scheme_file.*` (original `.sch` reader, unit-tested).
- Fixed 50 ms simulation step with render interpolation.
- Verified by automated runs: `--demo --frames N --screenshot` produces frames showing the `basic` scheme at 90 % density, cleared start areas, players, a bomb, flames, burning bricks and revealed powerups.
- Reads `valuelst.res` and schemes from the user's game directory at run time; draws placeholder shapes, not original art.
- Not yet done: interactive play has not been tried by a person; no audio, menus, round end handling or original graphics.

## 2026-10-04 — Session 6: scripted control of the original

- `xdotool` installed by the user. Key injection works; the game's screenshot key is Alt+C (found in the code after F12 did nothing). New tools: `drive_original.py`, `sprite_shift.py`.
- Menu structure, pre-game screen order, controller cycling and default keys observed (D13-D16).
- **Measured on the original**: walking speed ≈ 183 px/s (D17), fuse 2.000 s with a controlled bomb (D18), flame lifetime ≈ 500 ms (D19), blast shape and edge stop (D20).
- Correction: `Match_PlayerSetupScreen` (player list) comes before the level/scheme screen, not after.
- Validation level: fuse, flame lifetime, walking speed and basic blast propagation are now observed on the original, reproduced by the modern core and covered by unit tests (Level 4).

## 2026-10-04 — Session 7: original graphics in the modern renderer

- C++ readers for `.ani`, PCX and `color.pal`/`.rmp` (`src/resources/ani_file.*`), sprite textures built at run time (`src/rendering/sprites.*`), renderer draws background, tiles, powerups, flames, bombs and players with the original artwork.
- Correction to the ANI format: the size field in the CIMG sub-header includes the 12-byte sub-header. The Python tool had been lenient about it; the C++ reader rejected every frame until this was fixed.
- Palette model confirmed (UNKNOWN-012) and colour remap understood (UNKNOWN-021): only palette indices 100-174 are remapped.
- Pixel comparison with the original's screenshot: 99.46 % identical playfield, sprite position exact. Sequence step offsets are not draw offsets.
- Tests: 31 tests, 664 checks, all passing (new: ANI parser on a synthetic file).

### Session 7 addendum: more rules observed on the original

- Direction priority, chain reaction, kick speed and stop, born-with from schemes, hurry timing and the closing-wall spiral and cadence were all observed with scripted runs (D23-D28 in `dynamic-analysis.md`). Every prediction tested so far has held.
- Sound names learned: `bmdrop*` drop, `explo*`/`bomb_*` explosion, `kicker*` kick, `bmbstop*` sliding bomb stops, `sqrdrop*` wall block, `zai01c` hurry voice.

### Session 7 addendum 2: clock, closing walls and round result in the modern core

- `World`: round clock (`secondsLeft`), `hurry`, closing walls with the original's cursor logic (including the repeated corner cell, which the 16-blocks-at-18.03 s and 36-blocks-at-23.21 s screenshots of the original confirm), round result (`roundOver`, `winner`).
- Front end: seven-segment clock and player pips as a stand-in HUD; a decided round restarts after three seconds.
- Tests: 34 tests, 700 checks, all passing.
- Open: behaviour of the original when the clock reaches 0:00 (UNKNOWN-022).

## 2026-10-04 — Session 8: punch, grab, spooge

- Core: flying bombs (three-cell first hop, one-cell bounces, wrap-around, landing rules), punch, grab/carry/throw, spooge lines, head-hit stun with powerup loss. Renderer draws the flight arc and the carried bomb.
- Code detail: the hop counter comparison at `0x423961`-`0x423983` compares a cell index with a pixel coordinate saved at launch, so it is always "different" and the counter advances at every cell centre. The landing attempt therefore starts at the third centre; the original's behaviour matches (D31).
- Observed on the original: punch (D31), grab and throw (D32), spooge line (D33). A 3 s "stall" seen in the first punch run was my own screenshot polling, not the game.
- Tests: 38 tests, 734 checks, all passing.

### Session 8 addendum: events and audio

- Core records gameplay events per tick; the application maps them to the original's sound id ranges and plays the `.rss` files through SDL3 (`src/audio/`).
- Checked: device opens, 1051 names parsed (matches the original's own log line), no missing files in a scripted round. Not checked: how it sounds.
- Tests: 39 tests, 740 checks.

## 2026-10-04 — Session 9: diseases, animations, themes

- Core: nine diseases with shared timer, proximity spreading, cure roll on pickup; death animation number chosen at death; kick/punch action state for drawing.
- Renderer: `die green N`, `kick`, `punch`, `walkbomb`/`standbomb`; level theme selectable (`--level 0-10`); application counts round wins per match (`--wins`).
- Tests: 41 tests, 758 checks, all passing. Level 3 rendered and checked by eye.
- Not validated against the original: diseases (they are random there and cannot be forced from a scheme) and all animations.

### Session 9 addendum: AI

- The original's AI is a priority list of eight behaviour functions reached only through a pointer table at `0x45BA78`; they were missing from Ghidra's function list and were created by script. Structure, order, probabilities and the danger map are documented in `ai.md`. Confidence: HIGH for the structure and for behaviours 1, 2, 4, 5, 8; MEDIUM for 3, 6, 7 (their search routines were not read).
- Modern `AiPlayer` (`src/game/ai.*`) follows the same list. It is an input source; the core is unchanged.
- Measured: 200 rounds of four AIs end with a single survivor 149 times; about half of all deaths are self-inflicted.
- Tests: 42 tests, 762 checks.

## 2026-10-04 — Session 10: level extras; AI danger formula

- Bomb danger level for the AI is `100 + elapsed fuse ms` (`0x42429B`); closing walls mark cells ahead with a level falling by 10 per cell. Applied to the modern AI.
- Level extras read from `extra.c` and their users; documented in `extras.md`, specified in `specifications/maps.md`, implemented in the core (`World::setExtras`) with a reader for `extraN.res`. Rendered for levels 3 and 10 and checked by eye.
- Tests: 47 tests, 795 checks, all passing.
- Not yet observed on the original: any of the extras.

## 2026-10-04 — Session 11: fonts and HUD

- `.fon` bitmap font format decoded (header of three i32, 8 pointer bytes, width/offset table, 1-bit rows); verified by exact size match on all three fonts and by rendering. C++ reader `parseFont` with a unit test.
- The round clock uses sprite digits (`numeric font` in `kfont.ani`); the modern HUD now draws clock, hurry banner and score labels from the original assets at the original coordinates (values 110-119). Side-by-side with an original screenshot: clock and white player's label coincide.
- Tests: 48 tests, 808 checks.

### Session 11 addendum: music and menu flow

- Level music loops (sound id 1100 + level); menu music id 1010. Exercised with SDL's dummy audio driver, not listened to.
- The main menu's item texts are part of `mainmenu.pcx`; the game only draws the cursor (`cursor1` in `misc.ani`) at the coordinates of value 700. Found by rendering the three `.fon` fonts next to the original menu: none matches the menu lettering.
- Application: screen state machine (main menu → player list → match), original pictures and cursor, player list with the original's layout, cycling order and defaults. Result line overlay after each round.

### Session 12: level screen and result pictures (application)

- Added a level/scheme/wins screen after the player list (level names from messages 150-160, schemes listed from `data/schemes`), and the original's `draw`, `results` and `victoryN` pictures after a round.
- Checked: the level screen renders. NOT checked: the result pictures (no automated run reached a round end), and changing level from the screen (sprite bank reload).

### Session 12 addendum: the unverified screens are now checked

- Added `--result-shot` (capture the first decided round's result screen) and `--script` (scripted menu key presses) for automated checks; automated runs no longer wait for the display.
- Verified by capture: draw picture, round results picture with the score list, victory picture in the winner's colour (players 3 and 4), and changing level and scheme from the level screen (graphics reloaded as level 3).
- Fixed: the match-winner line was drawn over the victory artwork's title; it is now below it.

### Session 12 addendum 2: team play

- Core: `setTeamPlay`, contenders counted per team, `winningTeam`, team colours after value 32 frames (team 0 → colour 0, team 1 → colour 2, from `Players_InitRound`). AI skips team-mates. Application: toggle on the level screen, team result pictures.
- Checked by a scripted four-AI team match (two white, two red, bombs in team colours). Tests: 49 tests, 818 checks.
- `docs/specifications/game-modes.md` written.

### Session 12 addendum 3: a scripted run of the original lost keyboard focus

- A team-play run produced no screenshots: the game stayed on its main menu, so the injected keys never reached it. The Wine window did not have X keyboard focus (in earlier runs it did). The keys of that run (Enter, Down, Right, Alt+C; no text) went to whatever window had focus on the desktop.
- `drive_original.py` now checks before every key that the focused window is the Wine desktop or the game, and aborts without sending anything otherwise.
- Team-play colours on the original remain unobserved.

### Session 12 addendum 4: duds

- `0x422C13` sets the next dud time from the C library clock (seconds): now + value 320 + random(value 321). The timer is global, not per round. The dud animation is `bomb regular green dud` in `duds.ani`.
- Core: dud bombs (fuse held for value 323 frames). Tests: 50 tests, 826 checks.

## 2026-10-04 — Session 13: more scenarios on the original

- With the focus-checked driver: team colours (D38), field edge (D39), capacity (D40), bomb blocks the way back (D41), trigger bombs including the supply limit (D42), jelly bounce (D43), goldflame (D44). All as predicted.
- New lab schemes in the working copy: `testtrg`, `testjel`, `testgold`.

### Session 13 addendum: powerups and arrows on the original

- A scheme with powerup overrides (`testpow.sch`) makes every brick hide a skate, which makes pickup effects testable: reveal, pickup by cell and one-skate speed (about 212 px/s) observed (D47, D48).
- Arrows: a kicked bomb followed three arrows on level 3 at 200 px/s (D49).
- All four kinds of level extra have now been observed on the original (conveyor D37, warp D45, trampoline D46, arrow D49).

## 2026-10-04 — Session 14: network message layer (static)

- Packet framing (magic 0x536C, batched messages of length/type/sequence), reliable "critical" messages (types ≥ 32: acknowledged by type 12, resent after value 1100+protocol ms, dropped after 25 resends, de-duplicated by a 500-id ring per node), up to 5 nodes.
- 49 handlers registered through `0x40E412`; type and payload size of every sender recovered from its call to `0x40CE27`. Meanings assigned from the gameplay call sites already understood (bomb created, detonation, death, pickup, kick, punch, clock, level, values).
- Roles: 1 client, 2 host. Host owns map generation, clock, level choice, tuning values; each machine owns its players.
- Documented in `networking.md`. Nothing observed on the wire.

### Session 14 addendum: gamepads

- Application: SDL3 gamepads as controllers (`JOY n` in the player list), using the original's axis thresholds. Compiles and the program runs with no pad attached; never exercised with a real controller.
- Ghidra: 24 network and helper functions named (`Net_QueueMessage`, `Net_ReceiveAndDispatch`, `Net_Send…`, `Net_GetRole`, …).

## 2026-10-04 — Session 15: packaging

- Decision by the project owner: networking is out of scope for now.
- Portable OpenGL binding (`src/rendering/gl.*`) replaces the Mesa headers. CMake can fetch and statically link SDL3 (`-DAB_FETCH_SDL3=ON`); verified locally on Linux (361 build steps, runs).
- `atomic --import-assets SRC [--assets-dir DEST]`: builds the game's asset folder from a copy of the original (case-insensitive lookup, lower-case output, `.rss` → `.wav`). Verified: 246 data files, 971 sounds, 281 MB; the game runs from the result with graphics, extras, sounds and music.
- `.github/workflows/build.yml`: Linux and Windows packages, tests, release on `v*` tags. YAML parses; the workflow itself has not run (no GitHub remote), and the Windows build has never been compiled.
- `INSTALL.md` written.

### Discovery: end-of-round sounds

Where: `Match_Run`, `0x42A6D8` (`mov eax,0x46A; call 0x42741E`), `0x42A71C` (`mov eax,0x6A4; call 0x427BFB`), `0x42ACB9` (`mov eax,0x7D0; call 0x427BFB`).

Evidence:
- Tune 1130 (`draw`) is started for every round result, before the winner is looked up.
- No winner (lookup returns -1): random sound of the series starting at 1700 ("tie game/draw game" in `soundlst.res`).
- A winner: random sound of the series starting at 2000 ("we have a winner"). It is played for every won round, not only the last of the match.
- `0x427BFB` picks at random among the consecutive loaded ids from the given one.
- Dynamic: a drawn round under Wine loaded `draw.rss` and then `zaa55a`/`zaa59b` (ids 1716, 1720).

Confidence: HIGH

Implication: ids 320-322 (same three files as 2000-2002) are not what the result screen uses.

### Discovery: player animation states (pickup, spin, cornerhead)

Where: animation choice in `Players_Update`, dispatch on `OBJ+0x4E` at `0x42000F`.

Evidence:
- States: 0 normal, 1 kick, 2 punch, 3 stunned (`Player_HitOnHead` sets it at `0x421FA8`), 4 picking up a bomb (set at `0x420A1F` right after the grab), 6/7 warp out/in, 20-39 `cornerhead (state-20)`.
- Every state advances `OBJ+0x50` by one per 50 ms. Sequence frames are looked up modulo the sequence length (`0x41DAA7`).
- State 3 draws the ordinary sprite: the earlier note "stunned shows the pickup animation" was wrong and is corrected in `players.md`.
- State 4: input is not read while the counter ≤ value 665 = 2 (`0x41FA3B`); ends when the counter exceeds the sequence length; the frame drawn uses the walk counter / 3.
- States 6/7: `spin` frame = counter; at counter > 8 state 6 moves the player and becomes 7, state 7 ends.
- Cornerhead: ends when the counter reaches the sequence length (50 frames = 2.5 s).

Confidence: HIGH for the state table, MEDIUM for the visible result of state 4 (not observed running).

Modern: pickup state with its pause and the warp spin implemented; cornerhead still missing.

### Discovery: AI route to safety (`0x40970B`)

Evidence: full decompile; call site `0x40B38A` passes depth 20.
- Walker flood over blank, bomb-free cells; first cell with danger 0 wins; otherwise the least dangerous cell seen within 20 passes. The start cell is not a candidate. Flames do not block the search.

Confidence: HIGH

Modern: `AiPlayer::stepToSafety` now follows this rule (it used a guessed "half the current danger" rule before). Six AI-only demo rounds ran to a result afterwards. Details in `ai.md`.

### Discovery: exit sound

Where: `0x412987`. Confirmation box (message 10 "Are you sure you want to exit?"), then music off, random sound of the 2600 series, 4000 ms wait, exit. Confidence: HIGH (static). Implemented.

### Discovery: roulette ("goldman")

Where: `FUN_004034BC` (called from round setup `0x410FB8` when option `0x4648BC` is on, not in campaign), position formula `0x403382`/`0x40341F`, prize getter `0x403A9C`, prize applied in player init `0x4216DE`, sparkles `0x420D4E`/`0x420E39`.

Evidence:
- State at `0x45E024…0x45E03C`: wheel position, direction ±1, prize (-1), wheel speed (rand%20+20), state (0 spin, 1 slowing, 2 stopped), ring position, ring speed (wheel speed + rand%20).
- Circle of value 1004 × 6 positions; x = v1000 + v1002·cos(2π·pos·v1006/N), y = v1001 + v1003·sin(2π·pos·v1007/N) (constants 3.1415926 and 2.0 at `0x4580FC`).
- Slot table `0x45B7BC` = {0, 1, 3, 8, 4, 13}: bomb, flame, kicker, goldflame, skate, clog. This confirms inventory slot 13 as the clog.
- Sprites: `power <name>` for the slots, `ring` for the pointer, picture `roulette.pcx`; texts 790, 800+prize, 791; sounds 1300 tick, 1310 applause, 1320 buzzer (clog), 20 on any key.
- Keys: Enter/Space (slow down, then leave), Esc (abort the match).
- Next round: `inventory[prize]++` for the previous winner (team: members whose team colour equals the winner), no cap check. Sparkles: per update, first free of 100 slots, skipped once value 1010 seconds have passed, 5-in-6 chance; position x-20…+19, y-48…+1; sequence `goldman` (13 frames), one frame per 50 ms.

Confidence: HIGH (static). Not observed running in the original.

Modern: `src/game/roulette.*` (tested), screen in the application (`--roulette`), sparkles in the renderer.
