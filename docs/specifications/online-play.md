# Online play over the internet (target design)

Status: **phase 1 is implemented** (the screens of today's network mode worked with a gamepad, the on-screen keyboard, the ready state; described in `networking.md`). Nothing else in this document is implemented yet. It describes where the network mode is going. What the game does today is in `networking.md` (server-driven lockstep, TCP + UDP to an address, a TCP relay), which stays valid until the phases below replace its parts.

Like `networking.md`, this is the project's own design. Nothing here is a claim about the original game, whose IPX/serial network mode is not reproduced.

## Goals

- Reliable internet play for up to 10 players, including players behind home routers (NAT) and carrier-grade NAT, without anybody forwarding ports by hand.
- Games are found through a list, a code or an invitation, not by exchanging addresses.
- Community tournaments and statistics are possible later without redesigning what is built now.
- The server stays the only authority over a match. Clients send inputs and draw; they never decide outcomes.
- Everything normal in the online flow can be done with a gamepad.

## Decisions taken (2026-10-06)

| Question | Decision |
|---|---|
| Game model | **Snapshots**: the server sends state, clients interpolate and predict. Replaces lockstep. |
| Hosting | The project owner hosts the master server, its database and the STUN/TURN server. |
| Statistics | Recorded only for logged-in players in matches run by dedicated servers the master trusts. |
| Networks that block UDP | **TCP is kept as the last resort**, shown to the player as such. |
| Master server language | Go, with PostgreSQL. |
| STUN / TURN | Existing implementations: a library in the game, `coturn` as the server. No protocol of our own. |

## The four parts

```text
                     HTTPS (control only)
   Game client  <------------------------->  Master server  ---  PostgreSQL
        |                                        ^
        |  STUN / TURN (standard)                | HTTPS (register, heartbeat, results)
        +------------------>  coturn             |
        |                                        |
        |  game transport (datagrams)            |
        +---------------------------------> Game server (authoritative)
                                            in the host's game, or dedicated
```

1. **Master server**: accounts, lobbies, exchange of connection details, later tournaments and statistics. HTTP only. It never carries gameplay traffic and knows nothing of the simulation.
2. **Connectivity discovery**: STUN to learn public addresses, ICE checks to find a working path, TURN when there is none.
3. **Game transport**: datagrams between each client and the game server, with a small reliable channel beside them.
4. **Authoritative game server**: the existing `src/net` server and the gameplay core. It runs in the host's game or as `atomic_server`.

The relay (TURN) is none of these: it forwards encrypted packets and understands nothing of the game.

## 1. Master server

A separate program in its own folder (`master/`), written in Go, storing everything in PostgreSQL. It shares no code with the game; the game talks to it through the HTTP API below and nothing else.

### API (version 1, JSON over HTTPS, paths under `/v1`)

Sessions and accounts (accounts are optional; a guest session is enough to play):

| Call | Purpose |
|---|---|
| `POST /session` | Guest session: returns a session token. Body: display name, game version. |
| `POST /account` | Register: username, email, password. |
| `POST /account/verify` | Confirm the email address with the code sent to it. |
| `POST /login`, `POST /logout` | Session for an account. |
| `POST /account/password-reset`, `.../confirm` | By email. |
| `DELETE /account` | Removes the account and its personal data. |

Lobbies:

| Call | Purpose |
|---|---|
| `POST /lobbies` | Create: name, visibility (public / by code), password flag, settings summary, the server's identity key. Returns lobby id, a short join code, a host token. |
| `GET /lobbies` | List public lobbies: name, players/seats, phase, level, version, region hint, whether a password is needed. Paged. |
| `GET /lobbies/by-code/{code}` | Look one up by its code. |
| `POST /lobbies/{id}/join` | Returns a **join ticket** (below), the host's connection candidates and relay credentials. |
| `POST /lobbies/{id}/leave` | |
| `PUT /lobbies/{id}` | Host: update metadata (players, phase, settings). Doubles as heartbeat; a lobby not heard from for 30 s is removed. |
| `DELETE /lobbies/{id}` | Host: close. |
| `POST /lobbies/{id}/signal`, `GET /lobbies/{id}/signal` | Exchange of connection candidates between the host and one joining client (long poll). |

Infrastructure:

| Call | Purpose |
|---|---|
| `GET /status` | Operational / maintenance state, message of the day, minimum game version. |
| `POST /relay-credentials` | Short-lived TURN username and password for this session. |
| `POST /servers/heartbeat` | A trusted dedicated server announces itself and its load. |
| `POST /matches` | A trusted dedicated server reports a finished match. |

Later, without changing the above: `GET /players/{name}/stats`, `GET /matches/{id}`, and a `/tournaments` family (create, register, bracket, assign match to server, report). They read and write the master's own tables only.

### Join tickets

The game server must know who is joining without asking the master during play. On `join` the master returns a ticket: lobby id, session id, account id (or none), display name, expiry (two minutes), signed by the master with Ed25519. The client presents it in its first message to the game server, which checks the signature with the master's public key from the configuration (section 2). A game hosted without the master (LAN, direct address) uses no tickets, as today.

### Trusted dedicated servers

- The operator of a dedicated server registers its identity key with the master (an administrative action; no self-registration).
- Such a server authenticates its heartbeats and match reports with a token the master issued for that key.
- Statistics are written only from these reports, and only for players whose ticket carried an account. A game hosted on a player's machine is never recorded: its host could forge the result.
- Tournament matches are assigned to trusted servers for the same reason.

### Data (outline)

`accounts` (id, username, email, password hash, verified, created), `sessions`, `lobbies` (ephemeral; may live in memory and be mirrored), `servers` (identity key, operator, token hash, last seen), `matches` (id, server, started, ended, level, scheme, settings), `match_players` (match, account, seat, team, wins, kills, deaths, result), later `tournaments`, `tournament_entries`, `tournament_matches`.

### Rules for the master

- Passwords are stored as Argon2id hashes. Email addresses are used for verification and password reset only.
- Rate limits per address and per session on every write call.
- All game-facing calls work for guests except those under `/account`.
- The master may be down: LAN play and joining by address keep working without it.

## 2. Configuration

The game holds a small built-in **bootstrap**: one or two URLs of the configuration document and the Ed25519 public key that signs it.

The document is JSON with a detached signature, fetched over HTTPS when the player opens online play:

```text
serial, issued, expires
master: URL, signing key for join tickets
stun: list of host:port
turn: list of host:port (UDP), credentials come from the master
tcp_relay: list of host:port
protocol: lowest and current game protocol accepted
status: ok / maintenance, message
flags: free-form switches
```

- A document is used only if its signature verifies, it has not expired and its serial is not lower than the last one accepted (no replay of an old one).
- The last good document is cached and used when the endpoint cannot be reached.
- A command-line option and a settings key point the game at another bootstrap URL and key, for a self-hosted community.
- In the code this is one small interface (`OnlineConfig load()`), so that the source can change later without touching its users.

## 3. Connectivity

### Library and server

- In the game: **libjuice** (C, MPL-2.0, UDP only), which implements STUN, ICE connectivity checks and a TURN client. To be confirmed in phase 3 by building it for every target (Linux and Windows on x86-64 and ARM64, macOS universal, Android). If it fails a target, the fallback choice is `libnice` or `pjnath`; writing STUN ourselves is not an option.
- On the server side: **coturn**, serving STUN and TURN on the owner's machine. TURN credentials are time-limited ones derived from a secret shared between coturn and the master (coturn's `use-auth-secret` mode), so a credential leaked from one session is useless an hour later.

