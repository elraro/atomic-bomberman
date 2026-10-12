#!/usr/bin/env bash
# Prepares everything docker-compose.yml needs, in ./data and ./.env:
#
#   ./setup.sh [DOMAIN PUBLIC_IP]
#
# - .env, with a database password and a relay secret made up here;
# - the root key (data/root/root.key, root.pub), where there is none;
# - a TLS certificate signed by itself for the master and the relay, where there is none
#   (data/master/master.crt, master.tls.key: put your own there to use another);
# - the configuration (data/config.json), where there is none, and its signed form
#   (data/master/config.signed.json), which the master serves;
# - the relay's settings (data/coturn/turnserver.conf).
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
chmod 700 data/root

docker compose build master
abconfig() { docker compose run --rm -T abconfig "$@"; }

if [ ! -f data/root/root.pub ]; then
    abconfig keygen -out root/root >/dev/null
    echo "made the root key: data/root/root.key (keep a copy somewhere safe)"
fi
KEY=$(cat data/root/root.pub)
set_env ONLINE_KEY "$KEY"

if [ ! -f data/master/master.crt ]; then
    abconfig selfsigned -host "$DOMAIN" -out master/master >/dev/null
    echo "made a TLS certificate signed by itself for $DOMAIN"
fi
PIN=$(abconfig pin -cert master/master.crt | tr -d '\r\n')

if [ ! -f data/config.json ]; then
    cat >data/config.json <<JSON
{
  "kind": "configuration",
  "master": {"url": "https://$DOMAIN:$MASTER_PORT", "tls_pins": ["$PIN"]},
  "stun": ["$DOMAIN:$TURN_PORT"],
  "turn": [
    {"host": "$DOMAIN", "port": $TURN_PORT, "transport": "udp"},
    {"host": "$DOMAIN", "port": $TURN_TLS_PORT, "transport": "tls"},
    {"host": "$DOMAIN", "port": $TURN_PORT, "transport": "tcp"}
  ],
  "protocol": {"min": $PROTOCOL_MIN, "current": $PROTOCOL},
  "status": {"state": "ok", "message": ""},
  "trust": {"roots": ["$KEY"], "revoked": []},
  "flags": {}
}
JSON
    echo "wrote the configuration: data/config.json"
elif ! grep -q "$PIN" data/config.json; then
    echo "WARNING: data/config.json does not name the pin of the master's present key ($PIN)." >&2
    echo "         Games will refuse the master. Put the pin in, or delete the file to have it written anew." >&2
fi

# The relay. Its secret is shared with the master (.env) and nobody else.
{
    echo "# Written by setup.sh from .env: changes made here are lost."
    echo "listening-port=$TURN_PORT"
    echo "tls-listening-port=$TURN_TLS_PORT"
    echo "listening-ip=$PRIVATE_IP"
    echo "relay-ip=$PRIVATE_IP"
    [ "$PRIVATE_IP" = "$PUBLIC_IP" ] || echo "external-ip=$PUBLIC_IP/$PRIVATE_IP"
    echo "min-port=$(get_env TURN_MIN_PORT)"
    echo "max-port=$(get_env TURN_MAX_PORT)"
    echo "use-auth-secret"
    echo "static-auth-secret=$(get_env TURN_SECRET)"
    echo "realm=$DOMAIN"
    echo "no-cli"
    echo "no-dtls"
    echo "cert=/tls/master.crt"
    echo "pkey=/tls/master.tls.key"
    echo "pidfile=/tmp/turnserver.pid"
    echo "userdb=/tmp/turndb"
    echo "log-file=stdout"
    echo "simple-log"
} >data/coturn/turnserver.conf
chmod 600 data/coturn/turnserver.conf

if [ -f data/root/root.key ]; then
    ./sign.sh
else
    echo "data/root/root.key is not here: sign data/config.json where the key is (./sign.sh /path/to/root.key)"
fi

cat <<TEXT

Ready. Start it:   docker compose up -d
Open in the firewall: TCP $MASTER_PORT, UDP+TCP $TURN_PORT, TCP $TURN_TLS_PORT, UDP $(get_env TURN_MIN_PORT)-$(get_env TURN_MAX_PORT)

What a game needs to know:
  --online-config https://$DOMAIN:$MASTER_PORT/config.json --online-key $KEY
or built in:
  cmake -DAB_ONLINE_CONFIG_URLS=https://$DOMAIN:$MASTER_PORT/config.json -DAB_ONLINE_CONFIG_KEY=$KEY

The configuration is good for seven days: run ./sign.sh at least once a week.
TEXT
