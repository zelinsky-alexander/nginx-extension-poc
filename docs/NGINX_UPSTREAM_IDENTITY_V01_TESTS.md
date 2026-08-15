# NGINX Upstream Identity Monitor v0.1 — Local Test Harness Guide

This document explains exactly what each test in `tests/test-nginx-identity-v01.sh` does, what components it starts, what behavior it validates, and what constitutes a pass or failure.

The harness is intentionally self-contained. For runtime tests it creates an isolated NGINX prefix, builds the module and C test tools, starts the controlled HTTPS backend and C history collector itself, runs the requested scenario, checks the expected durable-history behavior, and then cleans up.

## 1. Test environment created by the harness

By default, the harness uses:

```text
Repository:
  ~/source/nginx-extension-poc

NGINX source:
  ~/source/nginx-1.28.0

NGINX binary:
  ~/nginx-upstream-identity-poc/sbin/nginx

Temporary isolated test root:
  /tmp/nginx-upstream-identity-v01-test

Proxy listener:
  127.0.0.1:18080

HTTPS backend:
  127.0.0.1:19443

Unix datagram socket:
  /tmp/nginx-upstream-identity-v01-test/history.sock

Durable JSONL history:
  /tmp/nginx-upstream-identity-v01-test/upstream-identity.jsonl
```

The isolated NGINX configuration is generated dynamically by the script under:

```text
/tmp/nginx-upstream-identity-v01-test/nginx/conf/nginx.conf
```

The generated config contains a local readiness endpoint:

```nginx
location = /__health {
    access_log off;
    return 204;
}
```

This endpoint is important because the harness can verify that NGINX has started without making an upstream request and accidentally generating `first_seen`.

The normal monitored proxy path remains:

```nginx
location / {
    proxy_ssl_verify off;
    proxy_ssl_server_name on;
    proxy_ssl_name backend.test;

    proxy_http_version 1.1;
    proxy_set_header Connection "";

    proxy_pass https://backend_tls;
}
```

## 2. Common setup performed by runtime tests

Most tests use the `fresh_runtime()` setup sequence.

It performs the following steps:

```text
cleanup previous test processes
        |
        v
remove previous temporary test directory
        |
        v
build C history collector
        |
        v
build C history sender
        |
        v
build NGINX dynamic module
        |
        v
create isolated NGINX prefix
        |
        v
copy module into isolated modules directory
        |
        v
generate nginx.conf
        |
        v
run nginx -t
```

The principal processes are:

```text
C history collector
    ^
    |
AF_UNIX / SOCK_DGRAM
    |
NGINX workers
    |
    v
HTTPS test backend
```

The HTTPS backend is currently implemented by `lab/https_keepalive_server.py`. Python is used only for the deterministic TLS test backend. The module, shared state, structured exporter, history collector, history sender and durable-history path are C.

## 3. `transport`

Run:

```bash
./test-nginx-identity-v01.sh transport
```

### Purpose

This test verifies the C Unix-domain datagram transport and C collector **without starting NGINX**. It isolates the durable event transport from the rest of the module.

### Sequence

```text
build history_collector.c
        |
        v
build history_sender.c
        |
        v
start C collector
        |
        v
collector binds history.sock
        |
        v
C sender sends one valid JSON event
        |
        v
collector receives datagram
        |
        v
collector validates event
        |
        v
collector appends event to JSONL
        |
        v
test verifies one line exists
```

### What it validates

- C collector compiles cleanly.
- C sender compiles cleanly.
- Collector can create and bind the Unix datagram socket.
- Sender can deliver a datagram to it.
- Collector accepts a correctly structured message.
- Collector writes exactly one JSONL record.
- Written record contains `"change":"first_seen"`.

### Pass condition

```text
PASS: found change=first_seen
PASS: standalone C transport
```

### What it does not validate

It does not test NGINX, TLS, upstream selection, shared memory, certificate hashing, SPKI hashing, or identity change classification.

## 4. `identity`

Run:

```bash
./test-nginx-identity-v01.sh identity
```

### Purpose

This is the main functional identity-transition test. It validates the complete path:

```text
real TLS upstream
    |
    v
NGINX module observes peer identity
    |
    v
shared-memory state compares identity
    |
    v
change is classified
    |
    v
structured event is emitted
    |
    v
C collector persists JSONL
```

### Initial setup

The test:

1. builds the C tools;
2. builds the NGINX module;
3. generates the isolated NGINX config;
4. starts the C collector;
5. starts backend certificate A;
6. starts four NGINX workers;
7. checks NGINX readiness using `/__health`.

The readiness check must not create a history event.

### Phase A — first observation

The backend starts with:

```text
backend-a.cert.pem
backend-a.key.pem
```

The test records the current history count and sends one request through `http://127.0.0.1:18080/`.

Because there is no previous state for this `(upstream, peer)` pair, expected classification is:

```text
first_seen
```

Expected history delta:

```text
+1
```

### Phase B — stable identity

