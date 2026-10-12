#!/usr/bin/env bash
# End-to-end check of online play (docs/specifications/online-play.md, phase 4) with the
# real programs: the master server over TLS with a certificate signed by itself, a signed
# configuration fetched and verified, a dedicated game server that announces itself, and
# two clients that find it (one by its code, one in the list) and play a match.
#
#   tests/integration/online.sh BUILD_DIR MASTER_BIN_DIR
#
# BUILD_DIR holds atomic_server, net_bot and atomic_verify; MASTER_BIN_DIR holds master and abconfig.
set -u
BUILD=${1:?build directory}
BIN=${2:?directory of the master programs}
WORK=$(mktemp -d)
# The programs keep what they remember (keys, the highest configuration serial seen) in
# the user's data folder: for this test, one of its own.
export XDG_DATA_HOME="$WORK/data" APPDATA="$WORK/data"
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
"$BUILD/atomic_server" --port "$GAME_PORT" --name "By code" --hidden --computers 1 --wins 1 --online code "${ONLINE[@]}" "${KEYFILES[@]}" --certificate "$WORK/server.cert" --records-dir "$WORK/records" >"$WORK/server-code.log" 2>&1 </dev/null &
pids+=($!)
"$BUILD/atomic_server" --port $((GAME_PORT + 1)) --name "Listed game" --hidden --computers 1 --wins 1 --online listed "${ONLINE[@]}" --identity-file "$WORK/id2.key" --signing-key-file "$WORK/sign2.key" --records-dir "$WORK/records2" >"$WORK/server-listed.log" 2>&1 </dev/null &
pids+=($!)
for _ in $(seq 100); do grep -q "Online: code, code" "$WORK/server-code.log" && grep -q "Online: listed, code" "$WORK/server-listed.log" && break; sleep 0.1; done
CODE=$(sed -n 's/.*Online: code, code \([0-9]*\).*/\1/p' "$WORK/server-code.log" | head -n 1)
[[ "$CODE" =~ ^[0-9]{6}$ ]] || fail "the game server was not announced (no six-digit code)"
grep -q "Online: listed, code" "$WORK/server-listed.log" || fail "the listed game server was not announced"
echo "ok   announced: code $CODE"

# By code: Ann starts the match, Bob joins it; both play it to the end.
timeout 150 "$BUILD/net_bot" "code:$CODE" Ann --start-after 3000 --matches 1 --player-key-file "$WORK/ann.key" "${ONLINE[@]}" >"$WORK/ann.log" 2>&1 &
ANN=$!
sleep 1
timeout 150 "$BUILD/net_bot" "code:$CODE" Bob --matches 1 --player-key-file "$WORK/bob.key" "${ONLINE[@]}" >"$WORK/bob.log" 2>&1 &
BOB=$!
# In the list: only the listed game, found by its name.
timeout 150 "$BUILD/net_bot" "listed:Listed game" Cat --start-after 1000 --matches 1 --no-ice "${ONLINE[@]}" >"$WORK/cat.log" 2>&1 &
CAT=$!
wait $ANN || fail "Ann (by code) did not finish her match"
wait $BOB || fail "Bob (by code) did not finish his match"
wait $CAT || fail "Cat (from the list) did not finish her match"
grep -q "master: code $CODE is \"By code\"" "$WORK/ann.log" || fail "the code did not lead to the game"
grep -q "done: matches=1" "$WORK/ann.log" && grep -q "done: matches=1" "$WORK/bob.log" && grep -q "done: matches=1" "$WORK/cat.log" || fail "a match was not played to its end"
grep -q "master: 1 game(s) listed" "$WORK/cat.log" || fail "the list should show the listed game and not the one found by code"
echo "ok   two players by code, one from the list: matches played"
# How they got there: Ann and Bob by a way found between the two ends (ICE; the descriptions
# went through the master), Cat at the address the master saw the server at.
grep -q "ice: a way to the host (Direct)" "$WORK/ann.log" && grep -q "path=Direct" "$WORK/ann.log" || fail "Ann should have reached the host by ICE"
grep -q "ice: a way to the host" "$WORK/bob.log" || fail "Bob should have reached the host by ICE"
grep -q "master: reach it at 127.0.0.1:$((GAME_PORT + 1))" "$WORK/cat.log" && ! grep -q "ice:" "$WORK/cat.log" || fail "Cat should have gone by the address"
grep -q "from .* (Direct)" "$WORK/server-code.log" || fail "the server should have taken in connections that came by ICE"
echo "ok   reached through ICE (two players) and by address (one)"
grep -q 'certificate: verified, operator "Integration Test", serial 424242' "$WORK/ann.log" || fail "the certificate should have been verified"
grep -q "certificate: none" "$WORK/cat.log" || fail "the listed server has no certificate"
grep -q "Certificate 424242 of \"Integration Test\": [0-9]* days left" "$WORK/server-code.log" || fail "the server should say whose certificate it shows"
echo "ok   the server's certificate is verified by the players' games"

