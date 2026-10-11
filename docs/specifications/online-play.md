# Online play over the internet (target design)

Status: **every phase but 5 is implemented** (1: the screens worked with a gamepad, the on-screen keyboard, the ready state; 2: the link; 3: snapshots, all three described in `networking.md`; 4: the master server, the signed configuration, games found by list or by code: `master/README.md`, `../operations/online.md`; 6: server certificates and player keys; 7: signed match records; 8: tournaments as signed files, all three in section 7 below). **Phase 5 (reaching players behind routers: STUN, TURN, the relay over TCP) is not implemented**: a host must still be reachable from outside, and the project's own TCP relay is still in place. Where what was built differs from what this document first said, the difference is noted in place. It describes where the network mode is going. What the game does today is in `networking.md` (snapshots over the link, to an address, a TCP relay), which stays valid until the phases below replace its parts.

Like `networking.md`, this is the project's own design. Nothing here is a claim about the original game, whose IPX/serial network mode is not reproduced.

## Goals

- Reliable internet play for up to 10 players, including players behind home routers (NAT) and carrier-grade NAT, without anybody forwarding ports by hand.
- Games are found through a list or a code, not by exchanging addresses. An "invitation" is nothing more than that code passed on as text by whatever means the players already use; no chat platform is integrated.
- Community tournaments and statistics are possible later without redesigning what is built now, and **without a central server having to be trusted**: what a tournament needs is carried by signatures (section 7). The one thing that must be reachable is the signed configuration file (section 2).
- The server stays the only authority over a match. Clients send inputs and draw; they never decide outcomes.
- Everything normal in the online flow can be done with a gamepad.

## Decisions taken (2026-10-06)

| Question | Decision |
|---|---|
| Game model | **Snapshots**: the server sends state, clients interpolate and predict. Replaces lockstep. |
| Hosting | The project owner hosts the master server, its database and the STUN/TURN server. |
| Trust in a server | **Certificates, not a list on the master** (revised 2026-10-06): an offline key signs the identity of each trusted dedicated server; the game checks the signature by itself. |
| Checking a result | Every recorded match leaves a **signed match record** that anyone can replay with the gameplay core. Statistics are worked out from published records, not kept by the master. |
| Tournaments | **Signed files** issued by an organiser; no master involved. |
| Accounts | Optional and undecided. Nothing above needs them: in a tournament a player is a key. |
| Networks that block UDP | **TURN over TCP** (and TLS on port 443) through coturn as the last resort, shown to the player as such. The project's own TCP relay is dropped (decided 2026-10-11). |
| Configuration that cannot be fetched | **No cache**: online play is refused until it can be (decided 2026-10-11). |
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

1. **Master server**: lobbies and the exchange of connection details for casual play. HTTP only. It never carries gameplay traffic, knows nothing of the simulation, and is not what makes a server or a result trustworthy (section 7).
2. **Connectivity discovery**: STUN to learn public addresses, ICE checks to find a working path, TURN when there is none.
3. **Game transport**: datagrams between each client and the game server, with a small reliable channel beside them.
4. **Authoritative game server**: the existing `src/net` server and the gameplay core. It runs in the host's game or as `atomic_server`.

The relay (TURN) is none of these: it forwards encrypted packets and understands nothing of the game.

## 1. Master server

A separate program in its own folder (`master/`), written in Go, storing what it must keep in PostgreSQL. It shares no code with the game; the game talks to it through the HTTP API below and nothing else.

Its job is **finding each other**: the list of games, join codes, and passing connection details between players who are behind routers. It is not needed for a tournament on dedicated servers with public addresses, and it is trusted with nothing: it cannot make a server look verified or a result look genuine.

### API (version 1, JSON over HTTPS, paths under `/v1`)

Sessions (no account; a guest session is all there is):

| Call | Purpose |
|---|---|
| `POST /session` | Returns a session token. Body: display name, game version. |

Lobbies:

