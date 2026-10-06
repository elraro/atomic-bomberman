# Network play (modern design)

This is **not** the original's network mode. The original (`../reverse-engineering/networking.md`) talks IPX or serial, lets every machine simulate its own players and was only analysed statically; none of it is reproduced. This document specifies a new client-server mode for the modern game. Everything here is this project's own design; nothing is a claim about the original.

**Where this is going:** `online-play.md` specifies the target for internet play (a master server, NAT traversal with STUN/TURN, snapshots instead of lockstep). It is not implemented; this document describes what the game does today.

## Goals

- Play over a LAN or the internet with TCP/IP (IPv4 and IPv6): up to 10 players, up to four at one machine, the rest computer players.
- One server decides everything. It can run inside the game ("Start Network Game") or alone as a dedicated program (`atomic_server`) with no window, no SDL and no graphics.
- Lobby with player list, match settings and chat; chat during the match as well.
- The gameplay rules are exactly those of the local game: the same core (`src/game`) runs the round.

## Model: server-driven lockstep

The gameplay core is deterministic: integer arithmetic only, its own random generator, a fixed 50 ms step. Two cores given the same start and the same inputs stay identical. The network mode uses that:

1. The server builds the round (`RoundSetup`: tuning values, scheme, level extras, options, seats, seed) and sends it to every client. Each side creates the same `World` from it.
2. Every 50 ms the server takes the latest input it has from each client, lets its computer players decide theirs, advances its own `World` one step and sends the **inputs of all ten seats for that step** to the clients.
3. A client never advances its `World` on its own: it applies the steps in order as they arrive. What it draws is always a state the server has already been through.
4. What a player may not know yet stays on the server (see "Secrets"): a client's `World` starts from a snapshot without it and is given, with each step, only what that step used.
5. With each batch of steps the server sends a hash of its (public) state. A client whose hash differs asks for the full state (a snapshot), loads it and goes on. The same snapshot lets someone who connects in the middle of a round watch it.

Why not send positions: the whole state (10 players, up to 100 bombs, three 15 x 11 grids) is several kilobytes per step; the inputs are 10 bytes. Sounds and animations need no messages at all because each client produces them from its own simulation.

Cost: the confirmed state lags by the round trip to the server plus up to one step. Client-side prediction (below) hides that delay for the player's own moves.

Computer players run on the server only, so their code need not be deterministic. A player who disconnects during a round is taken over by a computer player until the round ends.

## Secrets

Two things would let a modified client see more than a player should: which powerup lies under which brick, and the random generator, from which every future chance event follows. Neither is given to clients.

- The round does not start from a seed any more. The server builds it and sends a **public snapshot**: the whole state except hidden powerups (they are simply absent) and the random generator's state.
- Running a step, the server's `World` **records** the random numbers it draws and every hidden powerup it looks at (when a brick burns away, and in the few rules that look under other bricks, such as the early-round swap). These "secrets of the step" travel with the step's inputs.
- A client's `World` draws no numbers of its own: before a step it is fed that step's numbers, and the hidden powerups the step will look at are put under their bricks just in time. It then computes the step exactly as the server did. Using fewer or more numbers than were sent counts as being out of step, like a hash mismatch.
- The state hash is taken over the public view on both sides.
- Prediction runs on a copy with random numbers of its own, so whatever chance decides in those few steps is a guess. Guesses about chance are not shown: after running ahead, the copy gives up every powerup and regrown brick the confirmed state does not have, shows a player who died only in the guess as still confirmed (which death animation plays is chance), and keeps campaign enemies where they are confirmed. Such things appear when the server's step arrives. (Before this rule a dead player's scattered powerups appeared in guessed places and jumped to the real ones a moment later.)

What a client still learns early: the few hidden powerups a step looked at without revealing (the early-round swap can move one under another brick), and one timer that says when the next dud bomb may occur. What it cannot learn: the layout of hidden powerups, or anything decided by chance before it happens.

## Client-side prediction

What a player sees is not the confirmed state but a guess at the state a few steps later: the one in which the keys pressed now will take effect.

