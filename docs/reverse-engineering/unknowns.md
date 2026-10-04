# Unknowns Register

Status values: OPEN, IN PROGRESS, RESOLVED. Addresses are VAs in `bm95.exe`.

# UNKNOWN-001

Question:
How can the original be executed and observed? Every finding so far is static.

Evidence:
No `wine` binary on the analysis machine. Ghidra MCP exposes a debugger URL (`GHIDRA_DEBUGGER_URL` in `.mcp.json`) but there is no target to attach to.

Status:
RESOLVED (sessions 5-6). The original runs under Wine, can be driven with scripted keys and captured with its own screenshot key. See `dynamic-analysis.md`, `tools/diagnostics/run-original.sh`, `tools/diagnostics/drive_original.py`. A debugger is not attached yet.

# UNKNOWN-002

Question:
How exactly is `frame_ms` (`0x464958`) converted into movement and timers? Is position advanced as `speed * frame_ms / 50`, and where is rounding done?

Evidence:
`Game_Tick 0x42A191` clamps frame time to 150 ms. `0x46494C` holds 50 (ms per standard frame) and is used in `imul`/`div` at `0x401BD0`, `0x41E13E`, `0x41F370`-`0x41F4DB`, `0x41FDA2`. Positions look like 16.16 fixed point.

Status:
RESOLVED (session 2, static). Integer pixel positions; `acc += speed * frame_ms / 50`, one pixel per 100; timers accumulate milliseconds and advance one frame per 50. See `game-loop.md`. Runtime confirmation still pending under UNKNOWN-001.

# UNKNOWN-003

Question:
What is the full layout of `OBJ` (152 bytes), and is it shared by players, bombs, flames and powerups?

Evidence:
`sizeof(OBJ) = 152` logged at `0x410B28`. Player array at `0x461BC4`. Other arrays indexed with stride 0x98: `0x462214`, `0x45E0A8`, `0x45BE98`.

Status:
IN PROGRESS. Shared by players, bombs and flames (HIGH). About 45 fields recovered in `structures.md`.

Next action:
Read `power.c` and `extra.c` to confirm powerups and extras use the same structure; fill the remaining offsets (`+0x0C`, `+0x34`, `+0x40`, `+0x48`, `+0x6A`-`+0x6C`, `+0x8B`-`+0x91`).

# UNKNOWN-004

Question:
Which keyboard path feeds gameplay: the `WH_KEYBOARD` hook, `GetAsyncKeyState`, or DirectInput? What does the DirectInput object do (keyboard, mouse, or both)?

Evidence:
Hook installed at `0x43B44C`; hook proc `0x43B518`; `DInput_Init 0x444760` with helpers `0x444A68`, `0x444AE8`, `0x444B14`, `0x444BD0`.

Status:
MOSTLY RESOLVED (session 6). Default key bindings in the exe are DirectInput scan codes and the per-player input reads a key-state table indexed by them (`0x4A2BA0`), so gameplay keys come from the DirectInput keyboard. Menu keys arrive as GNW key codes through the input queue. The role of the `WH_KEYBOARD` hook (probably blocking system keys) is not confirmed.

# UNKNOWN-005

Question:
Does this build support TCP/IP play? `valuelst.res` lists protocol 4 = TCP/IP, value 1110 says 4 protocols are supported.

Evidence:
Only one `socket()` call site exists (`0x43BE44`) and it uses AF_IPX. WSOCK32 imports do not include `gethostbyname`, `inet_addr` or `connect`.

Status:
OPEN (leaning "not supported in 1.0"; LOW confidence)

Next action:
Read the protocol selection code in `net1.c` (uses option `netprotocol`) and the network menu functions `0x42B0CE` / `0x42B47D`; check `messages.txt` for the protocol names shown to the user.

# UNKNOWN-006

Question:
What is `levels.dat` (4 bytes: `81 FB BF 33`) and is it related to the "Software expired. Contact Interplay." check?

Evidence:
Strings `levels.dat` (`0x45A990`) and the expiry message (`0x45A99B`) are adjacent in the data segment. 0x33BFFB81 read as a Unix time is 1997-07-06.

Status:
OPEN

Next action:
Find the function referencing `0x45A990` and decompile it.

# UNKNOWN-007

Question:
How are `.ani` frame images (`CIMG`) encoded, and what do `HEAD`, `TPAL`, `CBOX`, `SEQ `/`STAT` records contain?

Evidence:
Strings in `ani.c`: `enctype 0x00: buffers unequal size`, `unsupported enctype: 0x%02x`, `unsupported bits per unit count`, `Total of %u framebytes saved by cropping!`. `PAL ` chunks are 8192 bytes, `TPAL` 1028 bytes. `tools/fredit.doc` and `tools/fredspit.txt` document the authoring tools.