| Call | Purpose |
|---|---|
| `POST /lobbies` | Create: name, visibility (public / by code), settings summary, the server's identity key and, if it has one, its certificate. Returns lobby id, a short join code, a host token. |
| `GET /lobbies` | List public lobbies: name, players/seats, phase, level, version, region hint, whether the server showed a certificate. Paged. |
| `GET /lobbies/by-code/{code}` | Look one up by its code. |
| `POST /lobbies/{id}/join` | Returns the host's connection candidates and relay credentials. |
| `POST /lobbies/{id}/leave` | |
| `PUT /lobbies/{id}` | Host: update metadata (players, phase, settings). Doubles as heartbeat; a lobby not heard from for 30 s is removed. |
| `DELETE /lobbies/{id}` | Host: close. |
| `POST /lobbies/{id}/signal`, `GET /lobbies/{id}/signal` | Exchange of connection candidates between the host and one joining client (long poll). |

Infrastructure:

| Call | Purpose |
|---|---|
| `GET /status` | Operational / maintenance state, message of the day, minimum game version. |
| `POST /relay-credentials` | Short-lived TURN username and password for this session. |

### Join codes; no passwords on listed games

- A code is **six digits**, drawn with the system's cryptographic random source (in Go `crypto/rand`, never `math/rand`, which can be predicted). The same holds for session and host tokens. Digits only, so that a gamepad types them on the small keyboard.
- Looking a code up is rate-limited per address and per session, and a wrong code and an unknown one answer alike. A code dies with its lobby.
- A game made through the master has **no password**. A private game is one that is not listed and is joined by its code. Reasons: the master checks a code before it gives out anything, whereas a password is checked by the host, so the host's address would already have been handed to whoever asked; and passwords people choose are weak. Passwords stay where there is no master: a game joined by address or found on the local network (`networking.md`).

The master passes a certificate on as it got it; the game verifies it itself (section 7) and believes nothing the master says about it.

### Data (outline)

`sessions` and `lobbies`, both short-lived (they may live in memory and be mirrored). No personal data.

### Rules for the master

- Rate limits per address and per session on every write call.
- The master may be down: LAN play, joining by address and tournaments keep working without it. (Verified servers and tournaments need the configuration file, which is not the master: section 2.)

### Optional later: accounts

Not decided, and nothing else depends on it. If names are ever to be reserved, the master would gain `/account` calls (register, verify by email, login, password reset, delete; passwords as Argon2id hashes) and would sign a short-lived **join ticket** naming the account, which the game server checks with the master's key from the configuration. Statistics and tournaments do not use accounts: they go by player keys (section 7).

## 2. Configuration

The game holds a small built-in **bootstrap**: one or two URLs of the configuration document and the Ed25519 public key that signs it.

