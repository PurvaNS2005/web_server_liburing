#!/usr/bin/env bash
#
# tests/smoke.sh -- end-to-end tests for the io_uring HTTP server.
#
# Starts webserver_liburing and drives it over a real socket, asserting on the
# bytes it actually sends back. Nothing here touches internal state, so these
# tests keep working if the implementation is rewritten.
#
# Usage:
#     ./tests/smoke.sh
#
# Environment:
#     SERVER_BIN   path to the built binary
#                  (default: <repo>/build/webserver_liburing)
#     PORT         port to test (default: 8000)
#
#                  The server hardcodes its port in webserver_liburing.c and does
#                  not parse argv, so PORT is only useful if you changed that
#                  constant and rebuilt.
#
# Exit status: 0 if no unexpected failures, 1 otherwise.
#
# Tests for bugs that are known but not yet fixed are marked XFAIL. They are
# expected to fail; if one starts passing the suite reports XPASS so you can
# drop the marker and turn it into a real test.

set -u -o pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${SERVER_BIN:-$ROOT/build/webserver_liburing}"
PORT="${PORT:-8000}"
HOST="127.0.0.1"

LOG="$(mktemp -t zerohttpd-smoke.XXXXXX)"
SERVER_PID=""

PASS=0
FAIL=0
XFAIL=0
XPASS=0
FAILED=()

# ---------------------------------------------------------------- reporting --

ok()    { PASS=$((PASS + 1));  printf '  ok     %s\n' "$1"; }
bad()   { FAIL=$((FAIL + 1));  FAILED+=("$1")
          printf '  FAIL   %s\n           expected: %s\n           actual:   %s\n' "$1" "$2" "$3"; }

# expect <description> <expected> <actual> [xfail-reason]
expect() {
    local desc="$1" want="$2" got="$3" reason="${4:-}"

    if [ "$want" = "$got" ]; then
        if [ -n "$reason" ]; then
            XPASS=$((XPASS + 1))
            printf '  XPASS  %s\n           the bug looks fixed -- drop the XFAIL marker\n' "$desc"
        else
            ok "$desc"
        fi
    elif [ -n "$reason" ]; then
        XFAIL=$((XFAIL + 1))
        printf '  xfail  %s\n           known bug: %s\n' "$desc" "$reason"
    else
        bad "$desc" "$want" "$got"
    fi
}

section() { printf '\n%s\n' "$1"; }

# ------------------------------------------------------------ server control --

# Readiness probe that does not depend on the server logging anything.
port_open() {
    ( exec 3<>"/dev/tcp/$HOST/$PORT" ) >/dev/null 2>&1
}

start_server() {
    : >"$LOG"
    # The server resolves public/ relative to the current directory, so it has
    # to be launched from the repo root regardless of where this script runs.
    #
    # stdbuf is not cosmetic. A C program's stdout is block-buffered whenever it
    # is redirected to a file, so "ZeroHTTPd listening on port: 8000" would sit
    # in the stdio buffer and never reach $LOG while the server blocked in its
    # event loop. Readiness is detected by polling the port instead, but line
    # buffering is still needed for the assertions that read $LOG.
    ( cd "$ROOT" && exec stdbuf -oL -eL "$SERVER_BIN" ) >>"$LOG" 2>&1 &
    SERVER_PID=$!

    local i
    for ((i = 0; i < 100; i++)); do
        if port_open; then
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            printf 'error: server exited during startup. Log:\n'
            sed 's/^/       /' "$LOG"
            return 1
        fi
        sleep 0.1
    done

    printf 'error: nothing accepted a connection on port %s within 10s. Log:\n' "$PORT"
    sed 's/^/       /' "$LOG"
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    SERVER_PID=""
}

# send_headers() lowercases the extension before matching, so a file whose name
# really does carry an uppercase extension must still get the right type.
#
# This deliberately creates its own fixture instead of requesting /TUX.PNG for
# the existing tux.png: that request depends on whether the filesystem is
# case-sensitive. On Linux (and in CI) it is a legitimate 404, because the file
# lookup happens before the extension is ever lowercased. Asking for a file that
# does not exist would test the filesystem, not the server.
CASE_FIXTURE="public/_case_check.PNG"

create_case_fixture() {
    cp "$ROOT/public/tux.png" "$ROOT/$CASE_FIXTURE" 2>/dev/null
}

drop_case_fixture() {
    rm -f "$ROOT/$CASE_FIXTURE" 2>/dev/null
}

cleanup() {
    stop_server
    drop_case_fixture
    rm -f "$LOG"
}
trap cleanup EXIT

# ------------------------------------------------------------- HTTP clients --

# Send a request verbatim over a real socket. Needed for cases where curl would
# rewrite what we send -- notably path traversal, which curl normalises away.
# Uses bash's /dev/tcp so there is no dependency on nc or telnet.
raw() {
    local req="$1" response=""
    exec 3<>"/dev/tcp/$HOST/$PORT" || return 1
    printf '%s' "$req" >&3 || true
    # The server is HTTP/1.0 and closes after each response, so this hits EOF.
    # The timeout is only a safety net against a hung connection.
    response="$(timeout 5 cat <&3)" || true
    exec 3<&- 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    printf '%s' "$response"
}

status_line() {
    printf '%s' "$1" | head -n 1 | awk '{ print $2 }'
}

# curl still prints %{http_code} when it exits non-zero, so capture the output
# and only substitute 000 when there is genuinely nothing to read.
# _code <path> <curl write-out field> [extra curl args...]
_code() {
    local path="$1" field="$2"
    shift 2
    local out
    out="$(curl -sS -o /dev/null -w "%{$field}" "$@" "http://$HOST:$PORT$path" 2>/dev/null)" || true
    [ -n "$out" ] || out="000"
    printf '%s' "$out"
}

