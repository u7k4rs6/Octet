#!/usr/bin/env bash
# End-to-end tests: bcurl against bserve, then each against tests/wire.py.
set -u
cd "$(dirname "$0")/.."

BSERVE=./bserve
BCURL=./bcurl
TMP=tests/tmp
PORT=${PORT:-$((20000 + RANDOM % 20000))}
FAILS=0

pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
expect() { # name expected-exit actual-exit
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (exit $3, want $2)"; fi
}

rm -rf "$TMP" && mkdir -p "$TMP/root/sub" "$TMP/out"
cp www/* "$TMP/root/"
echo "top secret" > "$TMP/secret.txt"
ln -s ../secret.txt "$TMP/root/escape"
ln -s hello.txt "$TMP/root/inside"
head -c $((17 * 1024 * 1024 + 123)) /dev/urandom > "$TMP/root/big.bin"

$BSERVE "$TMP/root" "$PORT" 2> "$TMP/server.log" &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 50); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.1
done

H=localhost:$PORT

echo "== bcurl <-> bserve"
$BCURL $H/hello.txt > "$TMP/out/hello"; expect "GET /hello.txt exits 0" 0 $?
cmp -s "$TMP/out/hello" www/hello.txt && pass "body is byte-identical" || fail "body is byte-identical"

$BCURL $H/ > "$TMP/out/index"; expect "GET / exits 0" 0 $?
cmp -s "$TMP/out/index" www/index.html && pass "/ serves /index.html" || fail "/ serves /index.html"

$BCURL $H/nope > "$TMP/out/nope" 2>/dev/null; expect "GET /nope exits 1" 1 $?
[ ! -s "$TMP/out/nope" ] && pass "404 writes nothing to stdout" || fail "404 writes nothing to stdout"

$BCURL $H/empty.txt > "$TMP/out/empty"; expect "empty file exits 0" 0 $?
[ ! -s "$TMP/out/empty" ] && pass "empty file has empty body" || fail "empty file has empty body"

$BCURL $H/big.bin > "$TMP/out/big"; expect "17 MiB file exits 0" 0 $?
cmp -s "$TMP/out/big" "$TMP/root/big.bin" && pass "17 MiB file split across DATA frames intact" \
    || fail "17 MiB file split across DATA frames intact"

$BCURL -v $H/hello.txt /nope /hello.txt > "$TMP/out/multi" 2> "$TMP/out/multi.err"
expect "three requests, one 404, exits 1" 1 $?
[ "$(cat "$TMP/out/multi")" = "$(cat www/hello.txt www/hello.txt)" ] \
    && pass "keep-alive bodies in order" || fail "keep-alive bodies in order"
[ "$(grep -c '^> REQUEST' "$TMP/out/multi.err")" = 3 ] && pass "-v dumps every sent frame" \
    || fail "-v dumps every sent frame"
[ "$(grep -o '127.0.0.1:[0-9]*' "$TMP/server.log" | tail -3 | sort -u | wc -l)" = 1 ] \
    && pass "all three used one connection" || fail "all three used one connection"

$BCURL 127.0.0.1:1/x 2>/dev/null; expect "connection refused exits 2" 2 $?
$BCURL 2>/dev/null; expect "no arguments exits 2" 2 $?
$BCURL -v nohostport 2>/dev/null; expect "bad target exits 2" 2 $?

echo "== bserve against an independent implementation"
python3 tests/wire.py server-tests "$PORT" || FAILS=$((FAILS + 1))

echo "== bcurl against an independent implementation"
python3 tests/wire.py client-tests "$BCURL" || FAILS=$((FAILS + 1))

echo "== bserve process behaviour"
grep -q "/hello.txt 200" "$TMP/server.log" && pass "logs address, path, status" \
    || fail "logs address, path, status"
$BSERVE "$TMP/does-not-exist" "$PORT" 2>/dev/null; expect "missing root exits 1" 1 $?
$BSERVE "$TMP/root" "$PORT" 2>/dev/null; expect "port in use exits 1" 1 $?
kill -TERM $SRV; wait $SRV; expect "SIGTERM exits 0" 0 $?
trap - EXIT

echo
if [ $FAILS -eq 0 ]; then echo "all tests passed"; else echo "$FAILS failure(s)"; fi
exit $((FAILS > 0))
