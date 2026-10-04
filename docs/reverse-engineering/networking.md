# Networking

Static analysis (Level 1), first pass. Nothing here has been observed on the wire, and **no packet structure below should be implemented as a compatibility target** until it has been (AGENTS.md §29). Addresses are VAs in `bm95.exe`.

## Layers

```text
net1.c  game messages (typed, optionally reliable)          0x40C000 - 0x410260
        packet assembly / dispatch                          0x40CE27, 0x40E765, Net_Pump 0x40EA1E
transport, chosen by option `netprotocol` (values 1100-1104)
        IPX datagrams through Winsock                       0x43B99C - 0x43C1xx
        serial / modem through a comm library               0x44C9xx - 0x44Dxxx
```

- Transport receive is called through a function pointer (`0x460108`), so the game layer is transport-independent. Confidence: HIGH.
- IPX: `socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX)`, `bind`, `sendto`/`recvfrom`, non-blocking (`ioctlsocket`). Confidence: HIGH (see `initial-analysis.md`). Port number and broadcast use: not read.
- TCP/IP is listed in the data files but no `AF_INET` code exists in this build (UNKNOWN-005).

## Roles

`Maybe_Net_GetRole` (`0x40C06A`) returns 0 (no network), 1 or 2. From the callers:

- **2 = host.** Chooses the level, generates tiles and powerups and sends them, runs the round clock and broadcasts it, sends the tuning values.
- **1 = client.** Waits for the level number, receives tiles, powerups and extras before the round starts, takes the clock from the host.

Confidence: HIGH for the split of duties (seen in `Round_Init`, `Map_GenerateForRound`, `Powerups_GenerateForRound`, the round clock); the 1/2 naming follows from it.

Authority model: each machine simulates **its own** players and reports them; a player controlled from another machine (controller type 4) is placed from the last received position and is never killed locally ("Ignoring a network player's supposed death"). Bombs, deaths, pickups and kicks are announced by the machine where they happen. Confidence: MEDIUM-HIGH.

Up to 5 machines ("nodes") are tracked (`0x460130`, five entries).

## Packet

Receive buffer at `0x45ED9C`. Confidence: MEDIUM (field boundaries inferred from accesses in `0x40E765`).

```text
+0   u16  magic 0x536C
+2   u16  (packet length; used only for statistics)
+4   u16  number of messages in the packet
+6   u16  sender's node id (packets carrying our own id are ignored)
+8   u16  ?
+10  messages...
```

Message:

```text
+0  i16  length of this message, including these six bytes
+2  i16  type (0-99)
+4  i16  sequence id: 0 for ordinary messages, non-zero for "critical" ones
+6  payload
```

Several messages are batched into one packet; `0x40CE27(type, payload, size)` queues one.

## Reliability ("critical" messages)

Confidence: HIGH for the mechanism.

- Types 32 and above are critical. Each carries a non-zero sequence id.
- The sender keeps up to 500 unacknowledged critical messages (`0x460014`, 28-byte records). `Net_Pump` resends any that has waited longer than value 1100 + protocol (200 ms for IPX, 750 modem, 400 serial) and logs `RETRANSMITTED A %u TYPE`. After 25 resends the message is dropped (`BLOWING IT AWAY!`).
- The receiver acknowledges every critical message with message type 12 carrying the sequence id (`0x40FBC2`), and remembers the last 500 ids per node so that a resent duplicate is acknowledged again but not processed twice.
- Types below 32 are sent once and may be lost.

## Messages

Type and payload size are read from each sender's call to `0x40CE27`; handlers are registered in a table at `0x45BACC` (`0x40E412`). "Meaning" is inferred from where the sender is called; rows without a named caller are guesses from position in the code and are marked LOW.

### Ordinary (may be lost)

| Type | Size | Sender | Handler | Meaning | Confidence |
|---:|---:|---|---|---|---|
| 0 | 82 | `0x40EBC1` | `0x40CF93` | Node announcement (lobby) | LOW |
| 1 | 0 | `0x40EC29` | `0x40D073` | Leave / cancel, sent from the player list screen | LOW |
| 2 | 0 | `0x40EC4C` | `0x40D10B` | ? | – |
| 3 | 43 | `0x40EC6F` | `0x40D175` | Game advertisement or join data | LOW |
| 4 | 12 | `0x40EF4E` | `0x40D519` | **Player state**: position, alive flag, direction, animation frame. Sent every tick for each local player | HIGH |
| 5, 10 | 6 | `0x40F504` | `0x40D5EE`, `0x40DC06` | Tile data during round start | LOW |
| 6 | 4 | `0x40F5E8` | `0x40D642` | Request for a tile during round start (client) | MEDIUM |
| 7, 15 | 4 / 16 | `0x40F75B` | `0x40D852`, `0x40D897` | Request for / reply with a powerup slot during round start | MEDIUM |
| 8 | 8 | `0x40F974` | `0x40DA36` | **Bomb position** (a moving bomb), sent when its position changed | HIGH |
| 9 | 0 | `0x40FA43` | `0x40DAD5` | Round-start handshake (host) | MEDIUM |
| 11 | ? | `0x40F623` | `0x40DC77` | ? | – |
| 12 | 4 | `0x40FBC2` | `0x40DCFC` | **Acknowledgement** of a critical message | HIGH |
| 13, 16, 17 | 2 / 160 / 2 | `0x40F830`, `0x40F7FF`, `0x40F862` | `0x40D9B0`, `0x40D913`, `0x40D9F3` | Level extras transfer during round start (request, record, done) | MEDIUM |
| 14 | 12 | `0x40ED08` | `0x40D251` | ? (lobby) | – |
| 18 | 2 | `0x40ECC3` | `0x40D12D` | ? (lobby) | – |