code()  { _code "$1" http_code; }
ctype() { _code "$1" content_type; }
clen()  { _code "$1" size_download; }

# Status for a non-GET request. -X sends the method without a body; HEAD needs
# -I instead, because -X HEAD would leave curl waiting for a body it should not
# expect.
status_for_method() { _code "$2" http_code -X "$1"; }
status_for_head()  { _code "$1" http_code -I; }

# -------------------------------------------------------------------- setup --

if [ ! -x "$SERVER_BIN" ]; then
    printf 'error: server binary not found or not executable: %s\n' "$SERVER_BIN" >&2
    printf '       build it first:\n'
    printf '         cmake -S . -B build\n'
    printf '         cmake --build build --target webserver_liburing\n' >&2
    exit 2
fi

if [ ! -r "$ROOT/public/index.html" ]; then
    printf 'error: %s/public/index.html is missing\n' "$ROOT" >&2
    exit 2
fi

printf 'smoke tests against %s (port %s)\n' "$SERVER_BIN" "$PORT"

if ! start_server; then
    exit 2
fi

# ------------------------------------------------- static file serving --

section 'Static file serving'

expect 'GET / returns 200'                    '200' "$(code /)"
expect 'GET / is served as text/html'          'text/html' "$(ctype /)"
expect 'GET /body matches public/index.html'   'same' \
    "$( if [ "$(clen /)" = "$(wc -c <"$ROOT/public/index.html")" ]; then echo same; else echo different; fi )"

expect 'GET /index.html returns 200'           '200' "$(code /index.html)"
expect 'GET /tux.png returns 200'              '200' "$(code /tux.png)"
expect 'GET /tux.png is served as image/png'   'image/png' "$(ctype /tux.png)"
expect 'GET /tux.png body matches the file'    'same' \
    "$( if curl -sS "http://$HOST:$PORT/tux.png" 2>/dev/null | cmp -s - "$ROOT/public/tux.png"
       then echo same; else echo different; fi )"

expect 'Server header is zerohttpd/0.1'        'zerohttpd/0.1' \
    "$( curl -sS -D - -o /dev/null "http://$HOST:$PORT/" 2>/dev/null \
       | tr -d '\r' | awk 'tolower($1) == "server:" { print $2 }' )"

if create_case_fixture; then
    expect 'Uppercase extension is served (200)'   '200' "$(code /_case_check.PNG)"
    expect 'Uppercase extension gets the right type' 'image/png' "$(ctype /_case_check.PNG)"
else
    printf '  skip   uppercase extension tests (could not create %s)\n' "$CASE_FIXTURE"
fi

# ------------------------------------------------------ error handling --

section 'Error handling'

expect 'GET on a missing path returns 404'      '404' "$(code /no-such-file)"
expect 'POST is not implemented (400)'          '400' "$(status_for_method POST /)"
expect 'HEAD is not implemented (400)'          '400' "$(status_for_head /)"
expect 'PUT is not implemented (400)'           '400' "$(status_for_method PUT /)"
expect 'DELETE is not implemented (400)'        '400' "$(status_for_method DELETE /)"
expect 'Lowercase method "get" is accepted'     '200' \
    "$( raw $'get / HTTP/1.0\r\n\r\n' | head -n 1 | awk '{ print $2 }' )"

expect '404 body mentions Not Found'            'yes' \
    "$( raw $'GET /no-such-file HTTP/1.0\r\n\r\n' \
       | grep -qi '404 Not Found' && echo yes || echo no )"

# --------------------------------------------------------- known bugs --
#
# Each of these documents a real defect. They are expected to fail today.
# When one passes, the suite flags XPASS -- that is the signal to fix the test.

section 'Known bugs (expected failures)'

if [ -r /etc/passwd ]; then
    # handle_get_method() does strcpy(final_path, "public"); strcat(final_path, path)
    # with no normalisation, so any client that sends the dots itself can read
    # outside the document root. Browsers normalise the path; curl and nc do not.
    traversal="$( raw $'GET /../../../../../../../../../../etc/passwd HTTP/1.0\r\n\r\n' | head -n 1 | awk '{ print $2 }' )"
    expect 'path traversal outside public/ is refused' '404' "$traversal" \
           'handle_get_method concatenates the request path unchecked'
else
    printf '  skip   path traversal test (/etc/passwd not readable here)\n'
fi

# check_kernel_version() compares major >= 5 and minor >= 5 independently, so
# a 6.1 kernel is rejected even though it supports everything the server needs.
# On a healthy kernel (5.15, 6.6) this just confirms the server started; on
# 6.1-6.4 or 7.x it is the test that goes red.
expect 'kernel version check accepts this kernel' 'accepted' \
    "$( if grep -q 'Your kernel version is' "$LOG"; then echo accepted; else echo rejected; fi )"

# handle_client_request() calls exit(1) when get_line() finds no CRLF, which
# takes down every other connection in the process.
stop_server
start_server || exit 2
raw 'this request has no CRLF at all' >/dev/null 2>&1 || true
sleep 0.3
expect 'server survives a request with no CRLF'  '200' "$(code /)" \
       'handle_client_request calls exit(1) on a malformed request line'

# ------------------------------------------------------------- summary --

printf '\n---\n'
printf 'passed: %d   failed: %d   xfail: %d   xpass: %d\n' "$PASS" "$FAIL" "$XFAIL" "$XPASS"

if [ "$FAIL" -gt 0 ]; then
    printf '\nfailing tests:\n'
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi

if [ "$XPASS" -gt 0 ]; then
    printf '\nSome XFAIL tests now pass -- the underlying bugs may be fixed.\n'
fi

exit 0
