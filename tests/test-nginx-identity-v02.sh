#!/usr/bin/env bash
set -Eeuo pipefail

# NGINX Upstream Identity Monitor v0.2 local validation harness.
#
# This script is self-contained for local WSL/Linux testing. It starts and
# stops the HTTPS test backend, C history collector and an isolated NGINX
# instance. No pre-running backend or collector is required.
#
# Usage:
#   ./tests/test-nginx-identity-v02.sh transport
#   ./tests/test-nginx-identity-v02.sh identity
#   ./tests/test-nginx-identity-v02.sh collector-failure
#   ./tests/test-nginx-identity-v02.sh reload
#   ./tests/test-nginx-identity-v02.sh restart
#   ./tests/test-nginx-identity-v02.sh concurrency
#   ./tests/test-nginx-identity-v02.sh status
#   ./tests/test-nginx-identity-v02.sh collector-rotation
#   ./tests/test-nginx-identity-v02.sh all

REPO="${REPO:-$HOME/source/nginx-extension-poc}"
NGINX_SRC="${NGINX_SRC:-$HOME/source/nginx-1.28.0}"
NGINX_BIN="${NGINX_BIN:-$HOME/nginx-upstream-identity-poc/sbin/nginx}"

TEST_ROOT="${TEST_ROOT:-/tmp/nginx-upstream-identity-v02-test}"
PROXY_PORT="${PROXY_PORT:-18080}"
BACKEND_PORT="${BACKEND_PORT:-19443}"
STATUS_PORT="${STATUS_PORT:-18081}"

SOCKET="$TEST_ROOT/history.sock"
HISTORY="$TEST_ROOT/upstream-identity.jsonl"
ROTATED_HISTORY="$TEST_ROOT/upstream-identity.jsonl.1"
NGINX_PREFIX="$TEST_ROOT/nginx"
NGINX_CONF="$NGINX_PREFIX/conf/nginx.conf"
NGINX_LOG="$NGINX_PREFIX/logs/error.log"
MODULE_BUILD="$NGINX_SRC/objs/ngx_http_upstream_identity_module.so"
MODULE_INSTALLED="$NGINX_PREFIX/modules/ngx_http_upstream_identity_module.so"
COLLECTOR_BIN="$TEST_ROOT/history_collector"
SENDER_BIN="$TEST_ROOT/history_sender"

