# Network mode: how it is built

The behaviour is specified in [`../specifications/networking.md`](../specifications/networking.md). This note says where each part lives and what was and was not checked. None of it comes from the original, whose network layer (`../reverse-engineering/networking.md`) is not reproduced.

## Parts

```text
src/game/world.*        saveState / loadState / stateHash / snapshotFields: one description of the state
                        (World::archive) used for saving, loading, hashing and snapshots
src/game/match.*        RoundSetup + applyRoundSetup: how a round is built, shared by the local game,
                        the server and the clients; MatchScore: wins, kills, match winner
src/net/socket.*        non-blocking TCP (framed) and UDP sockets, BSD sockets / Winsock, IPv4 and IPv6
src/net/link.*          the link: reliable messages and datagrams over encrypted packets; knows no sockets
src/net/snapshot.*      a state as its difference from an earlier one
src/net/protocol.*      messages and their byte encoding; decoding checks every range
src/net/server.*        Server: connections, lobby, seats, settings, rounds, snapshots, LAN answers
src/net/client.*        Client: connection, lobby state, chat, the newest state, prediction; LanBrowser
src/net/crypto.*        X25519, ChaCha20-Poly1305, SHA-256, HMAC; the keys of a connection
src/net/upnp.*          asking the home router to forward the port (UPnP); PortMapper runs it on a thread
src/server/main.cpp     atomic_server: the Server with a log and three console commands
src/app/net_ui.*        the game's screens: join, host, lobby with chat, match, result
tools/diagnostics/net_bot.cpp   a client without a window that presses random keys (for testing a server)
```

`ab_net` depends on `ab_game` and `ab_resources` only. Neither it nor `atomic_server` links SDL.

## Things worth knowing

- **Nothing blocks and there are no threads** in the server or the client: the owner calls `update(nowMs)`. The clock is an argument, which is what lets the tests run a whole match in a fraction of a second on a simulated clock. (The dedicated server has one extra thread that only reads console lines.)
- **A hosted game is the same `Server` object**, updated from the game's frame loop, with the host's own client connected to it through 127.0.0.1. If the host's window stops drawing (minimised on some systems, or dragged on Windows), the server stops too. For anything serious use `atomic_server`.
- **The server's word is the state.** Clients do not work the round out; they load what the server sends (`loadSnapshotFields`). So a client and a server of slightly different builds no longer drift apart, though the protocol version must still be raised whenever the state's layout or the rules change (the row of numbers has no names in it). The core is still deterministic, which prediction leans on (the same inputs give the same guess the server will confirm) and which a recorded match will lean on to be replayed.
- **Every member of `World` that matters must be in `World::archive`**: what is not there is not in a snapshot, and a client's copy keeps whatever stale value it had.
- **Encryption** sits in one place: the link seals and opens every packet (`sealDatagram` / `openSealed`). The key exchange is done by `Client` and `Server` before the link starts. The algorithms are written out in `crypto.cpp` and checked against the test vectors of FIPS 180-4, RFC 4231, RFC 7748 and RFC 8439; nobody has reviewed them for side channels or misuse.
- **What clients are not told** is decided in one place: `World::archive` with `publicView` leaves out hidden powerups and the random generator.
- **Clients trust the server.** Round setups are range-checked when decoded, but a snapshot is loaded as it comes (its length and shape are checked, its values are not): a hostile server could send a state that makes the client misbehave. Servers do not trust clients: all a client can influence is its own six keys per player, chat text and, for the administrator, the lobby settings.
- **Finding servers on the LAN**: the query is a broadcast to UDP 27409, which every server binds in shared mode next to its own port, so servers on any port are found. Two servers on one machine both receive a broadcast; a query sent to 127.0.0.1 reaches only one of them.
- Computer players run on the server.
- **Prediction** lives entirely in `Client` (`predict()`, `view()`): a second `World`, copied from the newest state and run ahead on every local step. The screens draw `view()`; everything else reads `world()`. Copying a `World` twenty times a second is cheap at this size.

## Checked

- `ab_tests` "state transfer": a state saved in the middle of a four-player round, loaded into a fresh `World`, stays hash-identical for the rest of the round.
- `ab_net_tests` (real sockets on 127.0.0.1, simulated clock): message encoding and refusal of truncated or out-of-range data; the link at 40 % loss with repeats and delays; the snapshot coder and its sizes; lobby (password, seats, names, chat, settings, teams, kick, administrator hand-over, ready); a match with two clients and a late joiner until it is decided, over UDP and inside TCP, every client ending each round on the server's state; a player leaving mid-round; 40 % of all datagrams dropped, from the first packet on; no UDP at all; a client cut off for five seconds; the cookie; the LAN answer and the search for a server on another port; client-side prediction; two players at one computer; roulette; campaign; bans and limits; server identity; relay.
- By scripted keys in unattended runs of the game: typing and sending a chat line in the lobby, changing settings, changing team, starting the match with F2, opening the chat line during the match.
- By hand, on one machine with the real game data: `atomic_server` with `net_bot` clients (one on UDP, one forced to TCP) over several rounds (before snapshots: equal hashes; since: one match, no whole state needed after the first); the game itself joining a dedicated server as a player and as a watcher; the join, host, lobby, match and result screens captured from unattended runs.

## Not checked

- Two different machines, a real LAN, the internet, NAT, real packet loss and delay.
- UPnP against a real router. The conversation was tested against a router made of strings (search answer, device description, the mapping requests, a refusal, no answer). On the development network no router answered, so only the "nobody there" path has run for real.
- IPv6 beyond the loopback address (`::1`): one test plays a round with an IPv6 and an IPv4 client on the same server. No IPv6 network was available.
- Four players at one computer: the test uses two, and no gamepad was available for the third and fourth.
- A campaign played to its end over the network, and the roulette prize as a person would notice it; the tests check the first stage and the prize's presence in the round.
- The Windows programs started by a person. (`ab_net_tests` does run on Windows in CI and passes, so the Winsock path of the sockets, server and client is exercised there.)
- A Linux and a Windows build playing each other. With snapshots they need not compute alike; only the encoding has to agree.
- The screens with a person at the keyboard (real key events and text input rather than scripted ones), `/kick`, the Esc-twice exit, how the delay feels.
- More than three clients at once.
- The roulette wheel on screen in a network game (the test checks that the client's wheel stops where the server says; nobody has looked at it).
- A TURN relay on another machine, and one under load.
- An attack by someone who knows what they are doing. The tests show the protections do what they were written to do; they are not a security review.
- Prediction with real delay. The test runs over the loopback interface with the look-ahead forced to four steps: it shows the predicted states are exactly right while inputs are steady, wrong for at most a few steps after a change, and that the confirmed state is never touched. How it feels at 50-200 ms, and how distracting the corrections of other players are, nobody has seen.
