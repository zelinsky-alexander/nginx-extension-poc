# PoC tests

Run these tests only after completing [LAB_SETUP.md](LAB_SETUP.md).

The purpose is to answer a small set of feasibility questions before any longitudinal database or agent is built.

## Test 1 — Observe one HTTPS upstream

Backend A should be running on `127.0.0.1:9443` with `backend-a.cert.pem`.

Send:

```bash
curl -sS http://127.0.0.1:8080/ >/dev/null
```

Inspect:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 5
```

Pass if the latest entry contains:

```text
peer="127.0.0.1:9443"
tls="..."
cert_sha256="<64 hex chars>"
spki_sha256="<64 hex chars>"
san_dns="backend.test"
```

An empty `alpn` is acceptable in this lab.

### Independent certificate check

Verify that NGINX's certificate fingerprint matches OpenSSL's fingerprint after removing separators and normalizing case:

```bash
openssl x509 \
  -in lab/certs/backend-a.cert.pem \
  -noout \
  -fingerprint \
  -sha256
```

The value should correspond to `cert_sha256` in the module log.

## Test 2 — Same address, different cryptographic identity

This is the key proof-of-concept test.

1. Stop backend A with `Ctrl+C`.
2. Start backend B at exactly the same address and port:

```bash
cd "$POC_DIR"
openssl s_server \
  -accept 127.0.0.1:9443 \
  -cert lab/certs/backend-b.cert.pem \
  -key lab/certs/backend-b.key.pem \
  -www
```

3. Send another request:

```bash
curl -sS http://127.0.0.1:8080/ >/dev/null
```

4. Compare the last observations:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 2
```

Pass if:

```text
peer is unchanged
cert_sha256 changed
spki_sha256 changed
HTTP request still succeeds
```

This demonstrates that ordinary availability can remain healthy while the upstream's cryptographic identity changes.

## Test 3 — Repeated requests are stable

With one backend identity running:

```bash
for i in $(seq 1 20); do
  curl -sS http://127.0.0.1:8080/ >/dev/null || exit 1
done
```

Inspect the observations:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 20
```

Pass if all entries for the same backend show the same certificate and SPKI fingerprints.

This PoC currently logs once per proxied response. The production design should avoid expensive per-request certificate work and move toward connection-level observation or aggressive deduplication.

## Test 4 — Module disabled

Edit the lab configuration:

```nginx
upstream_identity_monitor off;
```

Reload:

```bash
"$HOME/nginx-upstream-identity-poc/sbin/nginx" \
  -p "$HOME/nginx-upstream-identity-poc/" \
  -s reload
```

Record the current log line count:

```bash
grep -c 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log"
```

Send several requests and repeat the count. Pass if no new identity observations are added while requests still succeed.

Restore `upstream_identity_monitor on;` before continuing.

## Test 5 — Backend unavailable

Stop `openssl s_server` and send:

```bash
curl -i http://127.0.0.1:8080/
```

Expected: NGINX returns an upstream error such as `502 Bad Gateway`.

The module must not crash NGINX. Confirm:

```bash
"$HOME/nginx-upstream-identity-poc/sbin/nginx" \
  -t \
  -p "$HOME/nginx-upstream-identity-poc/"

pgrep -a nginx
```

Pass if the worker/master processes remain healthy.

## Test 6 — Two peers

After the single-peer tests pass, extend `lab/nginx.conf` temporarily:

```nginx
upstream backend_tls {
    server 127.0.0.1:9443;
    server 127.0.0.1:9444;
}
```

Run backend A:

```bash
openssl s_server \
  -accept 127.0.0.1:9443 \
  -cert lab/certs/backend-a.cert.pem \
  -key lab/certs/backend-a.key.pem \
  -www
```

Run backend B in another terminal:

```bash
openssl s_server \
  -accept 127.0.0.1:9444 \
  -cert lab/certs/backend-b.cert.pem \
  -key lab/certs/backend-b.key.pem \
  -www
```

Reload NGINX and send repeated requests:

```bash
for i in $(seq 1 20); do
  curl -sS http://127.0.0.1:8080/ >/dev/null
done
```

Inspect:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 20
```

Pass if observations correctly associate:

```text
127.0.0.1:9443 -> identity A
127.0.0.1:9444 -> identity B
```

The exact selection pattern is not important. Correct peer-to-identity association is.

## Test 7 — Worker concurrency smoke test

Change:

```nginx
worker_processes 4;
```

Reload NGINX and run:

```bash
seq 1 200 | xargs -n1 -P16 -I{} \
  curl -sS http://127.0.0.1:8080/ -o /dev/null
```

Pass if:

- all requests complete apart from expected transient test-lab failures;
- NGINX does not crash;
- log lines remain syntactically intact;
- fingerprints remain associated with the correct peer.

This is not a performance benchmark. It is only a concurrency smoke test.

## Test 8 — Keepalive experiment

This is exploratory because the PoC observes at the response header-filter stage rather than at TLS-connection creation.

Add inside `upstream backend_tls`:

```nginx
keepalive 16;
```

Add inside the proxy location:

```nginx
proxy_http_version 1.1;
proxy_set_header Connection "";
```

Repeat multiple requests and inspect the logs.

Questions to record:

1. Is `r->upstream->peer.connection` always available at the header-filter phase?
2. Does TLS metadata remain accessible on reused upstream connections?
3. Do retries/failover produce observations for only the successful peer or also failed attempts?

These answers determine whether the real implementation can stay on a normal module phase or needs a different upstream lifecycle integration point.

## Test results template

Record results in an issue or temporary notes using:

```text
NGINX version:
OpenSSL version:
Ubuntu version:
WSL/native Linux:

T1 single upstream: PASS/FAIL
T2 certificate replacement: PASS/FAIL
T3 repeated stability: PASS/FAIL
T4 monitor disabled: PASS/FAIL
T5 backend unavailable: PASS/FAIL
T6 two peers: PASS/FAIL
T7 concurrency smoke: PASS/FAIL
T8 keepalive experiment: observations

Unexpected logs/errors:
```

## Stop condition

Do not build SQLite/history/risk scoring yet if any of these occur:

- upstream connection is consistently unavailable at the chosen phase;
- certificate information disappears before the filter executes;
- keepalive causes incorrect peer identity association;
- retries cannot be represented correctly enough for the intended semantics.

Those are integration-design problems and should be solved before adding product features.
