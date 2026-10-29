#!/usr/bin/env bash
# Run from any directory; provide the freshly built server as the first argument.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${1:-$ROOT/build-release/webserver_liburing}"
SERVER="$(realpath "$SERVER")"
DEMO_DIR="$(mktemp -d -t zerohttpd-demo.XXXXXX)"
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" || true
    fi
    rm -rf -- "$DEMO_DIR"
}
trap cleanup EXIT
"$SERVER" --root "$ROOT/public" --port 0 --access-log \
  --metrics-file "$DEMO_DIR/metrics.jsonl" >"$DEMO_DIR/startup.log" 2>"$DEMO_DIR/server.log" &
SERVER_PID=$!
for ((i=0; i<100; i++)); do
    if [[ -s "$DEMO_DIR/startup.log" ]]; then break; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then cat "$DEMO_DIR/server.log"; exit 1; fi
    sleep .02
done
PORT="$(sed -n 's/.*:\([0-9]*\)$/\1/p' "$DEMO_DIR/startup.log")"
if [[ -z "$PORT" ]]; then cat "$DEMO_DIR/server.log"; exit 1; fi
URL="http://127.0.0.1:$PORT"
printf '\nGET /\n'
curl --max-time 5 -sS -i "$URL/"
printf '\n\nHEAD /tux.png\n'
curl --max-time 5 -sS -I "$URL/tux.png"
printf '\nMissing file\n'
curl --max-time 5 -sS -i "$URL/missing"
printf '\nTwo requests with connection reuse\n'
curl --max-time 5 -sS -o /dev/null -w 'status=%{http_code} new_connections=%{num_connects}\n' \
    "$URL/" -o /dev/null "$URL/"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID"
SERVER_PID=""
printf '\nFinal metrics\n'
tail -n 1 "$DEMO_DIR/metrics.jsonl"