Status:
MOSTLY RESOLVED (session 3). Container, frames, RLE and sequence→frame mapping are decoded and implemented in `tools/asset-extractor/`; see `file-formats.md`. Still undecoded: file `HEAD`, `PAL `, `TPAL`, `CBOX`, the 46-byte `STAT/HEAD`, and the unused type-11 images in `classics.ani`.

Next action:
Read `0x41CD03` (sequence loader) for the STAT fields, in particular whether the first word is a per-step duration.

# UNKNOWN-008

Question:
What is the exact rule in `Path_BuildResourceName` (`0x411D17`)? Callers pass names ending in `.plt`, but the files on disk are `.pcx`.

Evidence:
Format strings `%s/%s/%s.pcx` etc. are chosen by a switch in that function; `NM: '%s' --> ` debug print.

Status:
MOSTLY RESOLVED (session 5, dynamic). The debug log shows the mapping at work: `winz.plt` → `.\data\res\winz.pcx`, `bmdrop3.snd` → `.\data\sound\bmdrop3.rss`, `tiles3.ani` → `.\data\ani\tiles3.ani`. The requested extension selects the directory and the real extension. The full table inside `0x411D17` has not been transcribed.

# UNKNOWN-009

Question:
Which main-menu items are which? Items 1 and 2 both lead to network setup and then `Match_Run`.

Evidence:
`Menu_MainMenuLoop 0x42B9CE` switch: 0 → `Match_Run`, 1 → `0x42B0CE`, 2 → `0x42B47D`, 3 → `0x4080DC`, 4 → `0x41302D`, 5 → `0x41431C`, 6 → `0x412987`.

Status:
RESOLVED (session 6, screenshot). 0 Start Game, 1 Start Network Game, 2 Join Network Game, 3 Options, 4 About Bomberman, 5 Online Manual, 6 Exit Bomberman.

# UNKNOWN-010

Question:
What do the three return values of `Maybe_Net_GetRole` (`0x40C06A`) mean? Working guess: 0 = no network, 1 = client, 2 = server.

Evidence:
`Round_Init` waits for a level number from the network when the value is 1 and broadcasts when it is 2.

Status:
OPEN

Next action:
Decompile `0x40C06A` and the code that sets its backing variable.

# UNKNOWN-011

Question:
What is the network packet format, and how is game state synchronised (inputs only, or state)?

Evidence:
`net1.c` strings: `*invalid packet*`, `init error: illegal token: %u`, `RETRANSMITTED A %u TYPE (crit was %d)!`, `Ignoring a network player's supposed death.` Small functions `0x40EE16`, `0x40EE59`, `0x40F064`, `0x40FB44`, `0x40FB90` each build one message type.

Status:
OPEN (do not implement any packet structure yet)

Next action:
After the core gameplay is understood, enumerate the message builder functions between `0x40EC00` and `0x40FE00` and the receive dispatcher `0x40E765`.

# UNKNOWN-012

Question:
What is the second part of `color.pal` (32 768 bytes after the 768-byte palette), and are palette components 6-bit?

Evidence:
File size is exactly 768 + 32768. Palette bytes are ≤ 0x3F except the first entry (`FF FF FF`).

Status:
RESOLVED (session 7). `color.pal` = 256 six-bit RGB triples + RGB555→index table. Evidence: every colour in the original's screenshots is a palette entry ×4; `field0.pcx`'s palette equals palette ×4 in 254 entries; sprites decoded through the table and palette reproduce the original's screenshot (99.46 % of pixels identical).

# UNKNOWN-013

Question:
Is the frame rate really uncapped during a match? Value 30 ("frames per second we attempt to get") is only used to derive the 50 ms unit.

Evidence:
No `Sleep` or wait call is reachable from `Game_Tick` or the `Match_Run` loop in the code read so far. The present function `0x415C1F` has not been read; it may wait for vertical blank.

Status:
RESOLVED (static, session 3; confirmed dynamically, session 5): about 960 presents per second under Wine on a modern machine.

# UNKNOWN-014

Question:
What are the controller type values in `OBJ+0x10` for players? Known: 0 off, 1 AI, 4 network.

Evidence:
`Player_Update` returns when 0, calls `ai.c` when 1; `Player_Kill` ignores 4. `Match_PlayerSetupScreen` cycles a type with values 0-4.

Status:
RESOLVED (session 3). 0 off, 1 AI, 2 keyboard, 3 joystick, 4 network. See `players.md`.

# UNKNOWN-015

Question:
How are bricks and hidden powerups generated from a scheme (brick density, counts in values 400-412, "born with"/override/forbidden rules)?

