# Running online play

What the owner of an online service for the game has to set up, and what a group that wants its own has to do. The design is in [`../specifications/online-play.md`](../specifications/online-play.md); the master server's own notes are in [`../../master/README.md`](../../master/README.md).

State of things: this covers what exists (the master server, the signed configuration, games found by list or by code, server certificates, player keys, match records). Reaching players behind routers without forwarded ports (STUN / TURN, phase 5) and tournaments are not built yet; where a step below prepares for them it says so.

## The parts

| Part | What | Where it runs |
|---|---|---|
| Root key | An Ed25519 key that signs the configuration. | Offline. Never on the machine that faces the internet. |
| Configuration | A small signed file: where the master is, the pin of its TLS key, later the relay and the keys that may vouch for game servers. Issued for seven days at most. | Any web space, in one or several places. |
| Master server | `master/cmd/master`: sessions, the list of games, join codes. | A machine with a public address. |
| PostgreSQL | Keeps the master's sessions and lobbies over a restart. Optional. | Beside the master. |
| The game | Needs to know two things: where the configuration is, and the root key's public half. | Built in, or set by each player. |

A game never uses a configuration it cannot verify, and keeps none from an earlier day: **if the configuration cannot be fetched or has expired, there is no online play** (playing on the local network and joining by address go on working). So the configuration must be re-signed at least every seven days and should be reachable at more than one address.

## 1. Keys

```sh
cd master && go build -o bin/ ./cmd/...
bin/abconfig keygen -out root
```

This writes `root.key` (the private half: 64 hexadecimal digits; keep it offline, with a copy somewhere safe) and `root.pub`. Losing the key means every game has to be told a new one; somebody else having it means they can send players to a master of their own.

## 2. The master server