### Flow

```text
Host                                  Master                               Client
 create lobby  ---------------------->
 gather candidates (local, STUN, TURN)
 publish candidates  ----------------->
                                        <----------------------  join lobby (ticket, credentials)
                                        <----------------------  gather + publish candidates
 <--- client's candidates -------------  ---- host's candidates ----------->
 ICE checks  <============== UDP, both directions ===============>  ICE checks
 path chosen: direct, or through TURN
 application handshake over the chosen path (key exchange, ticket, version)
```

- One ICE connection per client, host at the centre (a star). Clients never connect to each other.
- A dedicated server with a public address publishes that address; clients reach it directly and ICE finds that path first.
- The handshake that follows is the game's own (key exchange, server identity, ticket): a path counts as working only when that handshake has completed in both directions.

### The ladder

Tried in this order; the first that works is used:

1. **Direct UDP** (same LAN, public address, or hole punched through both routers).
2. **TURN over UDP** through coturn.
3. **TCP, last resort**: to the server's address if it has a reachable one, otherwise through the project's TCP relay (the existing `--relay-server`, kept for this). Chosen only when no UDP path came up within the time limit.
4. No viable connection: said plainly, with what was tried.

### What the player sees

The lobby shows, per player, the path and the measured delay: `Direct`, `Relay`, `Relay (TCP), may stutter`. The test runs when joining a lobby, before any match starts, and takes a few seconds at most.