Evidence:
`.sch` `-B` and `-P` lines; `Maybe_Powerup_RevealAtCell 0x425107`; map init `0x4258E5`, `0x4260F5`.

Status:
RESOLVED (session 3). When a scheme is applied (`0x404630`-`0x4046CC`, part of the function before `0x4046CC`), for each powerup type 0-12: if born-with > 0 it is written to value `50 + type` (`Value_Set 0x4121BF`); if has-override is set, override-value is written to value `400 + type`. Generation and player init then read those values as usual. The forbidden flag (`0x4647E0[type]`) is read in one place in gameplay code: the "random" powerup re-rolls until it lands on a non-forbidden type (`0x41E480`). Tables: born-with `0x4647A4`, has-override `0x464764`, override `0x4646C4`, forbidden `0x4647E0`.

# UNKNOWN-016

Question:
What are the exact conditions for dropping a bomb, punching, grabbing/throwing and triggering? Can a bomb be dropped on a cell that already has one, or while off-centre?

Evidence:
`Player_DropBomb 0x41EB13` only builds the bomb. The checks are in its callers inside `Player_Update` / `Maybe_Player_ReadInput`.

Status:
RESOLVED (session 3). See "Actions" in `players.md`.

# UNKNOWN-017

Question:
Where does invulnerability (`OBJ+0x66`) prevent death?

Evidence:
The timer is set on campaign respawn and counted down in `Player_Update`, but `Player_Kill` does not test it.

Status:
RESOLVED (session 3). `0x41DCB2` (start of dying, called from `Player_Kill`) does nothing if `OBJ+0x66 != 0` or the player is already dying. It also updates the kill score: killer `+0x6C` += 1, or −1 for a suicide unless option `0x46497C` is set.

# UNKNOWN-018

Question:
When exactly does the closing-walls phase start, and is its 250 ms cadence really wall-clock time (unaffected by the 150 ms clamp and by pause)?

Evidence:
`Map_UpdateEnclosement 0x426818` compares time left against value 101 minus 5 and steps with `Time_GetMs`.

Status:
RESOLVED (session 3). `0x42685D`-`0x426873`: the phase is armed when `time_left <= V(101) − 5`, i.e. 55 s by default, and reset when above. With an infinite timer the getter returns 1001, so it never starts. The 250 ms cadence uses `Time_GetMs` directly. Effect of pause on it is still unchecked (minor).

# UNKNOWN-019

Question:
What does value 102 ("period of time when over-powerful powers won't appear", 40) measure, and from when?

Evidence:
`Powerup_RevealAtCell 0x425107` compares `Value_Get(0x66)` with the result of `0x4105B0` (a round-time getter next to `Maybe_Round_GetTimeLeft`).

Status:
RESOLVED (session 3). `0x4105B0` returns seconds elapsed in the round (`0x4601BC`). The protection applies while elapsed < V(102) = 40 s.

# UNKNOWN-020

Question:
Which behaviours are counted in ticks instead of milliseconds, and therefore depend on the machine's frame rate?

Evidence:
Found so far: head-hit stun (`OBJ+0x3A`, 16 ticks), disease pass cooldown (`OBJ+0x80`, value 129 ticks), chain-reaction delay (one tick per link), kick/stand animation counter when idle (`+0x30` incremented once per tick while standing).

Status:
IN PROGRESS. Measured under Wine: about 1 ms per tick, which makes tick-counted rules nearly instantaneous there. The designed rate is taken to be the nominal 20 frames per second (value 25). Recommendation: fixed 50 ms simulation step in the modern game.

Next action:
If period hardware or a throttled emulator (e.g. a slowed-down VM) becomes available, measure the real tick rate; grep the game modules for further bare per-tick counters.

# UNKNOWN-021

Question:
How are player colours applied? Sprites are stored once in green; `N.rmp` files hold 256-byte remap tables.

Evidence:
Sequence names end in `green`; `0x415ED1` references `Remap table #%u (%u.rmp)`; draw calls pass a colour index (`OBJ+0x3C`).

Status:
RESOLVED (session 7). `N.rmp` holds a 256-entry table that is non-zero only for palette indices 100-174 (the green band); a non-zero entry replaces the index. Player sprites, bombs and flames drawn through it match the original's colours. Where in the original's blitters the lookup happens was not traced.

# UNKNOWN-022

Question:
What happens in the original when the round clock reaches zero while more than one contender is alive?

Evidence:
`Match_Run` ends a round when the contender count is ≤ 1. No time-up branch has been identified yet; the clock code clamps at 0.

Status:
OPEN

Next action:
Scripted run with `playtime` at its minimum and enclosement depth 0, two idle players; watch what follows 0:00. Read the remainder of `Match_Run` (`0x42A6A9` onward).