A TLS certificate: one from an authority (Let's Encrypt and the like), or one signed by itself:

```sh
bin/abconfig selfsigned -host master.example.org -out master      # master.crt, master.tls.key
```

Either is as good as the other here, because the game does not consult the system's list of authorities: it recognises the master by the **pin** of its key, which the signed configuration names.

```sh
bin/abconfig pin -cert master.crt        # prints the pin: 64 hexadecimal digits
```

The pin is of the key, not of the certificate: renewing a certificate with the same key keeps the pin (for certbot: `--reuse-key`). A new key means a new pin and a newly signed configuration; to change keys without a gap, list the old and the new pin together for a week.

```sh
bin/master -listen :8443 -tls-cert master.crt -tls-key master.tls.key \
    -database postgres://master:PASSWORD@127.0.0.1/master \
    -config-file /srv/online/config.signed.json
```

See `master/README.md` for every option. `-config-file` makes the master serve the configuration at `/config.json`, which saves a web server; it may be served from anywhere else as well, and should be from at least one other place.

## 3. The configuration

```sh
bin/abconfig example > config.json      # then edit it
```

```json
{
  "master": {"url": "https://master.example.org:8443", "tls_pins": ["<the pin>"]},
  "stun": [], "turn": [],
  "protocol": {"min": 8, "current": 8},
  "status": {"state": "ok", "message": ""},
  "trust": {"roots": [], "revoked": []},
  "flags": {}
}
```

- `protocol.min`: games older than this are told they are too old. The game's protocol version is `kProtocolVersion` in `src/net/protocol.hpp`.
- `status.state`: `"maintenance"` closes online play with the message shown.
- `stun`, `turn`: for phase 5; leave them empty for now.
- `trust.roots`: the public keys that may sign server certificates (section 6); the root key itself, or a second key kept for that purpose. `trust.revoked`: serials of certificates no longer to be believed.

On the offline machine, at least once a week:

```sh
bin/abconfig sign -key root.key -in config.json -out config.signed.json -stamp
```

`-stamp` writes the serial (the present time), the day of issue and the expiry (`-days N`, seven at most) into the document before signing. A game refuses a document with a lower serial than one it has already accepted, so an old one cannot be played back. Copy `config.signed.json` to every address the games look at.

```sh
bin/abconfig verify -pub "$(cat root.pub)" -in config.signed.json     # what a game will accept, printed
```

## 4. Telling the game

A game looks for the configuration in this order:

1. The command line: `--online-config URL[,URL...] --online-key HEX` (`atomic`, `atomic_server` and the test client `net_bot`).
2. The settings file: `online_config_url` and `online_config_key`.
3. What was built in: `cmake -DAB_ONLINE_CONFIG_URLS="https://a.example/config.json,https://b.example/config.json" -DAB_ONLINE_CONFIG_KEY=<root.pub>`.

The packages built by this repository's CI have nothing built in: **until the project's owner runs a master and adds those two options to the release build, a player has to name a configuration by hand.** A group with its own master uses (1) or (2).

## 5. Game servers

In the game: Start Network Game, "Announce online: Listed for everybody / By its code only". The lobby then shows the code.

Dedicated:

```sh
atomic_server --online listed --name "Friday game"       # or: --online code
```

The log says `Online: listed, code 123456` once the master has the game. A game announced online has no password; one that is not for everybody is announced `code` and joined by its six digits.

Players reach a game server **at the address the master saw it at, with the port it said**. So for now the machine must be reachable from outside on that port (UDP, and TCP as the fallback): a public address, a forwarded port, or a router that honours UPnP (`--upnp`; the game asks by itself when hosting). Getting through routers without that is phase 5.

## 6. Certificates for game servers

A certificate says who answers for a dedicated server. Players' games show "verified server of NAME" in the lobby when it holds. It is made by hand, by whoever holds a key listed in `trust.roots`:

1. The server's operator runs `atomic_server --show-keys` (on the machine and under the user the server runs as) and sends the two keys it prints: the identity key and the signing key. Neither is secret.
2. The key holder, offline: `abconfig cert -key root.key -identity HEX -signing HEX -operator "Tuesday League" -contact admin@example.org -out tuesday.cert` (90 days; `-days N` for another span, 366 at most). The serial printed is what a revocation names.
3. The operator starts the server with `--certificate tuesday.cert`. It says whose certificate it shows and how many days are left, and refuses to start with one made out to other keys.

To take a certificate back before it runs out, add its serial to `trust.revoked` and sign the configuration anew; games pick that up the next time they fetch it (they keep none, so within the hour at the latest for a running game, at once for a newly started one).

A game hosted from the menu never has a certificate: its host could alter their own game, and nobody answers for it.

## 7. Match records

A dedicated server keeps a signed record of every round in `records/` in its per-user folder (`--records-dir DIR` for another place): about 6 KB for a short round. Its players are handed the same record and check it themselves.

```sh
atomic_verify --root "$(cat root.pub)" records/*.abrec            # does each hold?
atomic_verify --root "$(cat root.pub)" --stats records/*.abrec    # and who won what
```

`atomic_verify` plays each round again from its seed and inputs and compares hashes and result, checks the seed's shares and the players' signed input logs, and says whose certificate vouches for the server that signed. With `--root`, only records from servers with a certificate that counts go into the statistics. It must be the same release of the game as the server that made the records.

Publishing a tournament's records lets anybody check them; nothing has to be believed on the organiser's word.

## 8. Checking it

`tests/integration/online.sh BUILD_DIR master/bin` runs all of the above on one machine with made-up keys: master over TLS, signed configuration, two game servers (one with a certificate), clients that find them by code and in the list and play, and the refusals (wrong code, configuration signed by another key, master with another key than the pinned one, a certificate used by another server, a revoked certificate), and the match records: handed to the players, verified with `atomic_verify`, a changed one found out. The CI job `master` runs it on every change.

## What has not been done

- Nobody runs a public master for this project yet, so nothing is built into the released game.
- The master has only been run on one machine beside its clients, never across the internet, never under load.
- No coturn, no STUN, no hole punching (phase 5).