The test sends 20 more requests with backend A unchanged.

Expected behavior:

```text
same upstream
same peer
same certificate
same SPKI
        |
        v
shared state says stable
        |
        v
no new structured event
```

Expected history delta:

```text
0
```

This confirms that ordinary stable traffic does not flood the history stream.

### Phase C — renewed certificate with the same public key

The test restarts the backend using:

```text
backend-a-renewed.cert.pem
backend-a.key.pem
```

The certificate changed, but the key pair remains the same:

```text
previous certificate SHA != current certificate SHA
previous SPKI SHA        == current SPKI SHA
```

Expected classification:

```text
certificate_changed_same_key
```

Expected history delta:

```text
+1
```

### Phase D — new certificate and new public key

The test restarts the backend using:

```text
backend-b.cert.pem
backend-b.key.pem
```

Now both certificate and SPKI differ.

Expected classification:

```text
public_key_changed
```

Expected history delta:

```text
+1
```

### What it validates

- Real NGINX module loading.
- Real TLS upstream observation.
- Selected peer address extraction.
- Certificate SHA-256 calculation.
- SPKI SHA-256 calculation.
- Shared worker state.
- Stable-event suppression.
- `first_seen`.
- `certificate_changed_same_key`.
- `public_key_changed`.
- Structured C event export.
- C collector persistence.

### Pass condition

```text
PASS: first A observation generates one event
PASS: found change=first_seen
PASS: stable identity produces no event
PASS: same-key certificate rotation produces exactly one event
PASS: found change=certificate_changed_same_key
PASS: public-key rotation produces exactly one event
PASS: found change=public_key_changed
PASS: identity sequence
```

## 5. `collector-failure`

Run:

```bash
./test-nginx-identity-v01.sh collector-failure
```

### Purpose

This verifies a core safety property: **durable-history export must never become a dependency for proxy availability**.

### Sequence

The test starts backend A, collector and NGINX, then sends one request to establish the initial identity state. It stores the current history line count, stops the collector, switches backend A to B, and sends 20 requests while the collector is absent.

Expected flow:

```text
NGINX request
    |
    +--> upstream TLS request succeeds
    |
    +--> identity change detected
    |
    +--> sendto(history.sock)
            |
            +--> collector absent
                    |
                    +--> export fails/drops event
                    |
                    +--> request remains successful
```

The durable history file must not change while the collector is absent.

### Recovery phase

The collector is started again on the same socket path. NGINX remains running. Backend is switched again, from B back to A. A new identity transition occurs and future events must be exported again without restarting NGINX.

Expected history delta after recovery:

```text
+1
```

Expected change:

```text
public_key_changed
```

### What it validates

- Collector is not required for request success.
- Missing Unix socket does not crash workers.
- Missing collector does not block workers.
- History events are intentionally best-effort.
- Exporter failure is isolated from proxy operation.
- Collector can restart independently.
- NGINX does not need to restart when collector comes back.
- Future events resume after collector recovery.

### Pass condition

```text
PASS: proxy remains healthy with collector absent
PASS: event export resumes after collector restart
PASS: found change=public_key_changed
PASS: collector failure isolation and recovery
```

## 6. `reload`

Run:

```bash
./test-nginx-identity-v01.sh reload
```

### Purpose

This verifies that the shared-memory identity baseline survives a normal NGINX configuration reload.

Desired semantics:

```text
reload != new runtime identity baseline
```

### Sequence

The test starts backend A, collector and NGINX, sends one request to create the initial state, records history count, runs `nginx -s reload`, and then sends 20 requests with backend A unchanged.

Expected behavior:

```text
Before reload:
A -> first_seen

After reload:
A -> same shared-memory baseline -> stable
```

No new `first_seen` should appear.

Expected history delta:

```text
0
```

### Why this matters

Configuration reloads are routine NGINX operation. If every reload reset the baseline, normal config changes would create false `first_seen` events.

### Pass condition

```text
PASS: reload does not generate a new first_seen
PASS: reload preservation
```

## 7. `restart`

Run:

```bash
./test-nginx-identity-v01.sh restart
```

### Purpose

This verifies the difference between runtime shared state and durable historical evidence.

A full NGINX shutdown destroys the shared-memory baseline, but the external JSONL history must survive.

### Sequence

1. Start backend A, collector and NGINX.
2. Send one request.
3. Verify exactly one initial history event.
4. Fully stop NGINX.
5. Verify JSONL history still exists unchanged.
6. Start NGINX again.
7. Send another request.

### Expected behavior

Before restart:

```text
runtime baseline:
  A known

history:
  first_seen(A)
```

After full stop:

```text
runtime baseline:
  gone

history:
  first_seen(A)   <-- still present
```

After NGINX starts again, the in-memory baseline is empty, so the next observation of A is legitimately `first_seen` again.

Expected history delta:

```text
+1
```

### Why the second `first_seen` is correct

v0.1 does not restore the shared-memory baseline from durable JSONL. The durable history is evidence, not a startup state database.

