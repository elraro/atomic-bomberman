# The online service in containers

Everything a public service for online play needs, on one Linux machine with Docker: the master server, its PostgreSQL, and the relay (coturn). Optionally a dedicated game server. What each part is and why: [`../docs/operations/online.md`](../docs/operations/online.md).

| Service | Image | Ports on the machine |
|---|---|---|
| `master` | `ghcr.io/elraro/atomic-bomberman-master` (from `../master`; also holds the key tool `abconfig`) | TCP 8443 (HTTPS) |
| `postgres` | `postgres:17` | 127.0.0.1:5432 only |
| `coturn` | `coturn/coturn:4.7`, the project's own image, unchanged | UDP and TCP 3478, TCP 443 (TLS), UDP 5000-9999 |
| `gameserver` (optional) | `ghcr.io/elraro/atomic-bomberman-server` (`atomic_server`, `atomic_verify`, `net_bot`) | UDP and TCP 27410 |
| `autosign` (optional) | the master's image | none |

The master, the relay and the game server use the machine's network directly (`network_mode: host`, so Linux only): the relay hands out thousands of UDP ports, and the master must see the addresses players really come from.

## Starting

```sh
cd deploy
docker compose pull                          # the published images; without this they are built here
./setup.sh play.example.org 203.0.113.10     # the machine's name and its public address
docker compose up -d
```

The project's two images are published by CI at `ghcr.io/elraro/` for x86-64 and 64-bit ARM, after the job that tries them has passed: `latest` is the main branch, a release has its tag's name (`IMAGE_TAG` in `.env` chooses). With `--profile gameserver` on the `pull`, the game server's comes too.

`setup.sh` writes `.env` (with a database password and a relay secret it makes up) and, in `data/`:

| File | What |
|---|---|
| `data/root/root.key`, `root.pub` | The root key that signs the configuration. **See below.** |
| `data/master/master.crt`, `master.tls.key` | A TLS certificate signed by itself, used by the master and by the relay's TLS port. |
| `data/config.json` | The configuration: where the master and the relay are, the pin of the master's key. Yours to edit. |
| `data/master/config.signed.json` | Its signed form, served by the master at `/config.json`. |
| `data/coturn/turnserver.conf` | The relay's settings, written anew from `.env` at every run. |

It ends by printing what a game needs: the configuration's address and the root key's public half, as options for the command line and for the release build.

Open in the firewall: TCP 8443, UDP and TCP 3478, TCP 443, UDP 5000-9999 (and UDP and TCP 27410 for the game server). (Behind Traefik: see below.) On a machine whose public address is mapped onto a private one (most clouds), set `PRIVATE_IP` in `.env` to the address the machine itself holds and run `./setup.sh` again.

## Behind Traefik on 443, with Let's Encrypt

Instead of a certificate signed by itself on port 8443, the master and the relay's TLS port can
sit behind Traefik on 443 with certificates from Let's Encrypt. The configuration then names
Let's Encrypt's two roots (`letsencrypt/isrg-roots.pem`) instead of the pin of a key, so a
certificate renewed with a new key changes nothing for the games.

The master and the relay need a name each (Traefik tells them apart by name), both pointing at
this machine. In `.env`:

```sh
TURN_HOST=turn.example.org
PROXY=traefik                  # Traefik started here; or external: your own Traefik
ACME_EMAIL=you@example.org     # Let's Encrypt's notices about the certificates
```

```sh
./setup.sh master.example.org 203.0.113.10
docker compose --profile gameserver up -d     # COMPOSE_FILE in .env adds proxy.yml and traefik.yml
```

Games then use `--online-config https://master.example.org/config.json`.

| | Without a proxy | With `PROXY` |
|---|---|---|
| Master | HTTPS on 8443, certificate signed by itself | plain HTTP on `PROXY_BIND:8443` for Traefik only; HTTPS on 443 through Traefik |
| Relay over TLS | coturn on 443 | Traefik on 443 ends TLS for `TURN_HOST` and passes the stream to coturn's 3478 |
| Relay over UDP and TCP | 3478 | 3478 (unchanged, not through Traefik) |
| The games recognise them by | the pin of the key | a chain to a Let's Encrypt root, made out to the name |
| Firewall | TCP 8443, 443, UDP+TCP 3478, UDP 5000-9999 | TCP 443, UDP+TCP 3478, UDP 5000-9999 |