No fixed-size probe (such as 32 KB) is sent. Reasons: the ICE checks plus the game's handshake already prove both directions; the lobby then exchanges traffic for as long as players wait, which exposes paths that die after a while; and a path that fails later is handled by keepalives and stepping down the ladder during play. If real use shows firewalls that only interfere after some volume, a measured probe can be added to the lobby phase; that would be decided on evidence.

## 4. Game transport

One abstraction, a **link** between a client and the server, with two services:

- **Datagrams**: unreliable, unordered, each at most 1200 bytes.
- **Messages**: reliable and ordered, for anything that must arrive.

Two kinds of link provide it:

| Link | Datagrams | Messages |
|---|---|---|
| UDP link (direct or through TURN) | as they are | sequence numbers, acknowledgements and resending, built into the link |
| TCP link (last resort) | written to the stream (so they arrive late rather than never) | written to the stream |

Today's design has TCP beside UDP on a known address. A hole-punched path has no TCP beside it, which is why the reliable channel moves into the link.

What goes where:

| Datagrams | Messages |
|---|---|
| Inputs (client to server) | Handshake, ticket, version |
| Snapshots (server to client) | Lobby state, settings, ready state, chat |
| Keepalive and delay measurement | Round start with the full state, round result |
| | Anything a client must not miss |

A new datagram never waits for an old one. Encryption stays as specified in `networking.md` (fresh keys per connection, every datagram and message authenticated); TURN and the TCP relay carry ciphertext only.

Cryptography note: Ed25519 verification is new. It and, in time, the existing home-made primitives should come from **libsodium** rather than from this project's own code.

## 5. Game model: snapshots

### Server

- Runs the gameplay core at its fixed 50 ms step, as now. It is the only place the rules are applied with authority.
- Each client's input datagram carries its latest input **and the previous few** with their numbers, so a lost datagram does not lose a key press. The server applies, per step, the input meant for that step if it has it, otherwise the last one it has. Inputs are six bits (four directions, two buttons); there is nothing else a client can assert.
- After each step it sends every client a **snapshot**: the step number, the number of the last input of that client it applied, and the public state.

### What a snapshot holds

Only what a player may know: no hidden powerups under bricks, no random generator. That removes the "secrets" mechanism of the lockstep design.

The state today, measured over 20 ten-player rounds (49 495 steps) with the core's existing save format: 5.4 KB per full state, about 1.1 KB of it changing from one step to the next. That format is too large to send 20 times a second, so snapshots get a format of their own:

- A **baseline**: the full state, sent as a message at round start and whenever a client has fallen too far behind.
- **Deltas** as datagrams: changes against the newest snapshot that client has acknowledged. Players (position, facing, state, what they carry), bombs, flames, changed cells.
- **Events** of the step (a sound, an animation start, a death): repeated in following snapshots until acknowledged, since clients no longer compute them.
- Budget: one datagram of at most 1200 bytes per snapshot in ordinary play. Whether that holds with ten players and many bombs is to be **measured** when the format exists; if it does not, the delta is split or the rate for distant detail is lowered. No figure is claimed here.

### Client

- **Other players, bombs, flames**: drawn by interpolating between the two newest snapshots, about two steps (100 ms) in the past. A missing snapshot is bridged by the next.
- **Own player**: predicted. The client keeps its inputs not yet confirmed; when a snapshot arrives it takes the server's state and applies those inputs again with the same gameplay core, so its own moves appear at once and corrections are small.
- The client still contains the gameplay core, for that prediction and for local play. It is not trusted with anything.
- Sounds and effects come from the snapshot's events, except the player's own actions, which are heard at once as today.
- Joining in the middle of a round needs only a baseline.

### What changes compared with lockstep

