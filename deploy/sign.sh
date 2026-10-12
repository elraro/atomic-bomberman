#!/usr/bin/env bash
# Signs the configuration (data/config.json) anew, good for seven days from now, and puts
# it where the master serves it. To be run at least once a week, and after every change
# to data/config.json.
#
#   ./sign.sh [ROOT_KEY_FILE]        default: data/root/root.key
set -euo pipefail
cd "$(dirname "$0")"
KEY_FILE=${1:-data/root/root.key}
[ -f "$KEY_FILE" ] || { echo "no root key at $KEY_FILE" >&2; exit 1; }
KEY_FILE=$(cd "$(dirname "$KEY_FILE")" && pwd)/$(basename "$KEY_FILE")
abconfig() { docker compose run --rm -T -v "$KEY_FILE:/root.key:ro" abconfig "$@"; }
abconfig sign -key /root.key -in config.json -out master/config.signed.json.new -stamp >/dev/null
mv data/master/config.signed.json.new data/master/config.signed.json
abconfig verify -pub "$(cat data/root/root.pub)" -in master/config.signed.json >/dev/null
echo "signed: data/master/config.signed.json"
