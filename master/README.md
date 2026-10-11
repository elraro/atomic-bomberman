# Master server

Where online games are found: guest sessions, the list of public games, join codes, the passing of connection details between a host and those joining, and short-lived credentials for the relay. It carries no game traffic and decides nothing about a game; a game server or a result is not made trustworthy by it (`../docs/specifications/online-play.md`, sections 1 and 7).

Written in Go; needs nothing but the standard library and the PostgreSQL driver `pgx`.

## Programs

| Program | What it is |
|---|---|
| `cmd/master` | The server. |
| `cmd/abconfig` | Makes keys, signs and checks the configuration document, prints the pin of a TLS certificate's key. |

```sh
go build -o bin/ ./cmd/...
bin/master -listen :8443 -tls-cert fullchain.pem -tls-key privkey.pem \
    -database postgres://master:PASSWORD@127.0.0.1/master \
    -turn-secret "the secret in coturn's static-auth-secret" \
    -turn-urls "turn:turn.example.org:3478?transport=udp,turn:turn.example.org:3478?transport=tcp,turns:turn.example.org:443?transport=tcp"
```

Every flag can also be given by environment variable (`MASTER_LISTEN`, `MASTER_TLS_CERT`, `MASTER_TLS_KEY`, `MASTER_DATABASE_URL`, `MASTER_STATUS`, `MASTER_MESSAGE`, `MASTER_TURN_SECRET`, `MASTER_TURN_URLS`, `MASTER_TRUST_PROXY=1`).

- Without `-database` everything is kept in memory and lost at a restart (lobbies come back by themselves within seconds: every host repeats its lobby).
- Without a certificate it speaks plain HTTP: only for a TLS-terminating proxy in front of it on the same machine (then also `-trust-proxy`, so that limits go by the players' addresses and not the proxy's), or for trying things out.
- The tables are made on first start. Nothing in them is personal data: a display name, a game version, addresses of game servers that asked to be found. All of it expires (lobbies after 30 s of silence, sessions after a day).

How to set up the whole of online play (keys, the configuration document, coturn, this server): `../docs/operations/online.md`.

## API

Version 1, JSON, paths under `/v1`. A session token goes in `Authorization: Bearer ...`; a host's token in `X-Host-Token`. Errors are `{"error": "text"}` with a fitting status.

| Call | Purpose |
|---|---|
| `GET /status` | `status` (ok / maintenance), `message`, `min_version`. |
| `POST /session` | `{name, version}` gives `{token, id, name}`. |
| `POST /relay-credentials` | TURN user name and password for an hour (coturn `use-auth-secret`). |
| `POST /lobbies` | `{name, public, players, seats, phase, level, region, identity_key, certificate, port, candidates}` gives `{id, code, host_token}`. |
| `GET /lobbies?page=N` | Public lobbies, newest first, 50 a page. |
| `GET /lobbies/by-code/{code}` | One lobby by its six-digit code. |
| `POST /lobbies/{id}/join` | `{code}` for a lobby that is not listed. Gives the ways to reach the game server, its identity key and certificate, and relay credentials. |
| `POST /lobbies/{id}/leave` | |
| `PUT /lobbies/{id}` | Host: the lobby's present description. Also says "still here": 30 s without it and the lobby is removed. |
| `DELETE /lobbies/{id}` | Host: close. |
| `POST /lobbies/{id}/signal` | `{to, data}`: a player to `"host"`, the host to a player's session id. |
| `GET /lobbies/{id}/signal?wait=N` | What is waiting for the asker; waits up to N seconds (25 at most) for something to arrive. |

Rules it keeps: codes and tokens come from the system's cryptographic random source; tokens are stored as hashes; a wrong code and an unknown one answer alike; ten code look-ups a minute per address; 120 write calls a minute per address; request bodies of 16 KB at most.

## Tests

```sh
go test ./...                                   # memory store
MASTER_TEST_DATABASE_URL=postgres://... go test ./...   # and the same against PostgreSQL
```

The CI job `master` runs both.
