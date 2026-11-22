#!/usr/bin/env bash
# Prepares everything docker-compose.yml needs, in ./data and ./.env:
#
#   ./setup.sh [DOMAIN PUBLIC_IP]
#
# - .env, with a database password and a relay secret made up here;
# - the root key (data/root/root.key, root.pub), where there is none;
# - a TLS certificate signed by itself for the master and the relay, where there is none
#   (data/master/master.crt, master.tls.key: put your own there to use another); not with a
#   proxy (PROXY in .env), which shows certificates from Let's Encrypt instead;
# - the configuration (data/config.json), where there is none, and its signed form
#   (data/master/config.signed.json), which the master serves;
# - the relay's settings (data/coturn/turnserver.conf);
# - with a proxy: its routes (data/traefik/atomic.yml), and COMPOSE_FILE in .env.
#
# It can be run again at any time: it keeps keys, certificate and configuration, writes
# the relay's settings anew from .env and signs the configuration again.
set -euo pipefail
cd "$(dirname "$0")"

[ -f .env ] || cp .env.example .env
# set_env NAME VALUE: that line of .env, replaced.
set_env() {
    if grep -q "^$1=" .env; then
        local tmp
        tmp=$(mktemp)
        awk -v name="$1" -v value="$2" 'index($0, name "=") == 1 { print name "=" value; next } { print }' .env >"$tmp"
        cat "$tmp" >.env
        rm -f "$tmp"
    else
        printf '%s=%s\n' "$1" "$2" >>.env
    fi
}
get_env() { sed -n "s/^$1=//p" .env | tail -n 1; }
random_hex() { head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n'; }

[ $# -ge 1 ] && set_env DOMAIN "$1"
[ $# -ge 2 ] && set_env PUBLIC_IP "$2"
DOMAIN=$(get_env DOMAIN)
PUBLIC_IP=$(get_env PUBLIC_IP)
if [ -z "$DOMAIN" ] || [ -z "$PUBLIC_IP" ] || [ "$DOMAIN" = master.example.org ] || [ "$PUBLIC_IP" = 203.0.113.10 ]; then
    echo "Say what this machine is called and its public address: ./setup.sh DOMAIN PUBLIC_IP (or edit .env)" >&2
    exit 1
fi
MASTER_HOST=$(get_env MASTER_HOST)
MASTER_HOST=${MASTER_HOST:-$DOMAIN}
TURN_HOST=$(get_env TURN_HOST)
TURN_HOST=${TURN_HOST:-$DOMAIN}
set_env MASTER_HOST "$MASTER_HOST"
set_env TURN_HOST "$TURN_HOST"
PROXY=$(get_env PROXY)
case "$PROXY" in
"") set_env COMPOSE_FILE docker-compose.yml ;;
traefik)
    if [ -z "$(get_env ACME_EMAIL)" ]; then
        echo "PROXY=traefik gets its certificates from Let's Encrypt: set ACME_EMAIL in .env (an address for its notices)" >&2
        exit 1
    fi
    set_env COMPOSE_FILE docker-compose.yml:proxy.yml:traefik.yml ;;
external) set_env COMPOSE_FILE docker-compose.yml:proxy.yml ;;
*)
    echo "PROXY in .env is empty, traefik or external, not \"$PROXY\"" >&2
    exit 1 ;;
esac
if [ -n "$PROXY" ] && [ "$MASTER_HOST" = "$TURN_HOST" ]; then
    echo "With a proxy the master and the relay need names of their own (MASTER_HOST, TURN_HOST in .env): it tells them apart by name" >&2
    exit 1
fi
[ -n "$(get_env POSTGRES_PASSWORD)" ] || set_env POSTGRES_PASSWORD "$(random_hex)"
[ -n "$(get_env TURN_SECRET)" ] || set_env TURN_SECRET "$(random_hex)"
set_env HOST_UID "$(id -u)"
set_env HOST_GID "$(id -g)"
chmod 600 .env