- Traefik gets the certificates by the TLS-ALPN challenge on 443 itself: port 80 is not needed. They are kept in `data/acme/`.
- The master takes each player's address from Traefik's `X-Forwarded-For` (the last entry, the one Traefik added).
- **A Traefik of your own** (`PROXY=external`): `setup.sh` writes its routes to `data/traefik/atomic.yml`. Load that with its file provider, set `TRAEFIK_ENTRYPOINT` and `TRAEFIK_RESOLVER` to the names your Traefik uses, and make sure it reaches the master at `PROXY_BIND:8443` (on the machine's network: `127.0.0.1`) and coturn at `PRIVATE_IP:3478`.
- **Switching** an existing setup: delete `data/config.json` (it is written anew with the roots), run `./setup.sh`, then `docker compose up -d`. Games that only know pins refuse the new configuration until they are updated; to bridge that, add the old pin to `master.tls_pins` beside the roots for a while.
- **When Let's Encrypt moves to new roots** (YE, YR are announced): add them to `letsencrypt/isrg-roots.pem` (see the README there), delete `data/config.json` or edit its `tls_roots`, and sign again. No new game release is needed.
- A DNS `CAA` record that allows only `letsencrypt.org` for the two names narrows who could get a certificate the games would take.

## Every week: sign again

The signed configuration is good for seven days; a game that cannot get a valid one has no online play.

```sh
./sign.sh                       # with data/root/root.key
./sign.sh /media/usb/root.key   # with the key kept elsewhere
```

The master serves the new one at once; nothing is restarted.

**The root key.** Whoever has it can send every player to a master of their own. `setup.sh` leaves it in `data/root/` so that things work at once. Better: copy `root.key` away, delete it here, and sign on another machine (`config.json` there, `config.signed.json` back to `data/master/`), or bring the key only for signing as above.

Or accept the risk and let the machine do it: `docker compose --profile autosign up -d` signs once a day with the key in `data/root/`. Nobody then has to remember, and a break-in on this machine gives the key away.

## A dedicated game server

```sh
docker compose --profile gameserver up -d
docker compose logs gameserver      # "Online: listed, code 123456"
```

Its name, port and whether it is listed are in `.env` (`GAME_NAME`, `GAME_PORT`, `GAME_ONLINE`, `GAME_OPTIONS` for any other option of `atomic_server`). Its keys, match records and bans are in `data/gameserver/atomic-bomberman-modern/atomic/`; imported game assets put in `assets/` there give it the original schemes. It can run on another machine as well: only `.env` (`DOMAIN`, `MASTER_PORT`, `ONLINE_KEY`, `HOST_UID`, `HOST_GID`) and this compose file are needed there, started with `docker compose --profile gameserver up -d --no-deps gameserver`.

Its commands on standard input (`status`, `kick`...) are not reachable in a container; use `--admin-password` in `GAME_OPTIONS` and the chat.

## Changing things

- **Settings** (`.env`): edit, `./setup.sh`, `docker compose up -d`.
- **The configuration** (`data/config.json`: maintenance message, lowest protocol version, revoked certificates): edit, `./sign.sh`.
- **Another TLS certificate** (Let's Encrypt, say): put it at `data/master/master.crt` and `master.tls.key`, readable by the user who ran `setup.sh`; put the new pin (`docker compose run --rm abconfig pin -cert master/master.crt`) into `data/config.json` beside the old one; `./sign.sh`; `docker compose restart master coturn`. A renewal that keeps the key keeps the pin.
- **The key tool**: `docker compose run --rm abconfig ...`, working in `data/` (certificates for game servers, tournaments: sections 6a and 8 of the operations guide).
- **A new version**: `git pull`, then `docker compose pull` (or `docker compose build` to build it here), `docker compose up -d` (each with `--profile gameserver` where that is used).

## What is checked, and what is not

The CI job `docker-traefik` does the same behind Traefik (a test authority's certificate instead of Let's Encrypt's, which cannot be had there): the master by name through Traefik, and a match each over UDP, TCP and TLS through the relay. The CI job `docker` builds both images (on x86-64 and on 64-bit ARM), starts all of this on one machine and has test clients in containers of their own find the game server in the master's list and play a match on it through the relay: over UDP, over TCP and over TLS. (The way straight between two ends is not tried there, and the game server runs on Docker's bridge network instead of the machine's: on one machine, ends that share the relay's address get around it.)

Not tried: on a real public machine, behind a cloud's address mapping (`PRIVATE_IP`), with Docker on anything but Linux, with a rootless Docker, or `autosign` over more than a moment.
