# Network mode: how it is built

The behaviour is specified in [`../specifications/networking.md`](../specifications/networking.md). This note says where each part lives and what was and was not checked. None of it comes from the original, whose network layer (`../reverse-engineering/networking.md`) is not reproduced.

## Parts

```text
src/game/world.*        saveState / loadState / stateHash: one description of the state (World::archive)
                        used for saving, loading and hashing
src/game/match.*        RoundSetup + applyRoundSetup: how a round is built, shared by the local game,
                        the server and the clients; MatchScore: wins, kills, match winner
src/net/socket.*        non-blocking TCP (framed) and UDP sockets, BSD sockets / Winsock, IPv4
src/net/protocol.*      messages and their byte encoding; decoding checks every range
src/net/server.*        Server: lobby, seats, settings, rounds, steps, snapshots, LAN answers
src/net/client.*        Client: connection, lobby state, chat, the round's World; LanBrowser
src/net/crypto.*        X25519, ChaCha20-Poly1305, SHA-256, HMAC; the keys of a connection
src/net/relay.*         the relay (also `atomic_server --relay-server`)
src/net/upnp.*          asking the home router to forward the port (UPnP); PortMapper runs it on a thread
src/server/main.cpp     atomic_server: the Server with a log and three console commands
src/app/net_ui.*        the game's screens: join, host, lobby with chat, match, result
tools/diagnostics/net_bot.cpp   a client without a window that presses random keys (for testing a server)
```

`ab_net` depends on `ab_game` and `ab_resources` only. Neither it nor `atomic_server` links SDL.

## Things worth knowing

- **Nothing blocks and there are no threads** in the server or the client: the owner calls `update(nowMs)`. The clock is an argument, which is what lets the tests run a whole match in a fraction of a second on a simulated clock. (The dedicated server has one extra thread that only reads console lines.)
- **A hosted game is the same `Server` object**, updated from the game's frame loop, with the host's own client connected to it through 127.0.0.1. If the host's window stops drawing (minimised on some systems, or dragged on Windows), the server stops too. For anything serious use `atomic_server`.
- **Determinism is the load-bearing assumption.** The core uses integers only and its own random generator, and the tuning values travel with every round start, so two builds of the same source agree. Two *different* versions of the core would not; the protocol version must be raised whenever the simulation changes. The state hash catches a divergence within a step or two and a snapshot repairs it, so the visible effect of a mismatch is repeated corrections (`WARN  State resent` in the server log), not a broken game.
- **Encryption** sits in two places only: `TcpSocket` seals and opens frames once it has been given keys, and `sealDatagram` / `openSealed` wrap datagrams. The messages themselves are unchanged. The algorithms are written out in `crypto.cpp` and checked against the test vectors of FIPS 180-4, RFC 4231, RFC 7748 and RFC 8439; nobody has reviewed them for side channels or misuse.
- **Secrets** are kept by two small mechanisms in the core, not in the network code: `Rng` can record what it gives out or replay what it is fed, and `World::look()` notes every hidden powerup it is asked about. `World::takeSecrets()` / `feedSecrets()` carry them. A rule added to the core that reads `powerups_` directly where a hidden one could matter would break this silently for clients (they would drift and be repaired by snapshots): such reads must go through `look()`.
- **Clients trust the server.** Round setups are range-checked when decoded, but a snapshot is loaded as it comes: a hostile server could send a state that makes the client misbehave. Servers do not trust clients: all a client can influence is its own input byte, chat text and, for the administrator, the lobby settings.
- **Finding servers on the LAN**: the query is a broadcast to UDP 27409, which every server binds in shared mode next to its own port, so servers on any port are found. Two servers on one machine both receive a broadcast; a query sent to 127.0.0.1 reaches only one of them.
- Computer players run on the server and are sent as inputs like everyone else's.
- **Prediction** lives entirely in `Client` (`predict()`, `view()`): a second `World`, copied from the confirmed one and run ahead on every local step. The screens draw `view()`; everything else reads `world()`. Copying a `World` twenty times a second is cheap at this size.

## Checked

- `ab_tests` "state transfer": a state saved in the middle of a four-player round, loaded into a fresh `World`, stays hash-identical for the rest of the round.
- `ab_net_tests` (real sockets on 127.0.0.1, simulated clock): message encoding and refusal of truncated or out-of-range data; lobby (password, seats, names, chat, settings, teams, kick, administrator hand-over); a match with two clients and a late joiner until it is decided, over UDP and over TCP only, every client's hash equal to the server's after each round; a client whose state is deliberately broken gets exactly one snapshot; a player leaving mid-round; 40 % of all datagrams dropped; the LAN answer and the search for a server on another port; client-side prediction; two players at one computer; the roulette prize; a campaign stage with the free asset set as the server's data; IPv6; the router conversation; the cryptography's test vectors; a relay in the middle that sees no readable text and whose single flipped bit ends the connection; kick and ban (including the ban file across a restart), password guessing, message flooding and the per-address connection limit; administrator hand-over and the administrator password; a client that holds no hidden powerups and no random generator staying in step through whole rounds (also in `ab_tests`, "secrets stay on the server"); the roulette wheel reproduced from the server's seed; a server's identity across restarts and the refusal of a changed one; a game joined through a relay by its code.
- By scripted keys in unattended runs of the game: typing and sending a chat line in the lobby, changing settings, changing team, starting the match with F2, opening the chat line during the match.
- By hand, on one machine with the real game data: `atomic_server` with `net_bot` clients (one on UDP, one forced to TCP) over several rounds on random levels: equal hashes, no snapshots; the game itself joining a dedicated server as a player and as a watcher; the join, host, lobby, match and result screens captured from unattended runs.

## Not checked

- Two different machines, a real LAN, the internet, NAT, real packet loss and delay.
- UPnP against a real router. The conversation was tested against a router made of strings (search answer, device description, the mapping requests, a refusal, no answer). On the development network no router answered, so only the "nobody there" path has run for real.
- IPv6 beyond the loopback address (`::1`): one test plays a round with an IPv6 and an IPv4 client on the same server. No IPv6 network was available.
- Four players at one computer: the test uses two, and no gamepad was available for the third and fourth.
- A campaign played to its end over the network, and the roulette prize as a person would notice it; the tests check the first stage and the prize's presence in the round.
- The Windows programs started by a person. (`ab_net_tests` does run on Windows in CI and passes, so the Winsock path of the sockets, server and client is exercised there.)
- A Linux and a Windows build playing each other. Both use the same integer code, so they should agree, but this is the determinism assumption's first real test.
- The screens with a person at the keyboard (real key events and text input rather than scripted ones), `/kick`, the Esc-twice exit, how the delay feels.
- More than three clients at once.
- The roulette wheel on screen in a network game (the test checks that the client's wheel stops where the server says; nobody has looked at it).
- A relay on another machine, and a relay under load.
- An attack by someone who knows what they are doing. The tests show the protections do what they were written to do; they are not a security review.
- Prediction with real delay. The test runs over the loopback interface with the look-ahead forced to four steps: it shows the predicted states are exactly right while inputs are steady, wrong for at most a few steps after a change, and that the confirmed state is never touched. How it feels at 50-200 ms, and how distracting the corrections of other players are, nobody has seen.