### Critical (acknowledged, resent)

| Type | Size | Sender | Meaning | Confidence |
|---:|---:|---|---|---|
| 32 | 2 | `0x40F064` | A key / command code relayed to the other machines (e.g. "continue" on result screens) | MEDIUM |
| 33 | 18 | `0x40F096` | **Bomb created**: colour, cell, owner, range, fuse, delay, bomb id. Sent by `Player_DropBomb` | HIGH |
| 34 | 8 | `0x40F10F` | **Bomb detonates**: arrival direction and bomb position | HIGH |
| 35 | 8 | `0x40F710` | **Powerup placed** (revealed) at a cell | HIGH |
| 36 | 6 | `0x40F9BF` | **Player dies**, with the death animation number. Sent by `Player_Kill` | HIGH |
| 37 | 2 | `0x40FB44` | Match exit state | MEDIUM |
| 38 | 2 | `0x40FB90` | Match flag set from the F-key handler in `Match_Run` | LOW |
| 39 | 0 | `0x40EDC9` | Player list: leave | LOW |
| 40 | 6 | `0x40EE16` | **Player list: a slot's controller type** | HIGH |
| 41 | 2 | `0x40EE94` | Player list: finished | MEDIUM |
| 42 | 0 | `0x40F947` | Round-start handshake (client ready) | MEDIUM |
| 43, 44 | 2 | `0x40FA66`, `0x40FAD5` | Round-start handshakes | LOW |
| 45 | 2 | `0x40FCA1` | **Round clock**: seconds left, from the host | HIGH |
| 46 | 20 | `0x40FCE6` | ? | – |
| 47 | 4 | `0x40FD7B` | **Powerup removed** at a cell | HIGH |
| 48 | 6 | `0x40FDE8` | **Tile changed** at a cell | HIGH |
| 49 | 16 | `0x40FE88` | ? | – |
| 50 | 6 | `0x40FF14` | **Kill score** update | MEDIUM |
| 51 | 18 | `0x410021` | **Player inventory** (the 15 powerup counts) | HIGH |
| 52 | 22 | `0x410065` | **Player disease state** | HIGH |
| 53 | 40 | `0x4100B9` | Start positions for the ten players | MEDIUM |
| 54 | 4 | `0x41013F` | **Bomb launched** (punched / thrown), direction | HIGH |
| 55 | 4 | `0x41017A` | **Bomb kicked**, direction | HIGH |
| 56 | 2 | `0x4101B5` | **Level number** for the round, from the host | HIGH |
| 57 | 2 | `0x4101F1` | Sent from the joystick branch of the input reader (purpose unknown) | LOW |
| 58 | 4 | `0x40EE59` | Player list: a slot's team | MEDIUM |
| 59 | 318 | `0x41022D` | **Tuning values**: 79 pairs of (value id, value), from the host (the values marked `; PGT` in `valuelst.res`) | HIGH |
| 60 | 2 | `0x40FDB6` | **Carried bomb removed** (its holder died) | MEDIUM |
| 61 | – | – | Handler only (`0x40E3C7`) | – |

## What this means for the modern game

The original is neither lockstep nor a pure client-server design: movement is streamed unreliably per player, discrete events are sent reliably by whoever caused them, and the host owns the map set-up and the clock. Random decisions (dud bombs, disease swap, trampoline placement, the "random" powerup in generation) are disabled or host-decided in network games.

The modern core is deterministic given its inputs, which makes an input-synchronised (lockstep or rollback) design possible instead. That is a design decision still to be made; it does not need the original's packet format unless interoperability with the original is wanted.

## Open

- Field layouts of every payload.
- Lobby and discovery (types 0-3, 14, 18), node names (`nodename.ini`), joining and leaving mid-game, `lost_net_revert_ai`.
- IPX socket number, broadcast address use; the serial framing.
- Whether packets are padded or checksummed.