- The client keeps the confirmed `World` exactly as before; nothing predicted ever enters it. Scores, results, sounds and the state hash come from it alone.
- Once per 50 ms of its own clock the client copies the confirmed state and runs the copy forward to a target step: the last confirmed step plus the delay, where the delay is the measured round trip in steps plus one (1 to 10 steps). For those steps it uses its own inputs, remembering which input it assumed for which step, and assumes every other player goes on doing what they did in the last confirmed step.
- Other players are **drawn where the server last confirmed them**, not where their last movement would carry them: nobody knows their next move, and carrying the last one on made a player who turns about (a cornered computer player above all) seem to dart to and fro at speed.
- The target advances by one per local step and is pulled back to "confirmed + delay" only when it has drifted by more than two, so the picture moves evenly even when the server's steps arrive unevenly.
- The copy is thrown away and rebuilt every local step, so a wrong guess (another player turned, a bomb was dropped) lasts only until the server's steps say otherwise; the drawing interpolates over one step, which softens the correction.
- No prediction for watchers, after the round is decided, or when switched off (Options, "Network: Show Own Moves At Once"; `net_prediction` in the settings file).

Sounds follow the confirmed state, with one exception: the player's own actions (dropping, punching, grabbing or throwing a bomb, picking up a powerup, a trampoline, a warp) are heard from the predicted step, at once, and are left out when the server confirms them. A predicted action that never comes true has been heard for nothing; it is forgotten after 1.5 s.

Known effect: other players are seen a little in the past (by the round trip) while one's own player, the bombs and the flames are seen in the present, so at high delay an opponent can seem to stand in a flame for a moment before falling. An earlier version carried other players' last movement forward instead; a player who kept turning about (a cornered computer player above all) then seemed to dart to and fro at speed.

## Transport

One port number (default **27410**) for both protocols.

| Channel | Used for |
|---|---|
| TCP | Joining, lobby state, settings, chat, round start, round result, snapshots. Reliable and ordered. |
| UDP | The two messages that repeat during a round: inputs to the server, steps to the clients. Also finding servers on the LAN: every server, whatever its own port, also listens on UDP **27409** (a port several servers on one machine can share) and answers searches there. |

UDP loss is handled without retransmission requests: every input message carries the number of the last step the client has; every step message carries **all steps after that one** (at most 40, i.e. 2 s). A lost datagram is covered by the next one.

UDP is optional. After joining, the client sends UDP probes carrying the token it was given over TCP; when the server's answer arrives it reports "UDP works" over TCP. Until then, or if steps stop arriving by UDP for 2 s during a round, both directions use TCP for these messages too. So the game works through a TCP-only tunnel, only less smoothly.

IPv4 and IPv6: a server listens for both on one port where the system allows it (IPv4 alone otherwise); a client uses whichever the address it was given resolves to. An IPv6 literal with a port is written `[address]:port`. A link-local IPv6 address is given with its interface, `[fe80::1%eth0]:port`.

Protocol version 5 (1: one player per client; 2: several players, prizes, campaign stages; 3: encryption; 4: secrets kept on the server; 5: server identity).

## Encryption and the password

Everything a connection carries is encrypted and authenticated, except the two key messages that set it up and searches on the local network.

1. The client's first frame, in the clear: `ABMN`, the protocol version, and a fresh X25519 public key.
2. The server answers in the clear with its own fresh public key (or with Reject if the versions differ).
3. Both compute the shared secret and from it, with both public keys, a master value (SHA-256) and four keys: TCP to server, TCP to client, UDP to server, UDP to client.
4. From then on every TCP frame is `u32 length` + ChaCha20-Poly1305 of (type, payload), with the frame's number in its direction as the nonce. A frame that fails to open ends the connection: nothing can be altered, dropped, repeated or reordered unnoticed.
5. A connection's datagrams are `ABMU`, `0x80`, the session number, a counter, then ChaCha20-Poly1305 of the datagram under the UDP key of its direction (header authenticated). A receiver accepts a counter only if it is higher than the last one accepted, so a recorded datagram cannot be replayed. The session number only names the connection; knowing it gives nothing.
6. **Password.** The client never sends it. Hello carries HMAC-SHA-256 keyed with a hash of the password over the connection's master value: proof of knowing the password that is useless on any other connection. The server compares it in constant time.