### Pass condition

```text
PASS: full NGINX restart creates new runtime first_seen
PASS: found change=first_seen
PASS: durable history survives full restart
```

## 8. `concurrency`

Run:

```bash
./test-nginx-identity-v01.sh concurrency
```

### Purpose

This tests correctness of shared state under concurrent requests handled by multiple NGINX workers.

The generated config uses:

```nginx
worker_processes 4;
```

The key invariant is:

> One real identity transition should generate one shared-state change event, not one event per worker.

### Sequence

1. Start backend A.
2. Start collector.
3. Start four-worker NGINX.
4. Send one request to establish A as baseline.
5. Switch backend to B.
6. Record history count.
7. Send 100 requests with concurrency 32.

Conceptually:

```text
                   +--> worker 1
                   |
100 requests ------+--> worker 2
                   |
                   +--> worker 3
                   |
                   +--> worker 4
```

All workers may observe backend B around the same time.

Correct shared-state behavior should be:

```text
worker X:
    sees A
    updates shared state to B
    emits public_key_changed

other workers:
    see B already current
    emit nothing
```

Expected history delta:

```text
+1
```

Expected change:

```text
public_key_changed
```

### Crash detection

The test searches NGINX error logs for patterns such as:

```text
signal 11
segfault
worker process ... exited on signal
```

Any such match fails the test.

### What it validates

- Shared-memory locking.
- State consistency across four workers.
- Duplicate-event suppression under concurrency.
- Correctness during simultaneous identity transition.
- Worker stability under concurrent traffic.
- Absence of obvious crash behavior.

### What it does not prove

This is a concurrency regression test, not a complete stress test. It does not replace long-duration load testing, sanitizer runs, high connection-count benchmarks, memory-pressure tests, or race-oriented stress campaigns.

### Pass condition

```text
PASS: concurrent transition produces exactly one shared-state event
PASS: found change=public_key_changed
PASS: four-worker concurrency
```

## 9. `all`

Run:

```bash
./test-nginx-identity-v01.sh all
```

### Purpose

Runs all scenarios sequentially:

```text
transport
    |
    v
identity
    |
    v
collector-failure
    |
    v
reload
    |
    v
restart
    |
    v
concurrency
```

### Expected final result

```text
============================================================
ALL V0.1 LOCAL TESTS PASSED
============================================================
```

Use `all` only after the individual tests have been debugged and proven to pass independently.

## 10. What each test proves at a glance

| Test | Main thing being validated |
|---|---|
| `transport` | Pure C Unix datagram sender -> collector -> JSONL path |
| `identity` | End-to-end TLS identity observation and change classification |
| `collector-failure` | Collector failure cannot break proxy traffic; export recovers later |
| `reload` | Shared identity baseline survives NGINX config reload |
| `restart` | Durable history survives full NGINX restart while runtime baseline resets |
| `concurrency` | Four workers produce one event for one identity transition |
| `all` | Complete v0.1 regression sequence |

## 11. Expected identity transition model

```text
No previous identity
        |
        v
first_seen
```

```text
certificate changed
public key unchanged
        |
        v
certificate_changed_same_key
```

```text
public key changed
        |
        v
public_key_changed
```

Stable identity:

```text
certificate unchanged
SPKI unchanged
        |
        v
no event
```

## 12. Failure philosophy of v0.1

The v0.1 design deliberately prioritizes NGINX availability over guaranteed history delivery.

Therefore:

- collector unavailable -> event may be lost;
- Unix socket queue full -> event may be dropped;
- history file unavailable -> collector may fail;
- NGINX requests must continue whenever the upstream itself is healthy.

This is why `collector-failure` is a first-class test rather than an edge case.

## 13. Important operational note when testing module rebuilds

Never replace the dynamic module `.so` underneath a live NGINX master.

For module binary changes:

```text
build new .so
    |
    v
fully stop old NGINX master
    |
    v
replace .so
    |
    v
start NGINX
```

Use `nginx -s reload` only when the loaded module binary itself has not changed.

The automated harness avoids this problem by creating a fresh isolated NGINX runtime for the relevant test scenarios.

## 14. Current v0.1 scope

These tests cover the first usable v0.1 functionality:

- actual selected upstream peer observation;
- TLS certificate identity extraction;
- certificate SHA-256;
- SPKI SHA-256;
- shared-memory longitudinal state;
- three change classifications;
- structured C event export;
- C Unix-domain datagram collector;
- durable JSONL evidence;
- reload semantics;
- restart semantics;
- collector failure isolation;
- basic multi-worker concurrency.

Future test work should add:

- high-load latency and throughput comparison with monitoring off/on;
- collector socket queue saturation;
- malformed datagram tests;
- history disk-full behavior;
- shared-memory exhaustion;
- multiple upstream blocks;
- multiple peers per upstream;
- IPv6;
- upstream connection failures;
- TLS session reuse/resumption cases;
- ASan/UBSan regression runs;
- compatibility testing across multiple NGINX versions.