PRIVATE_IP=$(get_env PRIVATE_IP)
PRIVATE_IP=${PRIVATE_IP:-$PUBLIC_IP}
MASTER_PORT=$(get_env MASTER_PORT)
TURN_PORT=$(get_env TURN_PORT)
TURN_TLS_PORT=$(get_env TURN_TLS_PORT)
PROTOCOL=$(sed -n 's/.*kProtocolVersion = \([0-9]*\);.*/\1/p' ../src/net/protocol.hpp | head -n 1)
PROTOCOL_MIN=$(get_env PROTOCOL_MIN)
PROTOCOL_MIN=${PROTOCOL_MIN:-$PROTOCOL}

mkdir -p data/root data/master data/coturn data/gameserver
[ -z "$PROXY" ] || mkdir -p data/traefik data/acme
chmod 700 data/root

# The master's image holds the key tool. Pulled before (docker compose pull): used as it is.
IMAGE_TAG=$(get_env IMAGE_TAG)
docker image inspect "ghcr.io/elraro/atomic-bomberman-master:${IMAGE_TAG:-latest}" >/dev/null 2>&1 || docker compose build master
abconfig() { docker compose run --rm -T abconfig "$@"; }

if [ ! -f data/root/root.pub ]; then
    abconfig keygen -out root/root >/dev/null
    echo "made the root key: data/root/root.key (keep a copy somewhere safe)"
fi
KEY=$(cat data/root/root.pub)
set_env ONLINE_KEY "$KEY"

# How the games recognise the master and the relay over TLS: without a proxy by the pin of
# the key of the certificate made here; with one by Let's Encrypt's roots (any key).
if [ -z "$PROXY" ]; then
    if [ ! -f data/master/master.crt ]; then
        abconfig selfsigned -host "$MASTER_HOST" -out master/master >/dev/null
        echo "made a TLS certificate signed by itself for $MASTER_HOST"
    fi
    PIN=$(abconfig pin -cert master/master.crt | tr -d '\r\n')
    MASTER_URL="https://$MASTER_HOST:$MASTER_PORT"
    MASTER_TRUST="\"tls_pins\": [\"$PIN\"]"
    RELAY_TRUST=""
    TLS_PORT=$TURN_TLS_PORT
else
    ROOTS_FILE=$(get_env TLS_ROOTS_FILE)
    ROOTS_FILE=${ROOTS_FILE:-letsencrypt/isrg-roots.pem}
    [ -f "$ROOTS_FILE" ] || { echo "TLS_ROOTS_FILE: $ROOTS_FILE is not there" >&2; exit 1; }
    mkdir -p data/roots
    cp "$ROOTS_FILE" data/roots/roots.pem
    ROOTS=$(abconfig roots -in roots/roots.pem | tr -d '\r\n')
    MASTER_URL="https://$MASTER_HOST"
    MASTER_TRUST="\"tls_roots\": $ROOTS"
    RELAY_TRUST=", $MASTER_TRUST"
    TLS_PORT=443
    set_env TURN_TLS_PORT 443
fi
set_env ONLINE_CONFIG_URL "$MASTER_URL/config.json"

if [ ! -f data/config.json ]; then
    cat >data/config.json <<JSON
{
  "kind": "configuration",
  "master": {"url": "$MASTER_URL", $MASTER_TRUST},
  "stun": ["$TURN_HOST:$TURN_PORT"],
  "turn": [
    {"host": "$TURN_HOST", "port": $TURN_PORT, "transport": "udp"},
    {"host": "$TURN_HOST", "port": $TLS_PORT, "transport": "tls"$RELAY_TRUST},
    {"host": "$TURN_HOST", "port": $TURN_PORT, "transport": "tcp"}
  ],
  "protocol": {"min": $PROTOCOL_MIN, "current": $PROTOCOL},
  "status": {"state": "ok", "message": ""},
  "trust": {"roots": ["$KEY"], "revoked": []},
  "flags": {}
}
JSON
    echo "wrote the configuration: data/config.json"
elif [ -z "$PROXY" ] && ! grep -q "$PIN" data/config.json; then
    echo "WARNING: data/config.json does not name the pin of the master's present key ($PIN)." >&2
    echo "         Games will refuse the master. Put the pin in, or delete the file to have it written anew." >&2
elif [ -n "$PROXY" ] && ! grep -q "tls_roots" data/config.json; then
    echo "WARNING: data/config.json names no roots, but the master is behind a proxy now." >&2
    echo "         Games will refuse the master. Delete the file to have it written anew." >&2
