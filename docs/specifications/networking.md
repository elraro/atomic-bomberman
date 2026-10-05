# Network play (modern design)

This is **not** the original's network mode. The original (`../reverse-engineering/networking.md`) talks IPX or serial, lets every machine simulate its own players and was only analysed statically; none of it is reproduced. This document specifies a new client-server mode for the modern game. Everything here is this project's own design; nothing is a claim about the original.

## Goals

- Play over a LAN or the internet with TCP/IP: up to 10 players, one per machine, the rest computer players.
- One server decides everything. It can run inside the game ("Start Network Game") or alone as a dedicated program (`atomic_server`) with no window, no SDL and no graphics.
- Lobby with player list, match settings and chat; chat during the match as well.
- The gameplay rules are exactly those of the local game: the same core (`src/game`) runs the round.

## Model: server-driven lockstep

The gameplay core is deterministic: integer arithmetic only, its own random generator, a fixed 50 ms step. Two cores given the same start and the same inputs stay identical. The network mode uses that:

1. The server builds the round (`RoundSetup`: tuning values, scheme, level extras, options, seats, seed) and sends it to every client. Each side creates the same `World` from it.
2. Every 50 ms the server takes the latest input it has from each client, lets its computer players decide theirs, advances its own `World` one step and sends the **inputs of all ten seats for that step** to the clients.
3. A client never advances its `World` on its own: it applies the steps in order as they arrive. What it draws is always a state the server has already been through.
4. With each batch of steps the server sends a hash of its state. A client whose hash differs asks for the full state (a snapshot), loads it and goes on. The same snapshot lets someone who connects in the middle of a round watch it.

Why not send positions: the whole state (10 players, up to 100 bombs, three 15 x 11 grids) is several kilobytes per step; the inputs are 10 bytes. Sounds and animations need no messages at all because each client produces them from its own simulation.

Cost: the confirmed state lags by the round trip to the server plus up to one step. Client-side prediction (below) hides that delay for the player's own moves.

Computer players run on the server only, so their code need not be deterministic. A player who disconnects during a round is taken over by a computer player until the round ends.

## Client-side prediction

What a player sees is not the confirmed state but a guess at the state a few steps later: the one in which the keys pressed now will take effect.

- The client keeps the confirmed `World` exactly as before; nothing predicted ever enters it. Scores, results, sounds and the state hash come from it alone.
- Once per 50 ms of its own clock the client copies the confirmed state and runs the copy forward to a target step: the last confirmed step plus the delay, where the delay is the measured round trip in steps plus one (1 to 10 steps). For those steps it uses its own inputs, remembering which input it assumed for which step, and assumes every other player goes on doing what they did in the last confirmed step.
- The target advances by one per local step and is pulled back to "confirmed + delay" only when it has drifted by more than two, so the picture moves evenly even when the server's steps arrive unevenly.
- The copy is thrown away and rebuilt every local step, so a wrong guess (another player turned, a bomb was dropped) lasts only until the server's steps say otherwise; the drawing interpolates over one step, which softens the correction.
- No prediction for watchers, after the round is decided, or when switched off (Options, "Network: Show Own Moves At Once"; `net_prediction` in the settings file).

Sounds follow the confirmed state, with one exception: the player's own actions (dropping, punching, grabbing or throwing a bomb, picking up a powerup, a trampoline, a warp) are heard from the predicted step, at once, and are left out when the server confirms them. A predicted action that never comes true has been heard for nothing; it is forgotten after 1.5 s.

Known effect: other players are drawn where they would be if they had not changed direction, so at high delay they visibly jump when they do.

## Transport

One port number (default **27410**) for both protocols.

| Channel | Used for |
|---|---|
| TCP | Joining, lobby state, settings, chat, round start, round result, snapshots. Reliable and ordered. |
| UDP | The two messages that repeat during a round: inputs to the server, steps to the clients. Also finding servers on the LAN: every server, whatever its own port, also listens on UDP **27409** (a port several servers on one machine can share) and answers searches there. |

UDP loss is handled without retransmission requests: every input message carries the number of the last step the client has; every step message carries **all steps after that one** (at most 40, i.e. 2 s). A lost datagram is covered by the next one.

UDP is optional. After joining, the client sends UDP probes carrying the token it was given over TCP; when the server's answer arrives it reports "UDP works" over TCP. Until then, or if steps stop arriving by UDP for 2 s during a round, both directions use TCP for these messages too. So the game works through a TCP-only tunnel, only less smoothly.

IPv4 only for now.

### TCP framing

`u32 length` (little endian, of what follows, at most 1 MiB), `u8 type`, payload. Integers are little endian; strings are `u16 length` + UTF-8 bytes.

Client to server:

