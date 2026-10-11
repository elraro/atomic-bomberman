# Running online play

What the owner of an online service for the game has to set up, and what a group that wants its own has to do. The design is in [`../specifications/online-play.md`](../specifications/online-play.md); the master server's own notes are in [`../../master/README.md`](../../master/README.md).

State of things: this covers what exists (phases 1 to 4: the master server, the signed configuration, games found by list or by code). Reaching players behind routers without forwarded ports (STUN / TURN, phase 5), server certificates, match records and tournaments are not built yet; where a step below prepares for them it says so.

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
- `stun`, `turn`, `trust`: for phases 5 and 6; leave them empty for now.

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

## 6. Checking it

`tests/integration/online.sh BUILD_DIR master/bin` runs all of the above on one machine with made-up keys: master over TLS, signed configuration, two game servers, three clients, and the refusals (wrong code, configuration signed by another key, master with another key than the pinned one). The CI job `master` runs it on every change.

## What has not been done

- Nobody runs a public master for this project yet, so nothing is built into the released game.
- The master has only been run on one machine beside its clients, never across the internet, never under load.
- No coturn, no STUN, no hole punching (phase 5).