The document is JSON with a detached signature, fetched over HTTPS when the player opens online play. As built: the two travel in one small JSON envelope (the document's exact bytes in base64, the signature, the signing key), so that one fetch brings both; every signed document of this project has that form (`src/online/sign.*`, `master/internal/signed`).

```text
serial, issued, expires
master: URL, and the pins of its TLS key (see below)
trust: root keys that may sign server certificates, revoked certificate serials
stun: list of host:port
turn: list of host:port with transport (UDP, TCP, TLS), credentials come from the master
protocol: lowest and current game protocol accepted
status: ok / maintenance, message
flags: free-form switches
```

- A document is used only if its signature verifies, it has not expired and its serial is not lower than the last one accepted (no replay of an old one).
- **Nothing is cached.** If no current document can be fetched and verified, everything that rests on it is refused, with the reason shown: the list of games, joining by code, relayed connections, the "verified" mark of a server, tournaments. Outdated addresses or an outdated list of revoked certificates are worse than no connection. Playing on the local network and joining by address need no configuration and keep working.
- Because of that the file must be easy to keep reachable: it is a static signed file, the bootstrap may name several URLs for it (mirrors on different hosts), and any one that verifies will do. A mirror cannot forge it; it can only serve an older one, which the expiry (seven days at most) bounds. The highest serial seen is remembered, as a number only, to refuse a step back.
- A command-line option and a settings key point the game at another bootstrap URL and key, for a self-hosted community.
- **How the game knows the master (as built).** It does not consult the system's list of certificate authorities, which differs on every platform. The configuration names the master's TLS key by its pin (SHA-256 of the key), and the game talks to nobody else; the configuration itself may be fetched from anywhere, since its signature is what is checked. So the one thing a game trusts for online play is the root key in its bootstrap.
- In the code this is one small interface (`OnlineConfig load()`), so that the source can change later without touching its users.

## 3. Connectivity

### Library and server

- In the game: **pjnath** (the NAT library of PJSIP; C; GPL-2.0-or-later, which this project's GPL-3.0 allows), which implements STUN, ICE connectivity checks and a TURN client over UDP, TCP and TLS. To be confirmed in phase 5 by building it for every target (Linux and Windows on x86-64 and ARM64, macOS universal, Android) and by a relayed connection over TCP against coturn. If it fails, the fallback is `libnice` (needs GLib). libjuice, named in an earlier version, is UDP only and so cannot reach the relay over TCP. Writing STUN or TURN ourselves is not an option.
- On the server side: **coturn**, serving STUN and TURN on the owner's machine. TURN credentials are time-limited ones derived from a secret shared between coturn and the master (coturn's `use-auth-secret` mode), so a credential leaked from one session is useless an hour later.
- Deployment: coturn listens on **3478 (UDP and TCP) and 443 (TLS)**, and its range of relayed ports is set low (for instance 5000-9999). Reported, not verified by us: some mobile carriers drop UDP to high ports.

### Flow

```text
Host                                  Master                               Client
 create lobby  ---------------------->
 gather candidates (local, STUN, TURN)
 publish candidates  ----------------->
                                        <----------------------  join lobby (credentials)        
                                        <----------------------  gather + publish candidates
 <--- client's candidates -------------  ---- host's candidates ----------->
 ICE checks  <============== UDP, both directions ===============>  ICE checks
 path chosen: direct, or through TURN
 application handshake over the chosen path (key exchange, certificate, version)
```

- One ICE connection per client, host at the centre (a star). Clients never connect to each other.
- A dedicated server with a public address publishes that address; clients reach it directly and ICE finds that path first.
- The handshake that follows is the game's own (key exchange, server identity, certificate if any): a path counts as working only when that handshake has completed in both directions.

### The ladder

Tried in this order; the first that works is used:

1. **Direct UDP** (same LAN, public address, or hole punched through both routers).
2. **TURN over UDP** through coturn.
3. **TURN over TCP**, then **over TLS on port 443**, through the same coturn: the client's leg to the relay is a TCP stream, the relay's leg to the game server stays UDP. Chosen only when no UDP path came up within the time limit. To a network it looks like a web connection, which is what gets through where UDP does not.
4. No viable connection: said plainly, with what was tried.

### What the player sees

The lobby shows, per player, the path and the measured delay: `Direct`, `Relay`, `Relay (TCP), may stutter`. The test runs when joining a lobby, before any match starts, and takes a few seconds at most.

**Volume check.** A path is declared good only after about 64 KB has gone over it in each direction during the test (padding datagrams over two or three seconds, acknowledged by the other side). The ICE checks and the handshake prove both directions, but they are a few kilobytes, and so is the traffic of a lobby. Reported by a reviewer, not verified by us: some networks let a new UDP flow start and then drop it silently after roughly 16 KB. Without the check such a path would die in the first round instead of in the test. An earlier version of this document left the probe out pending evidence; this report is that evidence.

**Always relay.** A setting, off by default: "Hide my address (always use the relay)". With it the game offers and accepts relayed candidates only, so neither the host nor the other players learn its address; they see the relay's. It costs delay and the owner's traffic, and is said so beside the setting.

NAT type detection is not done: the old classification by STUN is unreliable, and the ICE checks answer the only question that matters (does a path work) directly.

## 4. Game transport

One abstraction, a **link** between a client and the server, with two services:

- **Datagrams**: unreliable, unordered, each at most 1200 bytes.
- **Messages**: reliable and ordered, for anything that must arrive.

There is **one kind of link**: datagrams over the path ICE chose, with messages built on top (sequence numbers, acknowledgements, resending). When the path is TURN over TCP the game still hands datagrams to the relay; only that leg rides a stream, so there a lost packet makes the following ones late instead of lost. The game needs no TCP code of its own for play.

Today's design has TCP beside UDP on a known address. A hole-punched path has no TCP beside it, which is why the reliable channel moves into the link. What goes away with it: a game joined by address on a network that blocks UDP (today it falls back to TCP). Such a player needs the relay, and so the master for its credentials.

The project's own relay (`--relay-server`, `CODE@relay` addresses; `networking.md`) stays until phase 5 works, and is removed then.

What goes where:

| Datagrams | Messages |
|---|---|
| Inputs (client to server) | Handshake, certificate, version |
| Snapshots (server to client) | Lobby state, settings, ready state, chat |
| Keepalive and delay measurement | Round start with the full state, round result |
| | Anything a client must not miss |

A new datagram never waits for an old one. Encryption stays as specified in `networking.md` (fresh keys per connection, every datagram and message authenticated); TURN carries ciphertext only.

Cryptography note: Ed25519 verification is new. It comes from an existing library (Monocypher; see section 10) rather than from this project's own code, and in time the existing home-made primitives should too.

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
- Hosted from the menu, that server runs in the host's game. The host therefore can cheat in their own game; such games carry no certificate and count for nothing outside themselves.
- Matches that count (tournaments, statistics) run on dedicated servers with a certificate and leave a signed match record (section 7).
- Neither the master nor TURN ever simulates or alters a match.
- Limits against misbehaving clients (message rate, connection attempts, bans) stay as in `networking.md`, with player keys added as something a ban can name.

## 7. Trust without the master: certificates, match records, tournaments

Added 2026-10-06, replacing "trusted dedicated servers" as a list kept by the master.

### What is not attempted, and why

A server is **not** asked to prove which program it runs (a hash of its binary, signed). The hash would be reported by the very machine in question, so a changed server would report the hash of the unchanged one. Proving what a remote machine runs needs attestation by its hardware, which community hosts do not have. The program is also open source: building a changed one takes minutes, and an honest self-built one has a different hash. So trust is placed in two things that can be checked: **who operates the server**, and **whether the published record of a match replays to the published result**.

### Keys

| Key | Kind | Held by | Purpose |
|---|---|---|---|
| Root | Ed25519 | The project owner, **offline** | Signs server certificates. Its public half is listed in the signed configuration (section 2). |
| Server identity | X25519 (exists today) | A server | Proven in every key exchange (`networking.md`). |
| Server signing | Ed25519 (new) | A server | Signs match records. |
| Player | Ed25519 (new) | A player's game, made on first use, kept in the per-user folder | Proves in a tournament that this is the listed player; signs that player's inputs. |
| Organiser | Ed25519 | Whoever runs a tournament | Signs the tournament file. |

### Server certificates

A small JSON document with a detached Ed25519 signature, in the same form as the configuration document:

```text
serial, issued, expires (short: about 90 days)
operator: name, contact
identity key (X25519), signing key (Ed25519)
issuer: the root key that signed it
```

- Issued by hand with a command-line tool that holds the root key; there is no self-registration and no online service that signs.
- The server sends its certificate as a message right after the key exchange. The client checks: the signature against a root key it holds, the dates, that the serial is not revoked, and that the identity key in it is the one the key exchange just proved. Only then is the server shown as **verified**, with the operator's name. Anything else is an ordinary server, shown as such, and playable.
- Revocation: the list of revoked serials in the signed configuration document, plus the short life of a certificate. The configuration is a static signed file; no service has to answer.
- A community may use a root of its own: the bootstrap option of section 2 already allows another configuration and key.
- A certificate says who answers for a server. It does not say the server is honest; the record does the checking.

As built (protocol version 9):

- The document carries `"kind": "server-certificate"`, and a configuration `"kind": "configuration"`: one is never taken for the other, even when the same key signed both.
- `abconfig cert` issues one (90 days unless told otherwise, 366 at most). The operator gets the two keys to name from `atomic_server --show-keys`; the server is given the certificate with `--certificate FILE` and refuses to start with one made out to other keys. A game hosted from the menu has none.
- The network library only carries the certificate (it has no signature code); the game judges it with the root keys and the revoked serials of the configuration, which it fetches for this purpose whichever way the server was found. The lobby shows "verified server of NAME", or why a certificate shown was not accepted. Without a configuration nothing can be verified, and the server is an ordinary one.

### Player keys (as built)

- A player's game makes an Ed25519 key on first use (`player.key` beside the settings).
- When joining, it sends the public half and a signature over a message bound to that very connection (a fixed label and the connection's master value), so the proof is good nowhere else. A server that can check signatures refuses a key that is not proved; one that cannot takes no notice of keys.
- The lobby state names each client's key. A ban names the key as well as the address (`key:...` in the ban list), so a new address does not bring a banned player back under the same key. (A new key does: a key is free to make. What a key is good for is being *listed*, in a tournament.)

### Signed match records

Every match on a server that has a signing key leaves a record that the server signs and hands to every player at the end (and that organisers publish):

```text
game version, protocol version, level, scheme (content hash), settings
seed, and how it came about (below)
seats: name, player key (if the player has shown one), computer or human
per step: the input applied for each of the ten seats, and for a human seat the number of that player's input
state hash every so many steps, and the final one
result: winner, wins, kills, number of steps
the players' signed input statements (below)
the server's certificate and signature
```

- **Replay.** The gameplay core is deterministic, so `atomic_verify RECORD` runs the recorded inputs through the same version of the core and must arrive at the same hashes and the same result. Inputs of computer seats are recorded like any others, so the verifier does not have to reproduce the computer players' thinking. A record can only be replayed by the game version it names.
- **The seed is not the server's choice.** Before a round each client sends a commitment to a random number, then the number; the seed is a hash over all of them and the server's own. No single party, the server included, can steer what the arena or the powerups will be.
- **Inputs are the players' own.** A client keeps a running hash over its numbered inputs and signs it with its player key at intervals and at the end of the round. The record carries these statements; the verifier checks that the inputs recorded for that seat are that sequence. The server therefore cannot invent what a player did.
- Size, estimated: ten input bytes per step at 20 steps a second is about 12 KB a minute before compression. To be measured when the format exists.
- The exact layout of the record and of the statements is settled in the phase that builds them, not here.

As built (protocol version 10; `src/net/record.*`, `src/online/record_check.hpp`, `tools/verify/`):

- **One record per round**, not per match: a round is what has one seed and can be played again. A match is its rounds' records in a row; each carries the score before and after.
- A server records when it has a signing key (every `atomic_server` makes one on first start). A game hosted from the menu records nothing, and then none of what follows happens.
- **Seed.** Before the round the server asks every player with a seat for a promise (the hash of a random number), then tells everybody all the promises, its own among them, then collects the numbers. A player who promised and does not show within 1.5 s has no part in the seed. The server's own number stays its secret until the record: the seed tells where the powerups are. Each player's game keeps the promises it was shown and the number it gave, and looks for them in the record. Known limit: a server sees the seed before the round starts and could give up a round it does not like and start another; players would see that as rounds that begin twice.
- **Inputs.** A player's game logs every different combination of keys held, in order, and about once a second sends the new part with one signature over the whole log so far (its running hash, the count, and the round's name, which is a hash of the promises); a last one when the round ends. The record holds each log and its last signature. The check: the inputs the server applied for that player's seats must be a walk forward through the log: values may be passed over (a datagram lost, a key held too briefly) but none invented and none gone back to. Steps in which the computer had the seat (the player had left) are marked and not held against the log.
- **Hashes** of the whole state (hidden powerups and random generator included) every 20 steps, and at the end.
- The record is a signed document like the others (its bytes inside the envelope), signed with the server's signing key, and carries the server's certificate if it has one. About 6 KB for a round of 150 steps with two players.
- **Who checks.** Every player's game, on being handed the record: the signature, the certificate if root keys are at hand, the replay, its own log and seed share. A record that does not hold is said so in the chat; one that holds is kept (`records/` beside the settings, the newest 200). The server keeps its own (`--records-dir`). Anybody, later: `atomic_verify [--root KEY] [--stats] FILES`.
- A record can be played again only by the release that made it (it names the release and the protocol version).

### What this catches, and what it does not

| A dishonest server that... | |
|---|---|
| reports another result than the play gave | Caught: the record does not replay to it. |
| invents or alters a player's inputs | Caught: they do not match the player's signed statement. |
| picks a favourable seed | Prevented: it does not choose the seed alone. |
| drops or delays one player's inputs | **Not caught** by the record (the record truthfully shows a held input). The player sees it; the operator answers for it by name. |
| tells one player what is hidden (the operator sees everything) | **Not caught.** |
| is run by somebody who also plays in the match | **Not caught**; a tournament's rules should forbid it. |

Cheating by a *player* (a program that plays for them) is outside all of this.

### Tournaments as signed files

A tournament is a JSON file signed by its organiser:

```text
name, organiser, serial, issued
rules: settings, scheme (content hash), wins needed, game version
players: name and player key each
servers: certificates (or bare keys the organiser vouches for)
bracket: matches, who plays whom, on which server, when
results so far: per finished match, the hash of its match record and where to fetch it
```

- The organiser publishes it anywhere (a web page, a file handed round) and issues it again with a higher serial as results come in. The game opens it from a file or a URL, checks the signature, shows the bracket, and connects a player to the server of their match.
- A server given the file admits to a tournament match only the listed players: each proves their player key by signing the handshake. No accounts and no master are involved.
- Whose organiser key to believe is the player's choice (it comes with the invitation), as with any tournament. The project's root does not sign tournaments.
- **Statistics** are worked out by anybody from published records; a tool sums them. Nothing has to store them centrally. The master may later list links to tournaments; it would be a notice board, not an authority.

As built (`src/online/tournament.*`, `master/internal/tournament`, `abconfig tournament-*`):

- The file is a signed document of kind `tournament`: name, organiser, serial, rules (release and protocol the matches are played with, round wins, level, scheme, play time, enclosement, team play, computer players), players (name and key), servers (name, address, identity key, signing key: bare keys the organiser vouches for), matches (id, round, players, server, time, winner, records by hash and address). A player of a later round who is not known yet is written `@ID`, "the winner of match ID".
- **The organiser** makes it with `abconfig tournament-new` (a knock-out bracket from a list of players; whoever is left over in a round goes through), sets times and servers by hand, signs it with `tournament-sign`, and after each match writes the result in with `tournament-result` (the hashes of the match's records; the winner's name into the next round) and signs it again. Players send their key (the game shows nothing of it yet; the file `player.key` holds the private half) and server operators the two keys of `atomic_server --show-keys`.
- **A server** is started for one match: `atomic_server --tournament FILE#ORGANISERKEY --match ID`. It takes the rules from the file and does not let them be changed, admits only the match's players (each must prove their key; one connection each), refuses to serve a match whose players are not all known, and refuses if its own two keys are not the ones the file names for that match. It needs a signing key: every round is recorded.
- **A player** opens the file in the game (Join Network Game, Tournament: a path or an address; an invitation `LOCATION#KEY` names the organiser's key at once). A file whose signer the player has not chosen to believe is shown with the signer's fingerprint and nothing can be played from it until the player says "this is my organiser's key"; the key and the newest issue seen are remembered, an older issue is refused, and a file signed with another key counts as not yet believed. "Play my match" connects to the server the file names for the player's next match, which must show the identity the file names.
- **Anybody** holds the stated results against the records: `atomic_verify --tournament FILE#KEY RECORDS...` says for each match with a winner whether its records are among the files (by hash), hold, were signed by the server named for the match, were played by the match's players and nobody else, and end with the stated winner.
- Not built: schedules beyond a time per match, group stages, seeding, a way to show one's own player key in the game, tournaments listed by the master.

## 8. Online screens, usable with a gamepad

Every ordinary action is a visible, selectable item. Keyboard shortcuts stay as extras.

- **Online menu**: Browse Games, Create Game, Join By Code, Tournaments, Back.
- **Browse**: a list (name, players, level, delay). Up/down, confirm to join, a button to refresh.
- **Create**: name, public or by code, then the lobby, which shows the code.
- **Join by code**: six digits on the on-screen keyboard of digits.
- **Lobby**: seats, settings (host), a row of actions: Ready / Not Ready, Start (host, when everybody is ready), Team, Chat, Leave. Each player's connection path and delay. Whether the server is verified, and its operator.
- **Tournaments**: open a file or a URL; the bracket; "Play my match".
- **Text entry** (name, password, chat): the on-screen keyboard with a gamepad or touch; a real keyboard types directly.
- **Connection test**: shown while joining, with the step being tried and the outcome.
- **During a match**: a pause-style menu on Start: Chat, Back to Lobby, Leave.

The existing F-key and typed commands keep working; none is required.

## 9. Phases

Each phase is usable on its own and ends with tests.

| Phase | Content | Depends on |
|---|---|---|
| 1 | Gamepad-friendly online screens for today's network mode (menu items, on-screen keyboard, ready state). **Done.** | nothing |
| 2 | The link abstraction: reliable messages over UDP; today's protocol moved onto it. **Done.** | nothing |
| 3 | Snapshot model replacing lockstep, with measurements of size and behaviour under loss. **Done** (sizes in `networking.md`; no buffer of two steps yet, see there). | 2 |
| 4 | Master server (guest sessions, lobbies, status), signed configuration, browse / create / join by code in the game. **Done**; a player reaches a host at the address the master saw, so the host must be reachable until phase 5. | 1 |
| 5 | pjnath + coturn: discovery, hole punching, TURN over UDP, TCP and TLS, the ladder, volume check, always-relay setting, connection display; the old relay removed | 2, 4 |
| 6 | Server certificates and player keys: the tool that issues certificates, the certificate in the handshake, "verified" in the lobby, revocation through the configuration. **Done.** | 2 (revocation: 4) |
| 7 | Signed match records: seed by commit and reveal, signed input statements, the record, `atomic_verify`, a tool that sums statistics from records. **Done** (per round; `atomic_verify --stats` is the tool that sums). | 3, 6 |
| 8 | Tournaments as signed files: the format, the organiser's tool, the screen, servers admitting only listed players. **Done.** | 6, 7 |
| - | Accounts on the master: optional, undecided | 4 |

Phases 6 to 8 do not depend on the master (4) or on NAT traversal (5): a tournament on dedicated servers with public addresses needs neither.

## 10. New dependencies

| Where | What | Note |
|---|---|---|
| Game | An HTTPS client | As built: **mbedTLS** (3.6, fetched and built with the game) under a small client of this project's own (`src/online/http.*`), the same code on every platform including Android. Chosen over libcurl and the platforms' own clients because it needs nothing installed and no Java side. |
| Game | pjnath (PJSIP) | Phase 5. |
| Game, server, tools | An Ed25519 implementation | As built: **Monocypher** (4.0, two C files fetched and built with the game) instead of libsodium, which has no CMake build and would have needed a different recipe on every platform. The master uses Go's standard library. The game's own X25519 / ChaCha20 code is still in use for connections. |
| Master | Go toolchain, PostgreSQL | Separate program, own CI job. |
| Owner's machine | master, PostgreSQL, coturn, a domain with a certificate; the configuration file also on at least one other host | |

## 11. Tests

- Link: messages arrive complete and in order under 40 % datagram loss, reordering and duplication; datagrams are never delayed by a lost one.
- Snapshots: a client fed deltas with loss reaches the server's public state; own-player prediction matches the server when nobody interferes; a late joiner needs one baseline; nothing hidden appears in any snapshot.
- Master: every call against a real PostgreSQL in CI; expired and forged configuration documents are refused.
- Certificates: a valid one is accepted; expired, revoked, signed by an unknown key, or naming another identity key than the one proven, each is refused (the server is then shown as ordinary).
- Records: an honest record verifies; one with a changed result, a changed input, a changed seed or a wrong signature does not; the seed differs when any one contribution differs.
- Tournaments: a file with a bad signature or a lower serial is refused; a server admits a listed player and refuses an unlisted key.
- Connectivity: against a local coturn in CI, direct and relayed paths; with UDP blocked the ladder steps down to TURN over TCP and a round completes; a path that dies after 16 KB fails the volume check.
- Configuration: with no reachable document the online screens refuse and say why; an expired one, a forged one and one with a lower serial are refused; a second URL is used when the first fails.
- Screens: every online action reachable by scripted gamepad input alone.

What automated tests cannot show, and must be tried by people: real routers and carrier-grade NAT, real loss and delay, ten players on ten connections, the owner's server under load.

## 12. Open points


- ~~Whether one datagram per snapshot is enough with ten players.~~ Measured in phase 3: yes (283 bytes on average against a state five steps old, 844 at most).
- Whether pjnath builds on every target and how much of PJSIP has to come with it (phase 5).
- Where the mirrors of the configuration file are hosted.
- Region and delay hints in the lobby list need a way to measure delay to a host before joining.
- Where the root key is kept and how certificates are renewed every 90 days without it becoming a chore.
- Whether a dropped or delayed input can be made visible after the fact (for instance clients reporting what they sent and when), so that a dispute has evidence.
- Whether a player key should be movable between a person's devices, and what happens when it is lost.
- How records are stored and offered for download by a server, and for how long.
- If accounts are ever added: privacy text and the handling of stored email addresses.
- Whether watchers are counted against the ten connections of a host.