| | Lockstep (today) | Snapshots (target) |
|---|---|---|
| Sent during a round | Ten input bytes per step | State changes per step, per client |
| A lost datagram | Covered by repetition; every step is needed | Skipped; the next snapshot replaces it |
| Bandwidth | Very low | Higher; to be measured (see above) |
| Identical results on every machine | Required | Not required |
| Hidden information | Withheld by the secrets mechanism | Simply not sent |
| State hash and repair | Needed | Not needed |

Cost of the change, stated plainly: the round part of `src/net` (steps, secrets, hashes, the present prediction) and its tests are rewritten, and relayed players cost the owner's TURN server more traffic than lockstep would.

## 6. Authoritative server

- A match is simulated in exactly one place: the game server.
- Hosted from the menu, that server runs in the host's game. The host therefore can cheat in their own game; such games are never recorded.
- Recorded and tournament matches run on trusted dedicated servers.
- Neither the master, nor TURN, nor the TCP relay ever simulates or alters a match.
- Limits against misbehaving clients (message rate, connection attempts, bans) stay as in `networking.md`, with accounts added as something a ban can name.

## 7. Online screens, usable with a gamepad

Every ordinary action is a visible, selectable item. Keyboard shortcuts stay as extras.

- **Online menu**: Browse Games, Create Game, Join By Code, Account, Back.
- **Browse**: a list (name, players, level, delay, lock). Up/down, confirm to join, a button to refresh.
- **Create**: name, public or by code, password, then the lobby.
- **Join by code**: an on-screen keyboard of letters and digits.
- **Lobby**: seats, settings (host), a row of actions: Ready / Not Ready, Start (host, when everybody is ready), Team, Chat, Leave. Each player's connection path and delay.
- **Text entry** (name, password, chat): the on-screen keyboard with a gamepad or touch; a real keyboard types directly.
- **Connection test**: shown while joining, with the step being tried and the outcome.
- **During a match**: a pause-style menu on Start: Chat, Back to Lobby, Leave.

The existing F-key and typed commands keep working; none is required.

## 8. Phases

Each phase is usable on its own and ends with tests.

| Phase | Content | Depends on |
|---|---|---|
| 1 | Gamepad-friendly online screens for today's network mode (menu items, on-screen keyboard, ready state). **Done.** | nothing |
| 2 | The link abstraction: reliable messages over UDP; today's protocol moved onto it | nothing |
| 3 | Snapshot model replacing lockstep, with measurements of size and behaviour under loss | 2 |
| 4 | Master server (guest sessions, lobbies, status), signed configuration, browse / create / join by code in the game | 1 |
| 5 | libjuice + coturn: discovery, hole punching, TURN, the ladder with TCP last, connection display | 2, 4 |
| 6 | Accounts, join tickets, trusted servers, match reports, statistics | 4 |
| 7 | Tournaments | 6 |

## 9. New dependencies

| Where | What | Note |
|---|---|---|
| Game | An HTTPS client | libcurl on desktop systems; on Android the platform's own through the Java side. To be settled in phase 4. |
| Game | libjuice | Phase 5. |
| Game, server | libsodium | Signatures now; the existing primitives later. |
| Master | Go toolchain, PostgreSQL | Separate program, own CI job. |
| Owner's machine | master, PostgreSQL, coturn, the TCP relay, a domain with a certificate | |

## 10. Tests

- Link: messages arrive complete and in order under 40 % datagram loss, reordering and duplication; datagrams are never delayed by a lost one.
- Snapshots: a client fed deltas with loss reaches the server's public state; own-player prediction matches the server when nobody interferes; a late joiner needs one baseline; nothing hidden appears in any snapshot.
- Master: every call against a real PostgreSQL in CI; expired and forged tickets and configuration documents are refused.
- Connectivity: against a local coturn in CI, direct and relayed paths; the ladder steps down when UDP is blocked.
- Screens: every online action reachable by scripted gamepad input alone.

What automated tests cannot show, and must be tried by people: real routers and carrier-grade NAT, real loss and delay, ten players on ten connections, the owner's server under load.

## 11. Open points

- Whether one datagram per snapshot is enough with ten players (phase 3 measures it).
- Whether libjuice builds and works on every target (phase 5).
- Region and delay hints in the lobby list need a way to measure delay to a host before joining.
- Privacy text and the handling of stored email addresses, before accounts open (phase 6).
- Whether watchers are counted against the ten connections of a host.