# Match records: every round of the certified server left one; the players were handed
# theirs and found it to hold; anybody can play it again, and a changed one is found out.
sleep 2
RECORD=$(ls "$WORK/records"/*.abrec 2>/dev/null | head -n 1)
[ -n "$RECORD" ] || fail "the server kept no record"
grep -q "record of round 1: holds, certified by \"Integration Test\"" "$WORK/ann.log" || fail "Ann should have been handed a record that holds"
grep -q "record of round 1: holds" "$WORK/bob.log" || fail "Bob should have been handed a record that holds"
"$BUILD/atomic_verify" --root "$KEY" --stats "$WORK/records"/*.abrec >"$WORK/verify.log" 2>&1 || fail "the records should hold"
grep -q "HOLDS" "$WORK/verify.log" && grep -q "certificate of \"Integration Test\"" "$WORK/verify.log" || fail "atomic_verify should name the certificate"
grep -Eq "^Ann +[0-9]+ +[0-9]+ +-?[0-9]+" "$WORK/verify.log" && grep -Eq "^Bob +[0-9]+" "$WORK/verify.log" || fail "the statistics should list both players"
grep -q "problem" "$WORK/verify.log" && fail "a record that holds should have no problem"
# The server without a certificate keeps records too; they hold, but nobody answers for them,
# and with root keys given they do not count towards the statistics.
"$BUILD/atomic_verify" --root "$KEY" --stats "$WORK/records2"/*.abrec >"$WORK/verify2.log" 2>&1 || fail "the other server's records should hold"
grep -q "showed no certificate" "$WORK/verify2.log" || fail "a record without a certificate should say so"
grep -q "Players in 0 record" "$WORK/verify2.log" || fail "records nobody answers for should not count"
# One changed character in the middle of the record: found out.
python3 - "$RECORD" "$WORK/forged.abrec" <<'PY'
import base64, json, sys
env = json.load(open(sys.argv[1]))
doc = bytearray(base64.b64decode(env["document"]))
doc[len(doc) // 2] ^= 1
env["document"] = base64.b64encode(bytes(doc)).decode()
json.dump(env, open(sys.argv[2], "w"))
PY
"$BUILD/atomic_verify" --root "$KEY" "$WORK/forged.abrec" >"$WORK/verify3.log" 2>&1 && fail "a changed record should not hold"
grep -q "DOES NOT HOLD" "$WORK/verify3.log" || fail "a changed record should be said not to hold"
echo "ok   records: handed to the players, hold when played again, a changed one does not"

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
# --- A tournament: a signed file, no master involved ---
# Three players by their keys; a knock-out: Ann against Bob, the winner against Cy.
kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null; pids=()
T=$WORK/t
mkdir -p "$T"
"$BIN/abconfig" keygen -out "$T/organiser" >/dev/null || fail "organiser key"
ORG=$(cat "$T/organiser.pub")
for who in ann bob cy mal; do "$BUILD/net_bot" x x --player-key-file "$T/$who.key" --show-player-key >"$T/$who.pub" || fail "player key of $who"; done
"$BUILD/atomic_server" --show-keys --identity-file "$T/id.key" --signing-key-file "$T/sign.key" >"$T/keys.txt" 2>&1 || fail "--show-keys"
TPORT=$((GAME_PORT + 20))
echo "$(sed -n 's/^identity key: *//p' "$T/keys.txt") $(sed -n 's/^signing key: *//p' "$T/keys.txt") 127.0.0.1:$TPORT main" >"$T/servers.txt"
printf '%s Ann\n%s Bob\n%s Cy\n' "$(cat "$T/ann.pub")" "$(cat "$T/bob.pub")" "$(cat "$T/cy.pub")" >"$T/players.txt"
VERSION=$("$BUILD/atomic_server" --version | sed -n 's/.* server \([^ ]*\) (protocol \([0-9]*\)).*/\1/p')
PROTOCOL=$("$BUILD/atomic_server" --version | sed -n 's/.* server \([^ ]*\) (protocol \([0-9]*\)).*/\2/p')
"$BIN/abconfig" tournament-new -name "Integration Cup" -organiser "The Organiser" -players "$T/players.txt" -servers "$T/servers.txt" -wins 1 -play-time 60 \
    -game-version "$VERSION" -protocol "$PROTOCOL" -out "$T/cup.json" >/dev/null || fail "tournament-new"
"$BIN/abconfig" tournament-sign -key "$T/organiser.key" -in "$T/cup.json" -out "$T/cup.signed.json" -serial 1 >/dev/null || fail "tournament-sign"
CUP="$T/cup.signed.json#$ORG"
SERVERKEYS=(--identity-file "$T/id.key" --signing-key-file "$T/sign.key" --records-dir "$T/records")
# The second round cannot be served before the first is decided; nor any match by a server with other keys.
"$BUILD/atomic_server" --port "$TPORT" --hidden --tournament "$CUP" --match R2-1 "${SERVERKEYS[@]}" >"$T/early.log" 2>&1 </dev/null
grep -q "has not been decided" "$T/early.log" || fail "a match whose players are not known yet should not be served"
"$BUILD/atomic_server" --port "$TPORT" --hidden --tournament "$CUP" --match R1-1 --identity-file "$T/x.key" --signing-key-file "$T/y.key" >"$T/wrongserver.log" 2>&1 </dev/null
grep -q "not the server the tournament names" "$T/wrongserver.log" || fail "a server with other keys should not serve a tournament's match"
"$BUILD/atomic_server" --port "$TPORT" --hidden --tournament "$T/cup.signed.json#$KEY" --match R1-1 "${SERVERKEYS[@]}" >"$T/wrongkey.log" 2>&1 </dev/null
grep -q "not signed by the organiser" "$T/wrongkey.log" || fail "a tournament signed by somebody else should be refused"

"$BUILD/atomic_server" --port "$TPORT" --hidden --tournament "$CUP" --match R1-1 "${SERVERKEYS[@]}" >"$T/server.log" 2>&1 </dev/null &
pids+=($!)
for _ in $(seq 50); do grep -q "Tournament \"Integration Cup\" match R1-1: Ann, Bob" "$T/server.log" && break; sleep 0.1; done
grep -q "Tournament \"Integration Cup\" match R1-1: Ann, Bob" "$T/server.log" || fail "the tournament server did not start"
# Its players find their match in the file; nobody else gets in.
timeout 200 "$BUILD/net_bot" "tournament:$CUP" Ann --player-key-file "$T/ann.key" --start-after 3000 --matches 1 >"$T/ann.log" 2>&1 &
ANN=$!
sleep 1
timeout 200 "$BUILD/net_bot" "tournament:$CUP" Bob --player-key-file "$T/bob.key" --matches 1 >"$T/bob.log" 2>&1 &
BOB=$!
timeout 30 "$BUILD/net_bot" "tournament:$CUP" Cy --player-key-file "$T/cy.key" >"$T/cy.log" 2>&1
grep -q "no match to play" "$T/cy.log" || fail "Cy has no match yet (the second round waits for the first)"
timeout 30 "$BUILD/net_bot" "127.0.0.1:$TPORT" Cy --player-key-file "$T/cy.key" >"$T/cy2.log" 2>&1
grep -q "for its listed players only" "$T/cy2.log" || fail "a player of the tournament who is not in this match should not get in"
timeout 30 "$BUILD/net_bot" "127.0.0.1:$TPORT" Mallory >"$T/mal.log" 2>&1
grep -q "for its listed players only" "$T/mal.log" || fail "somebody without a key should not get in"
wait $ANN || fail "Ann did not finish her tournament match"
wait $BOB || fail "Bob did not finish his tournament match"
grep -q "tournament \"Integration Cup\": match R1-1 on main" "$T/ann.log" || fail "Ann should have found her match in the file"
sleep 2
WINNER=$(sed -n 's/.*CHAT  \* \(Ann\|Bob\) wins the match!.*/\1/p' "$T/server.log" | head -n 1)
[ -n "$WINNER" ] || fail "the tournament match had no winner"
LOSER=$([ "$WINNER" = Ann ] && echo Bob || echo Ann)
echo "ok   tournament: the match's players found it and played; others were kept out; $WINNER won"

# The organiser writes the result into the file; anybody can hold it against the records.
RECORDS=$(ls "$T/records"/*.abrec | paste -sd, -)
"$BIN/abconfig" tournament-result -in "$T/cup.json" -match R1-1 -winner "$WINNER" -records "$RECORDS" >/dev/null || fail "tournament-result"
"$BIN/abconfig" tournament-sign -key "$T/organiser.key" -in "$T/cup.json" -out "$T/cup2.signed.json" -serial 2 >/dev/null || fail "tournament-sign (2)"
"$BUILD/atomic_verify" --tournament "$T/cup2.signed.json#$ORG" "$T/records"/*.abrec >"$T/verify.log" 2>&1 || fail "the stated result should be borne out"
grep -q "match R1-1, won by $WINNER: borne out by its records" "$T/verify.log" || fail "atomic_verify should bear the result out"
# The same records do not bear out the other player as the winner, and a result without its records is not borne out.
"$BIN/abconfig" tournament-result -in "$T/cup.json" -match R1-1 -winner "$LOSER" -records "$RECORDS" >/dev/null
"$BIN/abconfig" tournament-sign -key "$T/organiser.key" -in "$T/cup.json" -out "$T/cup3.signed.json" -serial 3 >/dev/null
"$BUILD/atomic_verify" --tournament "$T/cup3.signed.json#$ORG" "$T/records"/*.abrec >"$T/verify3.log" 2>&1 && fail "a false result should not be borne out"
grep -q "NOT BORNE OUT" "$T/verify3.log" && grep -q "the records say $WINNER won" "$T/verify3.log" || fail "atomic_verify should say who the records name as the winner"
"$BUILD/atomic_verify" --tournament "$T/cup2.signed.json#$ORG" "$WORK/records"/*.abrec >"$T/verify4.log" 2>&1 && fail "records of another game should not bear a result out"
grep -q "not among the files given" "$T/verify4.log" || fail "missing records should be named"
# The next round: the winner's name stands where "the winner of R1-1" stood, and that match can now be served.
grep -q "\"$WINNER\"" <(python3 -c "import json,sys; f=json.load(open('$T/cup.json')); print(json.dumps([m['players'] for m in f['matches'] if m['id']=='R2-1']))") || fail "the winner should be in the next round"
echo "ok   tournament: the result is borne out by the records; a false one is not"
# --- The relay (TURN), where coturn is installed: players who hide their address, and
# players whose network lets no UDP out ---
# The relay sits at a second address of this machine (127.0.0.2; Linux answers to all of
# 127.x.x.x). So it sees the players come from another address than its own and refuses
# what one of them sends straight to the other's relayed address, as a relay elsewhere
# would: the ways found here are the relayed ones, not ways around the relay.
if command -v turnserver >/dev/null 2>&1; then
    R=$WORK/relay
    mkdir -p "$R"
    TURN_PORT=$((MASTER_PORT + 30))
    TURN_AT=127.0.0.2
    SECRET=integration-secret
    turnserver -n -v --no-cli --no-dtls --cert "$WORK/master.crt" --pkey "$WORK/master.tls.key" --tls-listening-port $((TURN_PORT + 1)) --listening-ip $TURN_AT --relay-ip $TURN_AT \
        --listening-port "$TURN_PORT" --min-port 41000 --max-port 41200 \
        --use-auth-secret --static-auth-secret "$SECRET" --realm atomic --allow-loopback-peers --log-file stdout --simple-log >"$R/coturn.log" 2>&1 &
    pids+=($!)
    TURNS="{\"host\": \"$TURN_AT\", \"port\": $TURN_PORT, \"transport\": \"udp\"}, {\"host\": \"$TURN_AT\", \"port\": $TURN_PORT, \"transport\": \"tcp\"}, {\"host\": \"$TURN_AT\", \"port\": $((TURN_PORT + 1)), \"transport\": \"tls\"}"
    sed "s/\"stun\": \[\], \"turn\": \[\]/\"stun\": [\"$TURN_AT:$TURN_PORT\"], \"turn\": [$TURNS]/; s/:$MASTER_PORT\"/:$((MASTER_PORT + 2))\"/" "$WORK/config.json" >"$R/config.json"
    grep -q "\"transport\": \"tls\"" "$R/config.json" || fail "the relay was not written into the configuration"
    "$BIN/abconfig" sign -key "$WORK/root.key" -in "$R/config.json" -out "$R/signed.json" -stamp -serial 999999999999 >/dev/null || fail "sign (relay)"
    "$BIN/master" -listen "127.0.0.1:$((MASTER_PORT + 2))" -tls-cert "$WORK/master.crt" -tls-key "$WORK/master.tls.key" -config-file "$R/signed.json" \
        -turn-secret "$SECRET" -turn-urls "turn:$TURN_AT:$TURN_PORT?transport=udp" >"$R/master.log" 2>&1 &
    pids+=($!)
    RELAYED=(--online-config "https://127.0.0.1:$((MASTER_PORT + 2))/config.json" --online-key "$KEY")
    for _ in $(seq 50); do grep -q "master listening" "$R/master.log" && break; sleep 0.1; done
    "$BUILD/atomic_server" --port $((GAME_PORT + 40)) --name "Hidden" --hidden --computers 1 --wins 1 --online code --hide-address "${RELAYED[@]}" \
        --identity-file "$R/id.key" --signing-key-file "$R/sign.key" --records-dir "$R/records" >"$R/server.log" 2>&1 </dev/null &
    pids+=($!)
    for _ in $(seq 100); do grep -q "Online: code, code" "$R/server.log" && break; sleep 0.1; done
    RCODE=$(sed -n 's/.*Online: code, code \([0-9]*\).*/\1/p' "$R/server.log" | head -n 1)
    [[ "$RCODE" =~ ^[0-9]{6}$ ]] || fail "the server behind the relay was not announced"
    timeout 150 "$BUILD/net_bot" "code:$RCODE" Rex --start-after 2000 --matches 1 --hide-address "${RELAYED[@]}" >"$R/rex.log" 2>&1 || { cat "$R/rex.log" "$R/server.log"; tail -n 30 "$R/coturn.log"; fail "Rex did not finish a match through the relay"; }
    grep -q "ice: a way to the host (Relay)" "$R/rex.log" || { echo "FAIL detail: $(grep "ice:" "$R/rex.log" | head -n 1)"; fail "Rex should have reached the host through the relay"; }
    grep -qi "allocate" "$R/coturn.log" || {
        tail -n 30 "$R/coturn.log"
        echo "FAIL detail: $(grep -c . "$R/coturn.log") lines of coturn log; last: $(tail -n 1 "$R/coturn.log" | cut -c1-200)"
        echo "FAIL detail: $(grep "ice:" "$R/rex.log" | head -n 1)"
        fail "coturn should have made an allocation"
    }
    echo "ok   through coturn: a hidden player and a hidden host played a match"

    # A player whose network lets no UDP out reaches the relay over TCP, another over TLS:
    # the same TURN, in a stream (and from the relay on to the host's relayed address, over UDP).
    for way in tcp tls; do
        timeout 150 "$BUILD/net_bot" "code:$RCODE" "Tess-$way" --start-after 2000 --matches 1 "--relay-$way" "${RELAYED[@]}" >"$R/$way.log" 2>&1 || { cat "$R/$way.log"; tail -n 30 "$R/coturn.log"; fail "a player did not finish a match through the relay over $way"; }
        grep -q "ice: a way to the host (Relay (TCP))" "$R/$way.log" || { echo "FAIL detail: $(grep "ice:" "$R/$way.log" | head -n 1)"; fail "the way over $way should have been the relayed one"; }
    done
    echo "ok   through coturn over TCP and over TLS: a player without UDP played a match each way"
else
    echo "skip the relay: coturn (turnserver) is not installed here"
fi
echo "online: all passed"