| Type | Payload | Meaning |
|---|---|---|
| Hello | magic `ABMN`, `u16` protocol version, name, password | First message. Answered by Welcome or Reject. |
| Chat | text | |
| Option | `u8` option, `i8` direction | Administrator: change a match setting one step left or right. The server knows the choices (e.g. the scheme list). |
| Start | | Administrator: start the match. |
| Team | | Change own team. |
| Kick | `u8` client id | Administrator. |
| Input | as the UDP message | When UDP is not in use. |
| UdpState | `u8` on | "My UDP path works / stopped working". |
| NeedState | | Hash mismatch: send a snapshot. |
| Continue | | Key pressed on the result screen. |
| Pong | `u32` | Echo of Ping. |

Server to client:

| Type | Payload | Meaning |
|---|---|---|
| Welcome | client id, UDP token, server name | |
| Reject | reason text | Wrong version, wrong password, server full, kicked. |
| Lobby | phase, administrator, settings, ten seats, client list | Sent whenever any of it changes. |
| Chat | sender id (255 = server), sender name, text | |
| RoundStart | round id, tuning values, `RoundSetup`, scores | |
| Steps | round id, first step, count, 10 input bytes per step, hash of the state after the last one | When UDP is not in use. |
| RoundEnd | round id, winner, match winner, wins and kills per seat | The server's verdict; clients show the result screen. |
| Snapshot | round id, step number, `World` state | Answer to NeedState; also for late joiners. |
| Ping | `u32` | Once a second; the measured times are in the Lobby message. |

### UDP datagrams

`u32` magic `ABMU`, `u8` type, payload.

| Type | Direction | Payload |
|---|---|---|
| Probe | C to S | token |
| ProbeAck | S to C | |
| Input | C to S | token, round id, last step held, input byte |
| Steps | S to C | as the TCP message |
| Query | broadcast to the discovery port **27409** | protocol version |
| Info | S to C | protocol version, TCP port, server name, players, seats, phase, password needed |

An input byte: bits 0-3 north, east, south, west; bit 4 bomb; bit 5 action.

## Server

Phases: **Lobby** → **Round** → **Result** → (next round, or Lobby when the match is decided).

- Seats: ten, each empty, a client's or a computer player's. A joining client gets the lowest empty seat (a computer player's seat if none is empty; if all ten are humans it watches). Joining during a match: watches until the lobby returns.
- Administrator: the client that has been connected longest. Changes settings, the number of computer players, starts the match, kicks. In a game hosted from the menu this is the host.
- Settings: level (or random each match), scheme, wins needed, team play, play time, enclosement depth, computer players, random start, conveyor speed, stomped bombs detonate, win by kills, diseases destroyable. The meanings are those of the local game (`game-modes.md`).
- A match needs at least two occupied seats, and one player on each team in team play.
- Round: starts 1 s after RoundStart was sent; one step every 50 ms of the server's clock (it catches up after a stall, at most 5 steps at once).
- Result: scored as in the local game (`game-modes.md`, Match). The next round starts when every connected player has pressed a key, or after 10 s.
- The roulette and campaign mode are not part of network play.
- A client that sends nothing for 15 s is dropped. Chat lines are cut to 120 characters, names to 16.
- If the last human leaves during a match, the server returns to the lobby.

The dedicated server reads the game data (tuning values, schemes, level extras) from an imported asset folder or an original game folder, as the game does; without one it serves the built-in arena with the default values. Clients need the same folder only for graphics and sound: everything that affects the rules comes from the server.

## Client

- Sends its input (first keyboard set, or the first gamepad) whenever it changes and at least every 50 ms.
- With prediction: applies confirmed steps as they arrive and paces the picture by its own clock. Without (watchers, or prediction off): applies one step per 50 ms; with more than 3 steps waiting two per 50 ms, with more than 20 as fast as it can.
- Screens: join (name, address, servers found on the LAN), host (server name, port, password), lobby (seats, settings, chat), match, result.
- Chat: typed directly in the lobby; in a match `T` opens the line, Enter sends, Esc closes. The last lines stay on screen for 8 s.

## Test plan

- Core: state saved and loaded into a fresh `World` continues identically; the hash changes when any gameplay field changes.
- Codec: every message survives encode/decode; truncated messages are refused.
- Loopback: a server and two clients in one process over 127.0.0.1 play a round to its result with equal hashes; chat arrives; a wrong password is refused; a forced mismatch is repaired by a snapshot; a late joiner reaches the same state; with UDP blocked the round still completes over TCP.

## Limits accepted for now

- Prediction covers the picture and the sounds of one's own actions; everything else (explosions, other players) is heard with the round-trip delay, and other players' changes of direction appear as small corrections.
- IPv4 only; no NAT traversal: the host's port must be reachable.
- One player per client; no roulette, no campaign.
- Clients trust the server's snapshots (see `../migration/networking.md`).
- Different versions of the program must not be mixed: the protocol version is raised whenever the simulation changes.

Not covered by automated tests: behaviour on real links with loss and delay, NAT traversal (the host must be reachable on the port), more than a handful of clients.
