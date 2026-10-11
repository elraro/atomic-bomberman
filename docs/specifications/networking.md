# Network play (modern design)

This is **not** the original's network mode. The original (`../reverse-engineering/networking.md`) talks IPX or serial, lets every machine simulate its own players and was only analysed statically; none of it is reproduced. This document specifies a new client-server mode for the modern game. Everything here is this project's own design; nothing is a claim about the original.

**Where this is going:** `online-play.md` specifies the target for internet play (a master server, NAT traversal with STUN/TURN, certificates, match records). Its first three phases are in place and described here (gamepad screens, the link, snapshots); this document describes what the game does today.

## Goals

- Play over a LAN or the internet with TCP/IP (IPv4 and IPv6): up to 10 players, up to four at one machine, the rest computer players.
- One server decides everything. It can run inside the game ("Start Network Game") or alone as a dedicated program (`atomic_server`) with no window, no SDL and no graphics.
- Lobby with player list, match settings and chat; chat during the match as well.
- The gameplay rules are exactly those of the local game: the same core (`src/game`) runs the round.

## Model: snapshots

Since protocol version 8 (phase 3 of `online-play.md`; before that the server sent everybody's inputs and every client worked the round out for itself, "lockstep").

The server is the only place where the round is played. Clients send what their players hold down and are sent what the field looks like.

1. The server builds the round (`RoundSetup`: tuning values, scheme, level extras, options, seats) and sends it to every client, so that each has the round's rules. What is on the field comes next, as the first state.
2. Every 50 ms the server takes each player's input for that step, lets its computer players decide theirs, and advances its `World` one step.
3. After each step it sends every client a **snapshot**: the public state after the step, and what happened in it.
4. A client shows the newest state it has been sent. It works out nothing of the round's course itself, except a guess at its own players' next few steps (prediction, below).

**The public state** is everything in the `World` except what a player may not know yet: which powerup lies under which brick (hidden powerups are simply absent) and the random generator. So a changed client can see nothing more than its screen shows, and nothing decided by chance before it happens. One timer in it says when the next dud bomb may occur.

**Inputs.** A client names the step each input is meant for: the step its picture is showing (the newest state plus its delay, see prediction). Each input datagram carries the input for that step and for up to seven steps before it, so a lost datagram loses no key press. The server gives each step the input sent for it; if none has come, the player goes on holding what they held last. An input that arrives after its step counts from then on. An input is six keys (four directions, two buttons) for each of the up to four players at that computer; a client can say nothing else.

**Snapshots as differences.** The public state is written as a row of about 5000 numbers, always in the same places (`World::snapshotFields`). Ages that only grow with the clock (a flame's, a fuse's, a disease's) are written as the moment they began, so they do not change from step to step.

- Each input datagram also says which state the client holds (the newest it has been able to put together).
- The server sends the newest state as its **difference** from that one: for each number that differs, how many places were skipped and by how much it changed. It keeps the states of the last 64 steps (3.2 s) to make differences from, the same for every client.
- A client keeps the states it was sent over the same span, applies a difference to the one it names, and ignores a datagram that is older than what it has or names a state it does not hold. Its next input datagram tells the server where it stands, so nothing has to be asked for again.
- The **whole state** goes as a message (about 3.6 KB): at the start of a round, to somebody who connects in the middle of one, and to a client that has fallen further behind than 64 steps.
- A difference too large for a datagram goes as a message.

Measured in ten-player rounds of computer players (`ab_net_tests`, "snapshot fields"; 6088 steps): 123 bytes on average against the step before (571 at most), 283 against five steps before (844 at most), 469 against twenty steps before (1052 at most; 9 steps in 6088 over 1000 bytes). The whole state: 3572 bytes on average. So in ordinary play a snapshot is one datagram of a few hundred bytes, 20 times a second for each client.

**Events.** Sounds and one-off animations come from what happened in a step (a bomb dropped, a death, a pickup), which clients no longer work out. Each difference carries the events of every step after the state it is made from, and a client takes those of steps newer than the newest state it holds. So each event is given out exactly once whatever is lost; a client that needed the whole state has missed the events before it.

Cost: more traffic than lockstep's ten bytes a step, and the state shown lags by the trip from the server. Prediction hides that for the player's own moves. What it buys: a lost datagram is simply replaced by the next; clients need not compute the same thing as the server; nothing has to be withheld by special means, it is just not sent.

Computer players run on the server only. A player who disconnects during a round is taken over by a computer player until the round ends.

## Client-side prediction

What a player sees is not the confirmed state but a guess at the state a few steps later: the one in which the keys pressed now will take effect.

- The client keeps the state the server sent as it is; nothing predicted ever enters it. Scores, results and sounds come from the server's word alone.
- Once per 50 ms of its own clock the client copies the confirmed state and runs the copy forward to a target step: the last confirmed step plus the delay, where the delay is the measured round trip in steps plus one (1 to 10 steps). For those steps it uses its own inputs, the very ones it has sent the server for those steps, and assumes every other player goes on doing what they did in the last confirmed step. It runs them with the same gameplay core as the server, with random numbers of its own.
- Other players are **drawn where the server last confirmed them**, not where their last movement would carry them: nobody knows their next move, and carrying the last one on made a player who turns about (a cornered computer player above all) seem to dart to and fro at speed.
- Guesses about chance are not shown: after running ahead, the copy gives up every powerup and regrown brick the confirmed state does not have, shows a player who died only in the guess as still confirmed (which death animation plays is chance), and keeps campaign enemies where they are confirmed. Such things appear when the server's word arrives.
- The target advances by one per local step and is pulled back to "confirmed + delay" only when it has drifted by more than two, so the picture moves evenly even when the server's states arrive unevenly.
- The copy is thrown away and rebuilt every local step, so a wrong guess (another player turned, a bomb was dropped) lasts only until the server's next state says otherwise; the drawing interpolates over one step, which softens the correction.
- No prediction for watchers, after the round is decided, or when switched off (Options, "Network: Show Own Moves At Once"; `net_prediction` in the settings file).

Sounds follow the confirmed state, with one exception: the player's own actions (dropping, punching, grabbing or throwing a bomb, picking up a powerup, a trampoline, a warp) are heard from the predicted step, at once, and are left out when the server confirms them. A predicted action that never comes true has been heard for nothing; it is forgotten after 1.5 s.

Known effect: other players are seen a little in the past (by the round trip) while one's own player, the bombs and the flames are seen in the present, so at high delay an opponent can seem to stand in a flame for a moment before falling. An earlier version carried other players' last movement forward instead; a player who kept turning about (a cornered computer player above all) then seemed to dart to and fro at speed.

## Transport

One port number (default **27410**), UDP. Since protocol version 7 a connection is a **link** (`src/net/link.*`; phase 2 of `online-play.md`): encrypted packets of at most 1200 bytes that offer two services.

| Service | Used for |
|---|---|
| Messages: reliable and ordered, any size up to 1 MiB | Joining, lobby state, settings, chat, round start, round result, snapshots. |
| Datagrams: unreliable, unordered, at most 1100 bytes | The two things that repeat during a round: inputs to the server, snapshots to the clients. |

How the link delivers messages: they are written one after another into a stream (`u32` length, `u8` type, payload), the stream is cut into numbered segments of at most 1000 bytes, and every packet carries the number of the next segment its sender is waiting for and a bit for each of the 32 after it that has already come. A segment not acknowledged is sent again after one and a half times the measured round trip plus 10 ms (between 40 ms and 500 ms, a quarter longer with each repeat, 1 s at most), or at once when two later ones have been acknowledged past it, or when the other side is heard again after a second of silence. At most 64 segments are under way. A datagram never waits for any of this. With nothing to say, a packet goes out once a second; 15 s of silence is a lost connection. Going away is said in a last packet, after everything before it has been delivered.

Round traffic needs no repeats of its own: an input datagram carries the last eight inputs, and a snapshot lost is replaced by the next (see "Model").

Every server, whatever its own port, also listens on UDP **27409** (a port several servers on one machine can share) and answers searches of the local network there.

**Where UDP does not get through.** A client that hears nothing over UDP for two seconds opens a TCP connection to the same port number and sends the very same packets through it, each as one frame (`u32` length, `u8` type `0x7E`, the packet). The link above is unchanged; only the carrier differs. Play is then less smooth, since a lost TCP segment delays everything behind it. A relay (below) carries connections this way too. This carrier goes away when TURN over TCP replaces it (`online-play.md`, phase 5).

IPv4 and IPv6: a server listens for both on one port where the system allows it (IPv4 alone otherwise); a client uses whichever the address it was given resolves to. An IPv6 literal with a port is written `[address]:port`. A link-local IPv6 address is given with its interface, `[fe80::1%eth0]:port`.

Protocol version 9 (1: one player per client; 2: several players, prizes, campaign stages; 3: encryption; 4: secrets kept on the server; 5: server identity; 6: ready state; 7: the link, everything over UDP; 8: snapshots instead of lockstep; 9: player keys, the certificate message).

## Encryption and the password

Everything a connection carries is encrypted and authenticated, except the two key messages that set it up and searches on the local network.

1. The client's first packet, in the clear: `ABMU`, `0x81`, the protocol version, a fresh X25519 public key and a 16-byte cookie (zeros the first time), filled up to 96 bytes.
2. A server that does not find its own cookie in it answers with one (`0x84`, 16 bytes: a keyed hash of the sender's address that changes every ten seconds) and remembers nothing. The client says its first packet again with the cookie. So a stranger's packet costs the server no work and no memory, a forged sender address gets nowhere, and no answer is longer than what was sent. (Inside a TCP connection there is no cookie: the connection itself shows the address.)
3. The server then answers in the clear with its own fresh public key, its identity key and the connection's number (`0x82`), or with a refusal if the versions differ (`0x83`, text). The client repeats its first packet every 250 ms until an answer comes; the server answers a repeat with the same answer.
4. Both compute the shared secret and from it, with both public keys, a master value (SHA-256) and one key for each direction.
5. From then on every packet is `ABMU`, `0x80`, the connection's number, a packet counter, then ChaCha20-Poly1305 of the contents under the key of its direction (header authenticated). A receiver takes each counter once: higher than all before, or among the last 64 and not seen. A packet that does not open, or was seen, is dropped without an answer; to the link that is a lost packet. The connection's number only names the connection; knowing it gives nothing. A packet that opens may come from a new address (a router that changed its mapping), and the answers follow it there.
6. **Password.** The client never sends it. Hello carries HMAC-SHA-256 keyed with a hash of the password over the connection's master value: proof of knowing the password that is useless on any other connection. The server compares it in constant time.

What this gives: someone who can read the traffic learns nothing (not the chat, not the names, not the password) and cannot later decrypt a recording even with the password, because the keys exist only for that connection. Someone who can alter the traffic can only delay or stop it: an altered packet counts as lost and is sent again.

**Server identity.** A server has a lasting X25519 key (kept in its identity file, made on first start); its public half is sent with the key exchange and shown as a fingerprint such as `3F9A-11C0-7B42-E5D8` (in the server's log and in the lobby). The connection's keys are derived from the fresh exchange *and* from the secret between the client's fresh key and that lasting key, so whoever shows an identity without owning it never gets the keys. A client remembers the identity of each address it has joined (`known_servers.txt`); if the server at an address later shows another identity, the client refuses to connect and says so, and the player can choose to forget the old one. Servers on the same machine and games joined through a relay are not remembered.

What this does not give: the first visit to a server is taken on trust. An attacker in the middle at that very first connection can pose as the server (with a password set, such an attacker still cannot join the two halves, but can test password guesses against the proof it receives, so a guessable password falls to it). There are no certificates and no authority vouching for a server; comparing the fingerprint with one the owner gave you by other means is the only check of a first visit. The algorithms are implemented in this project (`src/net/crypto.*`) and pass their specifications' test vectors; the code has not been audited.

## Bans and limits

- **Administrator**: whoever has been connected longest, until the role is handed over (`/admin NAME`) or taken with the server's administrator password (`/login PASSWORD`, if the server was started with one). When the administrator leaves, the role falls back to whoever has been there longest.
- **Kick**: the player's address may not connect for five minutes.
- **Ban** (administrator: `/ban NAME`; dedicated server console: `ban NAME`): the address stays out until `unban ADDRESS`, and so does the player's key if they showed one (`unban key:...`). Bans are kept in the server's ban file and survive a restart. The administrator is told the address when banning.
- At most 10 connections from one address at a time (`maxPerAddress`).
- More than 20 connection attempts from one address in ten seconds: blocked for a minute.
- Five wrong passwords from one address in a minute: blocked for five minutes.
- More than 400 messages a second on a connection (a client sends about 40): dropped, and the address blocked for a minute.
- At most 30 answers a second to LAN searches, and 200 cookies a second.
- A connection that has exchanged keys and then does not join within five seconds is forgotten.

All of these go by address, so players behind one router share them, and an address is not a person: a banned player with a new address is back.

### Messages

Carried by the link. Integers are little endian; strings are `u16 length` + UTF-8 bytes.

Client to server:

| Type | Payload | Meaning |
|---|---|---|
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
| Continue | | Key pressed on the result screen. |
| Pong | `u32` | Echo of Ping. |

Server to client:

| Type | Payload | Meaning |
|---|---|---|
| Roulette | seed, gold player, prize, frames | The bonus wheel is shown before the match. |
| Welcome | client id, session number, server name | |
| Reject | reason text | Wrong version, wrong password, server full, kicked. |
| Lobby | phase, administrator, settings, ten seats, client list | Sent whenever any of it changes. |
| Chat | sender id (255 = server), sender name, text | |
| RoundStart | round id, tuning values, `RoundSetup`, scores | Always followed by a Snapshot of step 0. |
| Delta | as the datagram | Only when a difference is too large for a datagram. |
| RoundEnd | round id, winner, match winner, wins and kills per seat | The server's verdict; clients show the result screen. |
| Snapshot | round id, step number, the whole public state (as a difference from nothing) | Start of a round, late joiners, a client too far behind. |
| Ping | `u32` | Once a second; the measured times are in the Lobby message. |

### Datagrams

Carried by the link, sealed like everything else.

| Type | Direction | Payload |
|---|---|---|
| Input | C to S | round id, the newest state held (if any), the step the first input is meant for, up to eight inputs of four bytes (one per player at that computer), newest first |
| Delta | S to C | round id, step, the step of the state it is made from, events (steps back, kind, player, value), the difference |

In the clear on the discovery port (`u32` magic `ABMU`, `u8` type, payload):

| Type | Direction | Payload |
|---|---|---|
| Query | broadcast to the discovery port **27409** | protocol version |
| Info | S to C | protocol version, port, server name, players, seats, phase, password needed |

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
2. A player joins with the address `CODE@relay-address`. The relay tells the host, the host opens one more connection out, and the relay links the two: from then on it copies every frame from one to the other (each frame is one packet of the link).
3. Through that link the player and the host do the usual key exchange. The relay carries only encrypted frames; it cannot read or change the game, and it cannot pose as the host to someone who knows the host's identity or shares a password with it. It can refuse or drop the link.

Everything goes inside TCP (the relay carries no UDP), so play is as where UDP does not get through. The host's bans and limits see the player's real address, which the relay passes on. There is no public relay: somebody in the group has to run one on a reachable machine. There is no UDP hole punching.

## Client

- Sends its inputs (first keyboard set, or the first gamepad) whenever they change, whenever a newer state has arrived, and at least every 50 ms.
- With prediction: takes up the server's states as they arrive and paces the picture by its own clock. Without (watchers, or prediction off): takes up one state per 50 ms; with more than 3 waiting two per 50 ms, with more than 20 as fast as it can. The drawing glides from the state before to the newest over one step.
- Screens: join (name, address, servers found on the LAN, the two online entries), host (server name, port, password, relay, announce online), browse (the master's list of games), join by code, lobby (seats, settings, chat; the code of a game announced online), match, result.
- Chat: typed directly in the lobby; in a match `T` opens the line, Enter sends, Esc closes. The last lines stay on screen for 8 s.
- Everything a screen can do is an item that can be chosen (phase 1 of `online-play.md`), so that a gamepad is enough:
  - Join: the three fields, "Join this address", "Browse online games", "Join online by code", the servers found, "Back". Host: the five fields, "Announce online" (no / listed for everybody / by its code only), "Start the server", "Back". Browse: the games, "Refresh", "Back". Code: the six digits, "Join", "Back". The box shown while connecting has "Cancel"; an error has "Ok" and, for a changed server identity, "Forget the old identity".
  - Lobby: the settings, and under them a row of actions: Start (administrator) or Ready (the others), Team (one per player at this computer, with team play), Players (one more at this computer with each press; after four, one), Chat, Leave. The cursor starts on the first action. A line above the settings says what the lobby waits for.
  - Match and result: a menu (Start on a gamepad) with Resume, Chat, Back To Lobby, Leave The Server. The match goes on behind it; this computer's players stand still while it, the chat line or the on-screen keyboard is open.
- Gamepad as keys: d-pad or left stick are the arrows (a held direction repeats after 400 ms, then every 130 ms; the stick counts from half deflection and is released below 30 %), A confirms, B goes back, X erases, Y is a space, Start opens the menu (in the lobby: jumps to the actions). During a round only Start is a key; the rest steer the players.
- On-screen keyboard: opens when a text field is chosen, or the chat opened, with a gamepad. Ten columns: digits, three rows of letters, a row of marks (`- _ : @ / ! ( ) + =`), then Shift, Space, Erase, Done; a field of digits (the port) has the digits, Erase and Done. It moves round the edges. Done (or Start) ends the typing and sends a chat line; B ends it too, keeps a field's text and drops a chat line. A real keyboard goes on typing directly while it is shown. On a phone, typing by touch uses the phone's own keyboard.

## Games found through a master server

Phase 4 of `online-play.md`; how the service is run is in `../operations/online.md`, the master's calls in `../../master/README.md`.

- The game fetches the signed configuration (never kept from an earlier day), opens a guest session with the master it names, and from then on asks it: the list, a game by its code, how to reach a game.
- A hosted game set to be announced tells the master its name, seats, phase, scheme, identity key and port when any of them changes and every ten seconds; the master gives it a six-digit code, shown in the lobby. It is withdrawn when the host stops. A game announced online has no password.
- Joining: the master names the ways to the game server (the address it saw the host at with the port the host named, then any the host added) and the server's identity key. The game tries them in order until one answers, and **refuses a server that shows another identity** than the master named; the memory of servers visited is not consulted for such a game.
- The dedicated server does the same with `--online listed` or `--online code`.
- A copy of the game built without the online library (`-DAB_ONLINE=OFF`) says so when an online entry is chosen.

## Test plan

- Core: state saved and loaded into a fresh `World` continues identically; the hash changes when any gameplay field changes.
- Codec: every message survives encode/decode; truncated messages are refused.
- Link: messages complete and in order over a line that loses 40 % of the packets, repeats and delays others; datagrams never held back; altered, foreign and repeated packets refused.
- Connection: loss from the very first packet; no UDP at all (the client goes on inside TCP); the cookie.
- Online: `ab_online_tests` (JSON, Ed25519 against RFC 8032's vectors, signed documents, the configuration's rules, the HTTP client, the master client against a stand-in master); `ab_ui_tests` "online screens by pad"; `tests/integration/online.sh` with the real master over TLS.
- Loopback: a server and clients in one process over 127.0.0.1 play rounds to their results and end on the server's state; chat arrives; a wrong password is refused; a late joiner comes in with one whole state; nothing hidden is on any client; events are told once each at 40 % loss; a client cut off for five seconds is given the whole state again.
- Snapshots: a world rebuilt from differences is the public state; the coder refuses nonsense; sizes are measured and printed.

## Limits accepted for now

- Prediction covers the picture and the sounds of one's own actions; everything else (explosions, other players) is heard with the round-trip delay, and other players' changes of direction appear as small corrections.
- Port opening needs a router with UPnP switched on; it was tested against a simulated router only.
- Clients trust the server's snapshots (see `../migration/networking.md`).
- A first visit to a server is taken on trust: see "Encryption and the password".
- No public relay exists; a relayed game runs over TCP only.
- Different versions of the program must not be mixed: the protocol version is raised whenever the state or the rules change.
- A client shows other players and bombs as of the newest state received, with no extra delay to even out late datagrams; on a line with much loss their movement is uneven. (`online-play.md` names a buffer of about two steps; whether it is wanted is to be judged on real connections.)

Not covered by automated tests: behaviour on real links with loss and delay, NAT traversal (the host must be reachable on the port), more than a handful of clients.