What this gives: someone who can read the traffic learns nothing (not the chat, not the names, not the password) and cannot later decrypt a recording even with the password, because the keys exist only for that connection. Someone who can alter the traffic can only break the connection.

**Server identity.** A server has a lasting X25519 key (kept in its identity file, made on first start); its public half is sent with the key exchange and shown as a fingerprint such as `3F9A-11C0-7B42-E5D8` (in the server's log and in the lobby). The connection's keys are derived from the fresh exchange *and* from the secret between the client's fresh key and that lasting key, so whoever shows an identity without owning it never gets the keys. A client remembers the identity of each address it has joined (`known_servers.txt`); if the server at an address later shows another identity, the client refuses to connect and says so, and the player can choose to forget the old one. Servers on the same machine and games joined through a relay are not remembered.

What this does not give: the first visit to a server is taken on trust. An attacker in the middle at that very first connection can pose as the server (with a password set, such an attacker still cannot join the two halves, but can test password guesses against the proof it receives, so a guessable password falls to it). There are no certificates and no authority vouching for a server; comparing the fingerprint with one the owner gave you by other means is the only check of a first visit. The algorithms are implemented in this project (`src/net/crypto.*`) and pass their specifications' test vectors; the code has not been audited.

## Bans and limits

- **Administrator**: whoever has been connected longest, until the role is handed over (`/admin NAME`) or taken with the server's administrator password (`/login PASSWORD`, if the server was started with one). When the administrator leaves, the role falls back to whoever has been there longest.
- **Kick**: the player's address may not connect for five minutes.
- **Ban** (administrator: `/ban NAME`; dedicated server console: `ban NAME`): the address stays out until `unban ADDRESS`. Bans are kept in the server's ban file and survive a restart. The administrator is told the address when banning.
- At most 10 connections from one address at a time (`maxPerAddress`).
- More than 20 connection attempts from one address in ten seconds: blocked for a minute.
- Five wrong passwords from one address in a minute: blocked for five minutes.
- More than 400 messages a second on a connection (a client sends about 40): dropped, and the address blocked for a minute.
- At most 30 answers a second to LAN searches.

All of these go by address, so players behind one router share them, and an address is not a person: a banned player with a new address is back.

### TCP framing

`u32 length` (little endian, of what follows, at most 1 MiB), `u8 type`, payload. Integers are little endian; strings are `u16 length` + UTF-8 bytes.

Client to server:

| Type | Payload | Meaning |
|---|---|---|
| KeyExchange | magic `ABMN`, `u16` protocol version, 32-byte public key | First message, in the clear. Answered by KeyExchange or Reject. |
| Hello | name, 32-byte password proof | First encrypted message. Answered by Welcome or Reject. |
| Ban | `u8` client id | Administrator. |
| Admin | `u8` client id | Administrator: hand the role over. |
| Login | text | The administrator password. |
| LeaveMatch | | Back to the lobby for the rest of this match. |
| Unban | address | Administrator. |
| Chat | text | |
| Option | `u8` option, `i8` direction | Administrator: change a match setting one step left or right. The server knows the choices (e.g. the scheme list). |
| Start | | Administrator: start the match. |
| Ready | `u8` flag | In the lobby: this client is ready, or no longer. |
| Team | `u8` local player | That player of this computer changes team. |
| Locals | `u8` count | Players at this computer, 1-4 (in the lobby). |
| Kick | `u8` client id | Administrator. |
| Input | as the UDP message | When UDP is not in use. |
| UdpState | `u8` on | "My UDP path works / stopped working". |
| NeedState | | Hash mismatch: send a snapshot. |
| Continue | | Key pressed on the result screen. |
| Pong | `u32` | Echo of Ping. |

Server to client:

| Type | Payload | Meaning |
|---|---|---|
| KeyExchange | 32-byte public key, 32-byte identity key | In the clear. |
| Roulette | seed, gold player, prize, frames | The bonus wheel is shown before the match. |
| Welcome | client id, session number, server name | |
| Reject | reason text | Wrong version, wrong password, server full, kicked. |
| Lobby | phase, administrator, settings, ten seats, client list | Sent whenever any of it changes. |
| Chat | sender id (255 = server), sender name, text | |
| RoundStart | round id, tuning values, `RoundSetup`, scores | Always followed by a Snapshot of step 0. |
| Steps | round id, first step, count; per step 10 input bytes, the hidden powerups looked at and the random numbers drawn; hash of the public state after the last one | When UDP is not in use. |
| RoundEnd | round id, winner, match winner, wins and kills per seat | The server's verdict; clients show the result screen. |
| Snapshot | round id, step number, `World` state | Answer to NeedState; also for late joiners. |
| Ping | `u32` | Once a second; the measured times are in the Lobby message. |

### UDP datagrams

`u32` magic `ABMU`, `u8` type, payload.

| Type | Direction | Payload |
|---|---|---|
| Probe | C to S | session number |
| ProbeAck | S to C | |
| Input | C to S | token, round id, last step held, four input bytes (one per player at that computer) |
| Steps | S to C | as the TCP message |
| Query | broadcast to the discovery port **27409** | protocol version |
| Info | S to C | protocol version, TCP port, server name, players, seats, phase, password needed |

An input byte: bits 0-3 north, east, south, west; bit 4 bomb; bit 5 action.

## Server

Phases: **Lobby** → **Round** → **Result** → (next round, or Lobby when the match is decided).

- Seats: ten, each empty, a client's or a computer player's. A joining client gets the lowest empty seat (a computer player's seat if none is empty; if all ten are humans it watches). Joining during a match: watches until the lobby returns.
- Several players at one computer: a client may ask for up to four players; each gets a seat of its own while seats are free, its own input byte, and is named "Name (2)" and so on. The second uses the second key set, and each may use a gamepad.
- Finding the port from outside: with `upnp` on (always for a game hosted from the menu, `--upnp` for the dedicated server) the server asks the home router, by UPnP, to pass its TCP and UDP port to this machine, and tells the lobby the outcome and, if the router says so, the public address. The mapping is removed when the server stops. Without a UPnP router the port must be forwarded by hand; failing that, a relay (below).
- Administrator: the client that has been connected longest. Changes settings, the number of computer players, starts the match, kicks. In a game hosted from the menu this is the host.
- Ready: every other player with a seat says when they are ready (and may take it back). The administrator's start is refused, naming who is missing, until all have; the administrator's own start says it for them, and watchers are not asked. After a match everybody is asked again.
- Settings: level (or random each match), scheme, wins needed, team play, play time, enclosement depth, computer players, random start, conveyor speed, stomped bombs detonate, win by kills, diseases destroyable, gold bomberman (the roulette), campaign. The meanings are those of the local game (`game-modes.md`).
- A match needs at least two occupied seats, and one player on each team in team play.
- Round: starts 1 s after RoundStart was sent; one step every 50 ms of the server's clock (it catches up after a stall, at most 5 steps at once).
- Result: scored as in the local game (`game-modes.md`, Match). The next round starts when every connected player has pressed a key, or after 10 s.
- Roulette ("gold bomberman" on): the winner of a match is remembered; when the next match starts the server spins the wheel and sends its seed, so that everybody sees the same wheel turn and stop (the key is "pressed" for the gold player after two seconds); it then announces the prize in the chat, and that player (every member of that team) starts each round of the match with one more of that powerup, as in the local game.
- Campaign: with one of the server's campaign files chosen, Start plays its stages in order, everybody together against the stage's enemies. Each stage sets level, arena, enemies and its own number of computer players (the lobby's are left out); team play is off; one human is enough. A cleared stage leads to the next; a stage lost by the clock too; a stage lost because no human was left is played again. After the last stage, or when every human has left, the lobby returns.
- A client that sends nothing for 15 s is dropped. Chat lines are cut to 120 characters, names to 16.
- A player can go back to the lobby during a match without leaving the server (Esc twice): the computer plays that player's seats until the match ends, the result screens do not wait for that player's key, and the player is in the next match as usual. When nobody is left playing, the match is over.
- If the last human leaves during a match, the server returns to the lobby.

The dedicated server reads the game data (tuning values, schemes, level extras) from an imported asset folder or an original game folder, as the game does; without one it serves the built-in arena with the default values. Clients need the same folder only for graphics and sound: everything that affects the rules comes from the server.

## Relay

For a host whose router lets nothing in and will not be told to. A relay is a third machine that both sides can reach (`atomic_server --relay-server`, TCP port **27408**):

1. The host's server connects out to the relay and is given a six-character code.
2. A player joins with the address `CODE@relay-address`. The relay tells the host, the host opens one more connection out, and the relay links the two: from then on it copies every frame from one to the other.
3. Through that link the player and the host do the usual key exchange. The relay carries only encrypted frames; it cannot read or change the game, and it cannot pose as the host to someone who knows the host's identity or shares a password with it. It can refuse or drop the link.

Everything goes over TCP (the relay carries no UDP), so play is as with a TCP-only path. The host's bans and limits see the player's real address, which the relay passes on. There is no public relay: somebody in the group has to run one on a reachable machine. There is no UDP hole punching.

## Client

- Sends its input (first keyboard set, or the first gamepad) whenever it changes and at least every 50 ms.
- With prediction: applies confirmed steps as they arrive and paces the picture by its own clock. Without (watchers, or prediction off): applies one step per 50 ms; with more than 3 steps waiting two per 50 ms, with more than 20 as fast as it can.
- Screens: join (name, address, servers found on the LAN), host (server name, port, password), lobby (seats, settings, chat), match, result.
- Chat: typed directly in the lobby; in a match `T` opens the line, Enter sends, Esc closes. The last lines stay on screen for 8 s.
- Everything a screen can do is an item that can be chosen (phase 1 of `online-play.md`), so that a gamepad is enough:
  - Join: the three fields, "Join this address", the servers found, "Back". Host: the five fields, "Start the server", "Back". The box shown while connecting has "Cancel"; an error has "Ok" and, for a changed server identity, "Forget the old identity".
  - Lobby: the settings, and under them a row of actions: Start (administrator) or Ready (the others), Team (one per player at this computer, with team play), Players (one more at this computer with each press; after four, one), Chat, Leave. The cursor starts on the first action. A line above the settings says what the lobby waits for.
  - Match and result: a menu (Start on a gamepad) with Resume, Chat, Back To Lobby, Leave The Server. The match goes on behind it; this computer's players stand still while it, the chat line or the on-screen keyboard is open.
- Gamepad as keys: d-pad or left stick are the arrows (a held direction repeats after 400 ms, then every 130 ms; the stick counts from half deflection and is released below 30 %), A confirms, B goes back, X erases, Y is a space, Start opens the menu (in the lobby: jumps to the actions). During a round only Start is a key; the rest steer the players.
- On-screen keyboard: opens when a text field is chosen, or the chat opened, with a gamepad. Ten columns: digits, three rows of letters, a row of marks (`- _ : @ / ! ( ) + =`), then Shift, Space, Erase, Done; a field of digits (the port) has the digits, Erase and Done. It moves round the edges. Done (or Start) ends the typing and sends a chat line; B ends it too, keeps a field's text and drops a chat line. A real keyboard goes on typing directly while it is shown. On a phone, typing by touch uses the phone's own keyboard.

## Test plan

- Core: state saved and loaded into a fresh `World` continues identically; the hash changes when any gameplay field changes.
- Codec: every message survives encode/decode; truncated messages are refused.
- Loopback: a server and two clients in one process over 127.0.0.1 play a round to its result with equal hashes; chat arrives; a wrong password is refused; a forced mismatch is repaired by a snapshot; a late joiner reaches the same state; with UDP blocked the round still completes over TCP.

## Limits accepted for now

- Prediction covers the picture and the sounds of one's own actions; everything else (explosions, other players) is heard with the round-trip delay, and other players' changes of direction appear as small corrections.
- Port opening needs a router with UPnP switched on; it was tested against a simulated router only.
- Clients trust the server's snapshots (see `../migration/networking.md`).
- A first visit to a server is taken on trust: see "Encryption and the password".
- No public relay exists; a relayed game runs over TCP only.
- Different versions of the program must not be mixed: the protocol version is raised whenever the simulation changes.

Not covered by automated tests: behaviour on real links with loss and delay, NAT traversal (the host must be reachable on the port), more than a handful of clients.
