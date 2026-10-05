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
src/server/main.cpp     atomic_server: the Server with a log and three console commands
src/app/net_ui.*        the game's screens: join, host, lobby with chat, match, result
tools/diagnostics/net_bot.cpp   a client without a window that presses random keys (for testing a server)
```

`ab_net` depends on `ab_game` and `ab_resources` only. Neither it nor `atomic_server` links SDL.

## Things worth knowing

- **Nothing blocks and there are no threads** in the server or the client: the owner calls `update(nowMs)`. The clock is an argument, which is what lets the tests run a whole match in a fraction of a second on a simulated clock. (The dedicated server has one extra thread that only reads console lines.)
- **A hosted game is the same `Server` object**, updated from the game's frame loop, with the host's own client connected to it through 127.0.0.1. If the host's window stops drawing (minimised on some systems, or dragged on Windows), the server stops too. For anything serious use `atomic_server`.
- **Determinism is the load-bearing assumption.** The core uses integers only and its own random generator, and the tuning values travel with every round start, so two builds of the same source agree. Two *different* versions of the core would not; the protocol version must be raised whenever the simulation changes. The state hash catches a divergence within a step or two and a snapshot repairs it, so the visible effect of a mismatch is repeated corrections (`WARN  State resent` in the server log), not a broken game.
- **Clients trust the server.** Round setups are range-checked when decoded, but a snapshot is loaded as it comes: a hostile server could send a state that makes the client misbehave. Servers do not trust clients: all a client can influence is its own input byte, chat text and, for the administrator, the lobby settings.
- **Finding servers on the LAN** works only for servers on the default port (27410), since the query is a broadcast to that port.
- Computer players run on the server and are sent as inputs like everyone else's.

## Checked

- `ab_tests` "state transfer": a state saved in the middle of a four-player round, loaded into a fresh `World`, stays hash-identical for the rest of the round.
- `ab_net_tests` (real sockets on 127.0.0.1, simulated clock): message encoding and refusal of truncated or out-of-range data; lobby (password, seats, names, chat, settings, teams, kick, administrator hand-over); a match with two clients and a late joiner until it is decided, over UDP and over TCP only, every client's hash equal to the server's after each round; a client whose state is deliberately broken gets exactly one snapshot; a player leaving mid-round; 40 % of all datagrams dropped; the LAN answer.
- By hand, on one machine with the real game data: `atomic_server` with `net_bot` clients (one on UDP, one forced to TCP) over several rounds on random levels: equal hashes, no snapshots; the game itself joining a dedicated server as a player and as a watcher; the join, host, lobby, match and result screens captured from unattended runs.

## Not checked

- Two different machines, a real LAN, the internet, NAT, real packet loss and delay.
- A Windows build of the socket code running (it is compiled by CI; nobody has started it).
- A Linux and a Windows build playing each other. Both use the same integer code, so they should agree, but this is the determinism assumption's first real test.
- The screens with a person at the keyboard: typing, chat during a match, the administrator's settings rows, the Esc-twice exit.
- More than three clients at once.