BACKEND_PID=""
COLLECTOR_PID=""
NGINX_STARTED=0

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
pass() { printf '\033[1;32mPASS: %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

history_count() {
    if [[ -f "$HISTORY" ]]; then wc -l < "$HISTORY"; else echo 0; fi
}

wait_for_file() {
    local path="$1"
    for _ in $(seq 1 50); do
        [[ -e "$path" ]] && return 0
        sleep 0.1
    done
    return 1
}

wait_for_backend() {
    for _ in $(seq 1 50); do
        if curl -kfsS --max-time 1 "https://127.0.0.1:$BACKEND_PORT/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_nginx() {
    for _ in $(seq 1 50); do
        if curl -fsS --max-time 1 "http://127.0.0.1:$PROXY_PORT/__health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

stop_backend() {
    if [[ -n "${BACKEND_PID:-}" ]]; then
        kill "$BACKEND_PID" 2>/dev/null || true
        wait "$BACKEND_PID" 2>/dev/null || true
        BACKEND_PID=""
    fi
}

stop_collector() {
    if [[ -n "${COLLECTOR_PID:-}" ]]; then
        kill "$COLLECTOR_PID" 2>/dev/null || true
        wait "$COLLECTOR_PID" 2>/dev/null || true
        COLLECTOR_PID=""
    fi
    rm -f "$SOCKET"
}

stop_nginx() {
    if [[ "$NGINX_STARTED" -eq 1 ]]; then
        "$NGINX_BIN" -p "$NGINX_PREFIX/" -c conf/nginx.conf -s quit >/dev/null 2>&1 || true
        for _ in $(seq 1 50); do
            [[ ! -f "$NGINX_PREFIX/logs/nginx.pid" ]] && break
            sleep 0.1
        done
        NGINX_STARTED=0
    fi
}

cleanup() {
    stop_nginx
    stop_backend
    stop_collector
}
trap cleanup EXIT INT TERM

build_tools() {
    log "Building C history tools"
    mkdir -p "$TEST_ROOT"
    cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
        "$REPO/lab/history_collector.c" -o "$COLLECTOR_BIN"
    cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
        "$REPO/lab/history_sender.c" -o "$SENDER_BIN"
}

build_module() {
    log "Building NGINX dynamic module"
    (
        cd "$NGINX_SRC"
        ./configure \
            --with-http_ssl_module \
            --with-compat \
            --add-dynamic-module="$REPO" >/dev/null
        make -j"$(nproc)" modules
    )
    [[ -s "$MODULE_BUILD" ]] || fail "module was not produced: $MODULE_BUILD"
}

prepare_nginx_prefix() {
    log "Preparing isolated NGINX prefix"
    rm -rf "$NGINX_PREFIX"
    mkdir -p "$NGINX_PREFIX/conf" "$NGINX_PREFIX/logs" "$NGINX_PREFIX/modules"
    cp "$MODULE_BUILD" "$MODULE_INSTALLED"

    cat > "$NGINX_CONF" <<EOF
load_module modules/ngx_http_upstream_identity_module.so;

worker_processes 4;
error_log logs/error.log notice;
pid logs/nginx.pid;

events {
    worker_connections 2048;
}

http {
    upstream_identity_state_zone upstream_identity_state 1m;
    upstream_identity_history_socket $SOCKET;

    upstream backend_tls {
        server 127.0.0.1:$BACKEND_PORT;
        keepalive 16;
        upstream_identity_peer_monitor;
    }

    server {
        listen 127.0.0.1:$PROXY_PORT;

        location = /__health {
            access_log off;
            return 204;
        }

        location / {
            proxy_ssl_verify off;
            proxy_ssl_server_name on;
            proxy_ssl_name backend.test;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_pass https://backend_tls;
        }
    }

    server {
        listen 127.0.0.1:$STATUS_PORT;

        location = /upstream-identity-status {
            allow 127.0.0.1;
            deny all;
            upstream_identity_status upstream_identity_state;
        }
    }
}
EOF

    "$NGINX_BIN" -p "$NGINX_PREFIX/" -c conf/nginx.conf -t
}

start_backend() {
    local cert="$1"
    local key="$2"
    stop_backend
    log "Starting backend: $(basename "$cert")"
    python3 "$REPO/lab/https_keepalive_server.py" \
        "$BACKEND_PORT" "$cert" "$key" >"$TEST_ROOT/backend.log" 2>&1 &
    BACKEND_PID=$!
    wait_for_backend || fail "HTTPS backend failed to start"
}

start_backend_a() {
    start_backend "$REPO/lab/certs/backend-a.cert.pem" "$REPO/lab/certs/backend-a.key.pem"
}

start_backend_a_renewed() {
    start_backend "$REPO/lab/certs/backend-a-renewed.cert.pem" "$REPO/lab/certs/backend-a.key.pem"
}

start_backend_b() {
    start_backend "$REPO/lab/certs/backend-b.cert.pem" "$REPO/lab/certs/backend-b.key.pem"
}

start_collector() {
    local sync_mode="${1:-always}"
    log "Starting C history collector (--sync $sync_mode)"
    rm -f "$SOCKET"
    mkdir -p "$(dirname "$HISTORY")"
    touch "$HISTORY"
    "$COLLECTOR_BIN" --sync "$sync_mode" "$SOCKET" "$HISTORY" \
        >"$TEST_ROOT/collector.log" 2>&1 &
    COLLECTOR_PID=$!
    wait_for_file "$SOCKET" || fail "collector socket was not created"
    [[ -S "$SOCKET" ]] || fail "$SOCKET is not a Unix socket"
}

start_nginx() {
    log "Starting isolated NGINX"
    "$NGINX_BIN" -p "$NGINX_PREFIX/" -c conf/nginx.conf
    NGINX_STARTED=1
    wait_for_nginx || fail "NGINX failed to become ready"
}

reload_nginx() {
    "$NGINX_BIN" -p "$NGINX_PREFIX/" -c conf/nginx.conf -s reload
    sleep 1
}

request_once() {
    curl -fsS --max-time 3 "http://127.0.0.1:$PROXY_PORT/" >/dev/null
}

status_json() {
    curl -fsS --max-time 3 "http://127.0.0.1:$STATUS_PORT/upstream-identity-status"
}

expect_delta() {
    local before="$1"
    local expected="$2"
    local description="$3"
    sleep 0.2
    local after
    after="$(history_count)"
    local delta=$((after - before))
    if [[ "$delta" -ne "$expected" ]]; then
        echo "History:"; cat "$HISTORY" || true
        echo "NGINX log:"; tail -n 80 "$NGINX_LOG" || true
        fail "$description: expected delta=$expected, got delta=$delta"
    fi
    pass "$description"
}

expect_last_change() {
    local expected="$1"
    grep -F "\"change\":\"$expected\"" "$HISTORY" | tail -n 1 >/dev/null || \
        fail "expected change '$expected' not found"
    pass "found change=$expected"
}

expect_status_contains() {
    local needle="$1"
    local json
    json="$(status_json)"
    printf '%s\n' "$json" | grep -F "$needle" >/dev/null || {
        printf 'Status JSON:\n%s\n' "$json"
        fail "status JSON missing: $needle"
    }
    pass "status contains $needle"
}

fresh_runtime() {
    cleanup
    rm -rf "$TEST_ROOT"
    mkdir -p "$TEST_ROOT"
    build_tools
    build_module
    prepare_nginx_prefix
    : > "$HISTORY"
}

test_transport() {
    log "TEST: standalone C Unix-datagram transport"
    cleanup
    rm -rf "$TEST_ROOT"
    mkdir -p "$TEST_ROOT"
    build_tools
    : > "$HISTORY"
    start_collector none
    "$SENDER_BIN" "$SOCKET" \
        '{"schema_version":1,"timestamp":1786720000,"change":"first_seen","upstream":"backend_tls","peer":"127.0.0.1:19443","previous_cert_sha256":"","current_cert_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","previous_spki_sha256":"","current_spki_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}'
    sleep 0.2
    [[ "$(history_count)" -eq 1 ]] || fail "collector should contain exactly one record"
    expect_last_change first_seen
    pass "standalone C transport"
}

test_identity() {
    log "TEST: identity sequence"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx

    [[ "$(history_count)" -eq 0 ]] || fail "NGINX startup unexpectedly generated an identity event"

    local before
    before="$(history_count)"; request_once
    expect_delta "$before" 1 "first A observation generates one event"
    expect_last_change first_seen

    before="$(history_count)"
    for _ in $(seq 1 20); do request_once; done
    expect_delta "$before" 0 "stable identity produces no event"

    start_backend_a_renewed
    before="$(history_count)"; request_once
    expect_delta "$before" 1 "same-key certificate rotation produces exactly one event"
    expect_last_change certificate_changed_same_key

    start_backend_b
    before="$(history_count)"; request_once
    expect_delta "$before" 1 "public-key rotation produces exactly one event"
    expect_last_change public_key_changed
    pass "identity sequence"
}

test_collector_failure() {
    log "TEST: collector failure isolation and recovery"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx
    request_once
    local before
    before="$(history_count)"

    stop_collector
    start_backend_b
    for _ in $(seq 1 20); do
        request_once || fail "proxy request failed while collector was absent"
    done
    [[ "$(history_count)" -eq "$before" ]] || fail "history changed while collector was offline"
    pass "proxy remains healthy with collector absent"

    start_collector
    start_backend_a
    before="$(history_count)"; request_once
    expect_delta "$before" 1 "event export resumes after collector restart"
    expect_last_change public_key_changed
    pass "collector failure isolation and recovery"
}

test_reload() {
    log "TEST: NGINX reload preserves shared baseline"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx
    request_once
    local before
    before="$(history_count)"
    reload_nginx
    for _ in $(seq 1 20); do request_once; done
    expect_delta "$before" 0 "reload does not generate a new first_seen"
    pass "reload preservation"
}

test_restart() {
    log "TEST: durable history survives full NGINX restart"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx
    request_once
    local before
    before="$(history_count)"
    [[ "$before" -eq 1 ]] || fail "expected one initial durable event before restart"
    stop_nginx
    [[ "$(history_count)" -eq "$before" ]] || fail "durable history changed during shutdown"
    start_nginx
    request_once
    expect_delta "$before" 1 "full restart creates a new runtime first_seen"
    expect_last_change first_seen
    pass "durable history survives full restart"
}

test_concurrency() {
    log "TEST: four-worker concurrent identity transition"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx
    request_once
    start_backend_b
    local before
    before="$(history_count)"
    seq 1 100 | xargs -P32 -I{} curl -fsS --max-time 5 \
        "http://127.0.0.1:$PROXY_PORT/" -o /dev/null
    expect_delta "$before" 1 "concurrent transition produces exactly one shared-state event"
    expect_last_change public_key_changed
    if grep -E 'signal 11|segfault|worker process .* exited on signal' "$NGINX_LOG" >/dev/null; then
        tail -n 100 "$NGINX_LOG"
        fail "worker crash detected"
    fi
    pass "four-worker concurrency"
}

test_status() {
    log "TEST: v0.2 operational status endpoint and counters"
    fresh_runtime
    start_collector
    start_backend_a
    start_nginx

    # Empty state before first upstream request.
    expect_status_contains '"schema_version":2'
    expect_status_contains '"peers_tracked":0'

    request_once
    sleep 0.2

    expect_status_contains '"peers_tracked":1'
    expect_status_contains '"events_generated":1'
    expect_status_contains '"events_exported":1'
    expect_status_contains '"events_dropped":0'
    expect_status_contains '"export_errors":0'
    expect_status_contains '"identity_changes":0'
    expect_status_contains '"upstream":"backend_tls"'
    expect_status_contains "\"peer\":\"127.0.0.1:$BACKEND_PORT\""
    expect_status_contains '"observations":1'
    expect_status_contains '"changes":0'

    start_backend_a_renewed
    request_once
    sleep 0.2

    expect_status_contains '"events_generated":2'
    expect_status_contains '"events_exported":2'
    expect_status_contains '"identity_changes":1'
    expect_status_contains '"changes":1'

    start_backend_b
    request_once
    sleep 0.2

    expect_status_contains '"events_generated":3'
    expect_status_contains '"events_exported":3'
    expect_status_contains '"identity_changes":2'
    expect_status_contains '"changes":2'

    # Collector failure must be visible in counters while traffic remains healthy.
    stop_collector
    start_backend_a
    request_once
    sleep 0.2

    expect_status_contains '"events_generated":4'
    expect_status_contains '"events_exported":3'
    expect_status_contains '"identity_changes":3'

    local json
    json="$(status_json)"
    if ! printf '%s\n' "$json" | grep -E '"events_dropped":[1-9][0-9]*|"export_errors":[1-9][0-9]*' >/dev/null; then
        printf 'Status JSON:\n%s\n' "$json"
        fail "collector failure was not reflected in export failure counters"
    fi
    pass "collector failure reflected in status counters"

    # HEAD is allowed; POST is not.
    curl -fsSI --max-time 3 "http://127.0.0.1:$STATUS_PORT/upstream-identity-status" >/dev/null || \
        fail "HEAD status request failed"

    local post_code
    post_code="$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        "http://127.0.0.1:$STATUS_PORT/upstream-identity-status")"
    [[ "$post_code" == "405" ]] || fail "expected POST status=405, got $post_code"
    pass "status endpoint method restrictions"

    pass "v0.2 operational status endpoint and counters"
}

test_collector_rotation() {
    log "TEST: v0.2 collector SIGHUP history reopen"
    cleanup
    rm -rf "$TEST_ROOT"
    mkdir -p "$TEST_ROOT"
    build_tools
    : > "$HISTORY"
    start_collector none

    local payload1 payload2
    payload1='{"schema_version":1,"timestamp":1,"change":"first_seen","upstream":"rotation","peer":"127.0.0.1:443","previous_cert_sha256":"","current_cert_sha256":"a","previous_spki_sha256":"","current_spki_sha256":"b"}'
    payload2='{"schema_version":1,"timestamp":2,"change":"public_key_changed","upstream":"rotation","peer":"127.0.0.1:443","previous_cert_sha256":"a","current_cert_sha256":"c","previous_spki_sha256":"b","current_spki_sha256":"d"}'

    "$SENDER_BIN" "$SOCKET" "$payload1"
    sleep 0.2
    [[ "$(history_count)" -eq 1 ]] || fail "first event was not written before rotation"

    mv "$HISTORY" "$ROTATED_HISTORY"
    : > "$HISTORY"
    chmod 600 "$HISTORY"
    kill -HUP "$COLLECTOR_PID"
    sleep 0.2

    "$SENDER_BIN" "$SOCKET" "$payload2"
    sleep 0.2

    [[ "$(wc -l < "$ROTATED_HISTORY")" -eq 1 ]] || fail "rotated file changed unexpectedly"
    [[ "$(history_count)" -eq 1 ]] || fail "new history file did not receive post-SIGHUP event"
    grep -F '"change":"public_key_changed"' "$HISTORY" >/dev/null || \
        fail "post-SIGHUP event missing from reopened history"
    pass "collector SIGHUP history reopen"
}

run_all() {
    test_transport
    test_identity
    test_collector_failure
    test_reload
    test_restart
    test_concurrency
    test_status
    test_collector_rotation
    echo
    echo '============================================================'
    printf '\033[1;32mALL V0.2 LOCAL TESTS PASSED\033[0m\n'
    echo '============================================================'
}

case "${1:-}" in
    transport) test_transport ;;
    identity) test_identity ;;
    collector-failure) test_collector_failure ;;
    reload) test_reload ;;
    restart) test_restart ;;
    concurrency) test_concurrency ;;
    status) test_status ;;
    collector-rotation) test_collector_rotation ;;
    all) run_all ;;
    *)
        cat <<EOF
Usage:
  $0 transport
  $0 identity
  $0 collector-failure
  $0 reload
  $0 restart
  $0 concurrency
  $0 status
  $0 collector-rotation
  $0 all
EOF
        exit 2
        ;;
esac