fi

# The relay. Its secret is shared with the master (.env) and nobody else.
{
    echo "# Written by setup.sh from .env: changes made here are lost."
    echo "listening-port=$TURN_PORT"
    [ -n "$PROXY" ] || echo "tls-listening-port=$TURN_TLS_PORT"
    echo "listening-ip=$PRIVATE_IP"
    echo "relay-ip=$PRIVATE_IP"
    [ "$PRIVATE_IP" = "$PUBLIC_IP" ] || echo "external-ip=$PUBLIC_IP/$PRIVATE_IP"
    echo "min-port=$(get_env TURN_MIN_PORT)"
    echo "max-port=$(get_env TURN_MAX_PORT)"
    echo "use-auth-secret"
    echo "static-auth-secret=$(get_env TURN_SECRET)"
    echo "realm=$TURN_HOST"
    echo "no-cli"
    echo "no-dtls"
    if [ -n "$PROXY" ]; then
        echo "no-tls"  # the proxy ends TLS on 443 and passes the stream to listening-port
    else
        echo "cert=/tls/master.crt"
        echo "pkey=/tls/master.tls.key"
    fi
    echo "pidfile=/tmp/turnserver.pid"
    echo "userdb=/tmp/turndb"
    echo "log-file=stdout"
    echo "simple-log"
} >data/coturn/turnserver.conf
chmod 600 data/coturn/turnserver.conf

# The proxy's routes (Traefik's file provider): the master by its name over HTTPS, the
# relay by its name (TLS ended by the proxy, the stream passed on to coturn).
if [ -n "$PROXY" ]; then
    ENTRY=$(get_env TRAEFIK_ENTRYPOINT)
    ENTRY=${ENTRY:-websecure}
    RESOLVER=$(get_env TRAEFIK_RESOLVER)
    if [ -n "$RESOLVER" ]; then TLS="{certResolver: $RESOLVER}"; else TLS="{}"; fi
    BIND=$(get_env PROXY_BIND)
    cat >data/traefik/atomic.yml <<YAML
# Written by setup.sh from .env: changes made here are lost.
http:
  routers:
    atomic-master:
      rule: Host(\`$MASTER_HOST\`)
      entryPoints: [$ENTRY]
      service: atomic-master
      tls: $TLS
  services:
    atomic-master:
      loadBalancer:
        servers:
          - url: http://${BIND:-127.0.0.1}:$MASTER_PORT
tcp:
  routers:
    atomic-relay:
      rule: HostSNI(\`$TURN_HOST\`)
      entryPoints: [$ENTRY]
      service: atomic-relay
      tls: $TLS
  services:
    atomic-relay:
      loadBalancer:
        servers:
          - address: $PRIVATE_IP:$TURN_PORT
YAML
    echo "wrote the proxy's routes: data/traefik/atomic.yml"
fi

if [ -f data/root/root.key ]; then
    ./sign.sh
else
    echo "data/root/root.key is not here: sign data/config.json where the key is (./sign.sh /path/to/root.key)"
fi

if [ -z "$PROXY" ]; then
    OPEN="TCP $MASTER_PORT, UDP+TCP $TURN_PORT, TCP $TURN_TLS_PORT"
else
    OPEN="TCP 443, UDP+TCP $TURN_PORT"
fi
cat <<TEXT

Ready. Start it:   docker compose up -d
Open in the firewall: $OPEN, UDP $(get_env TURN_MIN_PORT)-$(get_env TURN_MAX_PORT)
TEXT
[ "$PROXY" != external ] || echo "Your Traefik: give it data/traefik/atomic.yml (file provider); it must reach the master at $(get_env PROXY_BIND):$MASTER_PORT and the relay at $PRIVATE_IP:$TURN_PORT"
[ -z "$PROXY" ] || echo "DNS: $MASTER_HOST and $TURN_HOST must both point at $PUBLIC_IP"
cat <<TEXT

What a game needs to know:
  --online-config $MASTER_URL/config.json --online-key $KEY
or built in:
  cmake -DAB_ONLINE_CONFIG_URLS=$MASTER_URL/config.json -DAB_ONLINE_CONFIG_KEY=$KEY

The configuration is good for seven days: run ./sign.sh at least once a week.
TEXT
