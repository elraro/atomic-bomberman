#!/usr/bin/env bash
# End-to-end check of online play (docs/specifications/online-play.md, phase 4) with the
# real programs: the master server over TLS with a certificate signed by itself, a signed
# configuration fetched and verified, a dedicated game server that announces itself, and
# two clients that find it (one by its code, one in the list) and play a match.
#
#   tests/integration/online.sh BUILD_DIR MASTER_BIN_DIR
#
# BUILD_DIR holds atomic_server and net_bot; MASTER_BIN_DIR holds master and abconfig.
set -u
BUILD=${1:?build directory}
BIN=${2:?directory of the master programs}
WORK=$(mktemp -d)
MASTER_PORT=${MASTER_PORT:-38443}
GAME_PORT=${GAME_PORT:-37410}
pids=()
fail() {
    echo "FAIL: $*"
    for f in "$WORK"/*.log; do echo "--- $f"; tail -n 25 "$f"; done
    exit 1
}
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# Keys, a certificate, and a configuration that names the master by the pin of its key.
"$BIN/abconfig" keygen -out "$WORK/root" >/dev/null || fail "keygen"
KEY=$(cat "$WORK/root.pub")
"$BIN/abconfig" selfsigned -host 127.0.0.1 -out "$WORK/master" >/dev/null || fail "selfsigned"
PIN=$("$BIN/abconfig" pin -cert "$WORK/master.crt") || fail "pin"
cat > "$WORK/config.json" <<JSON
{
  "kind": "configuration",
  "master": {"url": "https://127.0.0.1:$MASTER_PORT", "tls_pins": ["$PIN"]},
  "stun": [], "turn": [],
  "protocol": {"min": 1, "current": 1},
  "status": {"state": "ok", "message": ""},
  "trust": {"roots": ["$KEY"], "revoked": [REVOKED]},
  "flags": {}
}
JSON
sed 's/REVOKED/424242/' "$WORK/config.json" > "$WORK/config-revoking.json"
sed -i 's/REVOKED//' "$WORK/config.json"
"$BIN/abconfig" sign -key "$WORK/root.key" -in "$WORK/config.json" -out "$WORK/signed.json" -stamp >/dev/null || fail "sign"
"$BIN/abconfig" verify -pub "$KEY" -in "$WORK/signed.json" >/dev/null || fail "verify"

"$BIN/master" -listen "127.0.0.1:$MASTER_PORT" -tls-cert "$WORK/master.crt" -tls-key "$WORK/master.tls.key" -config-file "$WORK/signed.json" >"$WORK/master.log" 2>&1 &
pids+=($!)
CONFIG="https://127.0.0.1:$MASTER_PORT/config.json"
ONLINE=(--online-config "$CONFIG" --online-key "$KEY")
for _ in $(seq 50); do grep -q "master listening" "$WORK/master.log" && break; sleep 0.1; done
grep -q "master listening" "$WORK/master.log" || fail "the master did not start"

# The first game server has a certificate: its operator shows its two keys to the owner of
# the root key and gets a certificate made out to them.
KEYFILES=(--identity-file "$WORK/id.key" --signing-key-file "$WORK/sign.key")
"$BUILD/atomic_server" --show-keys "${KEYFILES[@]}" >"$WORK/keys.txt" 2>&1 || fail "--show-keys"
IDENTITY=$(sed -n 's/^identity key: *//p' "$WORK/keys.txt")
SIGNING=$(sed -n 's/^signing key: *//p' "$WORK/keys.txt")
"$BIN/abconfig" cert -key "$WORK/root.key" -identity "$IDENTITY" -signing "$SIGNING" -operator "Integration Test" -serial 424242 -out "$WORK/server.cert" >/dev/null || fail "cert"
# Another server cannot use it: it is made out to keys that are not its own.
"$BUILD/atomic_server" --port $((GAME_PORT + 5)) --hidden --identity-file "$WORK/other-id.key" --signing-key-file "$WORK/other-sign.key" --certificate "$WORK/server.cert" >"$WORK/thief.log" 2>&1 </dev/null
grep -q "not a certificate for this server's keys" "$WORK/thief.log" || fail "a server should refuse to start with somebody else's certificate"

# A game server that is found by its code only (the one with the certificate), and one that is listed.
"$BUILD/atomic_server" --port "$GAME_PORT" --name "By code" --hidden --computers 1 --wins 1 --online code "${ONLINE[@]}" "${KEYFILES[@]}" --certificate "$WORK/server.cert" >"$WORK/server-code.log" 2>&1 </dev/null &
pids+=($!)
"$BUILD/atomic_server" --port $((GAME_PORT + 1)) --name "Listed game" --hidden --computers 1 --wins 1 --online listed "${ONLINE[@]}" >"$WORK/server-listed.log" 2>&1 </dev/null &
pids+=($!)
for _ in $(seq 100); do grep -q "Online: code, code" "$WORK/server-code.log" && grep -q "Online: listed, code" "$WORK/server-listed.log" && break; sleep 0.1; done
CODE=$(sed -n 's/.*Online: code, code \([0-9]*\).*/\1/p' "$WORK/server-code.log" | head -n 1)
[[ "$CODE" =~ ^[0-9]{6}$ ]] || fail "the game server was not announced (no six-digit code)"
grep -q "Online: listed, code" "$WORK/server-listed.log" || fail "the listed game server was not announced"
echo "ok   announced: code $CODE"

# By code: Ann starts the match, Bob joins it; both play it to the end.
timeout 150 "$BUILD/net_bot" "code:$CODE" Ann --start-after 3000 --matches 1 "${ONLINE[@]}" >"$WORK/ann.log" 2>&1 &
ANN=$!
sleep 1
timeout 150 "$BUILD/net_bot" "code:$CODE" Bob --matches 1 "${ONLINE[@]}" >"$WORK/bob.log" 2>&1 &
BOB=$!
# In the list: only the listed game, found by its name.
timeout 150 "$BUILD/net_bot" "listed:Listed game" Cat --start-after 1000 --matches 1 "${ONLINE[@]}" >"$WORK/cat.log" 2>&1 &
CAT=$!
wait $ANN || fail "Ann (by code) did not finish her match"
wait $BOB || fail "Bob (by code) did not finish his match"
wait $CAT || fail "Cat (from the list) did not finish her match"
grep -q "master: code $CODE is \"By code\"" "$WORK/ann.log" || fail "the code did not lead to the game"
grep -q "done: matches=1" "$WORK/ann.log" && grep -q "done: matches=1" "$WORK/bob.log" && grep -q "done: matches=1" "$WORK/cat.log" || fail "a match was not played to its end"
grep -q "master: 1 game(s) listed" "$WORK/cat.log" || fail "the list should show the listed game and not the one found by code"
echo "ok   two players by code, one from the list: matches played"
grep -q 'certificate: verified, operator "Integration Test", serial 424242' "$WORK/ann.log" || fail "the certificate should have been verified"
grep -q "certificate: none" "$WORK/cat.log" || fail "the listed server has no certificate"
grep -q "Certificate 424242 of \"Integration Test\": [0-9]* days left" "$WORK/server-code.log" || fail "the server should say whose certificate it shows"
echo "ok   the server's certificate is verified by the players' games"

# What must not work.
timeout 30 "$BUILD/net_bot" "code:000000" Dan "${ONLINE[@]}" >"$WORK/dan.log" 2>&1
grep -q "No game with that code" "$WORK/dan.log" || fail "a wrong code should be told so"
WRONG=$(printf 'ab%.0s' $(seq 32))
timeout 30 "$BUILD/net_bot" "code:$CODE" Eve --online-config "$CONFIG" --online-key "$WRONG" >"$WORK/eve.log" 2>&1
grep -q "not trusted" "$WORK/eve.log" || fail "a configuration signed by another key should be refused"
# A master showing another key than the configuration names (a second certificate on another port).
"$BIN/abconfig" selfsigned -host 127.0.0.1 -out "$WORK/other" >/dev/null
"$BIN/master" -listen "127.0.0.1:$((MASTER_PORT + 1))" -tls-cert "$WORK/other.crt" -tls-key "$WORK/other.tls.key" >"$WORK/master2.log" 2>&1 &
pids+=($!)
sed "s/:$MASTER_PORT\"/:$((MASTER_PORT + 1))\"/" "$WORK/config.json" > "$WORK/config2.json"
"$BIN/abconfig" sign -key "$WORK/root.key" -in "$WORK/config2.json" -out "$WORK/signed.json" -stamp -serial 9999999999 >/dev/null
sleep 0.5
timeout 30 "$BUILD/net_bot" "code:$CODE" Fay "${ONLINE[@]}" >"$WORK/fay.log" 2>&1
grep -q "does not match" "$WORK/fay.log" || fail "a master with another key than the pinned one should be refused"
echo "ok   refused: a wrong code, a configuration signed by another key, a master with another key"

# A revoked certificate: the configuration lists its serial, and the games no longer accept it.
"$BIN/abconfig" sign -key "$WORK/root.key" -in "$WORK/config-revoking.json" -out "$WORK/signed.json" -stamp -serial 99999999990 >/dev/null
timeout 60 "$BUILD/net_bot" "code:$CODE" Hal --matches 0 "${ONLINE[@]}" >"$WORK/hal.log" 2>&1
grep -q "certificate: not accepted (the certificate has been revoked)" "$WORK/hal.log" || fail "a revoked certificate should not be accepted"
echo "ok   a revoked certificate is no longer accepted"

# Stopping the game server takes its game off the master.
kill -TERM "${pids[1]}"
for _ in $(seq 50); do grep -q "Shutting down" "$WORK/server-code.log" && break; sleep 0.1; done
sleep 0.5
"$BIN/abconfig" sign -key "$WORK/root.key" -in "$WORK/config.json" -out "$WORK/signed.json" -stamp -serial 99999999999 >/dev/null
timeout 30 "$BUILD/net_bot" "code:$CODE" Gus "${ONLINE[@]}" >"$WORK/gus.log" 2>&1
grep -q "No game with that code" "$WORK/gus.log" || fail "a stopped game should be gone from the master"
echo "ok   a stopped game is withdrawn"
echo "online: all passed"
