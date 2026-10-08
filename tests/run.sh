#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."
make -s || exit 1

PORT=${PORT:-9137}
ROOT=$(mktemp -d) || exit 1
OUT=$(mktemp -d) || exit 1
cp www/* "$ROOT"
head -c 100000 /dev/urandom > "$ROOT/big.bin"

./bserve "$ROOT" "$PORT" 2> "$OUT/server.log" &
SERVER=$!
./bserve --unknown-frame "$ROOT" $((PORT + 1)) 2> "$OUT/server2.log" &
SERVER2=$!
trap 'kill $SERVER $SERVER2 2>/dev/null; wait 2>/dev/null; rm -rf "$ROOT" "$OUT"' EXIT

for _ in $(seq 50); do
  ./bcurl "localhost:$PORT/" > /dev/null 2>&1 && ./bcurl "localhost:$((PORT + 1))/" > /dev/null 2>&1 && break
  sleep 0.1
done

failed=0
check() {
  if [ "$2" = 0 ]; then echo "ok - $1"; else echo "not ok - $1"; failed=1; fi
}

./bcurl "localhost:$PORT/index.html" > "$OUT/a"
check "bcurl prints the body" $?
cmp -s "$OUT/a" www/index.html
check "body matches the file" $?

./bcurl "localhost:$PORT/big.bin" | cmp -s - "$ROOT/big.bin"
check "100 KB file arrives intact over several DATA frames" $?

./bcurl -v "localhost:$PORT/big.bin" 2> "$OUT/v" > /dev/null
[ "$(grep -c '^< DATA' "$OUT/v")" = 7 ] && [ "$(grep -c ' END length=' "$OUT/v")" = 2 ]
check "-v dumps 7 DATA frames, END on request and last DATA only" $?

./bcurl "localhost:$PORT/missing" > /dev/null 2>&1
check "exit code 4 on 404" $(( $? != 4 ))

./bcurl "localhost:$PORT/a" "otherhost:$PORT/b" > /dev/null 2>&1
check "refuses URLs that need a second connection" $(( $? != 2 ))

./bcurl "localhost:$PORT/index.html" "localhost:$PORT/big.bin" "localhost:$PORT/index.html" > "$OUT/multi"
cat www/index.html "$ROOT/big.bin" www/index.html | cmp -s - "$OUT/multi"
check "three URLs come back in order" $?
[ "$(tail -n 3 "$OUT/server.log" | cut -d' ' -f2 | sort -u | wc -l | tr -d ' ')" = 1 ]
check "all three went over one connection" $?

./bcurl -v "localhost:$((PORT + 1))/index.html" 2> "$OUT/v2" > "$OUT/b"
check "bcurl skips unknown frames from the server" $?
grep -q 'UNKNOWN(0x7f)' "$OUT/v2" && cmp -s "$OUT/b" www/index.html
check "the unknown frame really was sent" $?

./bcurl --unknown-frame "localhost:$PORT/index.html" > /dev/null
check "bserve skips unknown frames from the client" $?
grep -q 'skipped unknown frame type 0x7f' "$OUT/server.log"
check "the server logged the skip" $?

python3 tests/raw_frames.py "$PORT" www/index.html || failed=1

exit $failed
