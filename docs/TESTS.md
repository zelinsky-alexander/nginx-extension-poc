# PoC tests

Run these tests after completing [LAB_SETUP.md](LAB_SETUP.md).

The current experiment uses the upstream peer lifecycle rather than the downstream response header filter. The expected successful observation stage is:

```text
stage="peer_free"
```

## Test 1 — Observe one HTTPS upstream

Start backend A on `127.0.0.1:9443`:

```bash
cd "$POC_DIR"
openssl s_server \
  -accept 127.0.0.1:9443 \
  -cert lab/certs/backend-a.cert.pem \
  -key lab/certs/backend-a.key.pem \
  -www
```

In another terminal send:

```bash
curl -sS http://127.0.0.1:8080/ >/dev/null
```

Inspect:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 5
```

Pass if the latest successful observation contains:

```text
stage="peer_free"
peer="127.0.0.1:9443"
tls="..."
cert_sha256="<64 hex chars>"
spki_sha256="<64 hex chars>"
san_dns="backend.test"
```

An empty `alpn` is acceptable in this lab.

Verify the leaf certificate independently:

```bash
openssl x509 \
  -in lab/certs/backend-a.cert.pem \
  -noout \
  -fingerprint \
  -sha256
```

After removing separators and normalizing case, this must match `cert_sha256` from the module log.

## Test 2 — Same address, different TLS identity

Stop backend A and start backend B at the same address:

```bash
openssl s_server \
  -accept 127.0.0.1:9443 \
  -cert lab/certs/backend-b.cert.pem \
  -key lab/certs/backend-b.key.pem \
  -www
```

Send another request:

```bash
curl -sS http://127.0.0.1:8080/ >/dev/null
```

Compare observations:

```bash
grep 'upstream_identity.*stage="peer_free"' \
  "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 2
```

Pass if the peer remains `127.0.0.1:9443`, while both certificate and SPKI fingerprints change and HTTP still succeeds.

## Test 3 — Repeated stable requests

With one backend identity running:

```bash
for i in $(seq 1 20); do
  curl -sS http://127.0.0.1:8080/ >/dev/null || exit 1
done
```

Inspect:

```bash
grep 'upstream_identity.*stage="peer_free"' \
  "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 20
```

Pass if every observation for the same backend has the same certificate and SPKI fingerprints.

## Test 4 — Backend unavailable

Stop the backend and send:

```bash
curl -i http://127.0.0.1:8080/
```

Expected: NGINX returns an upstream error such as `502 Bad Gateway`. The module may report an unavailable peer lifecycle observation, but NGINX must not crash.

Confirm:

```bash
"$HOME/nginx-upstream-identity-poc/sbin/nginx" \
  -t \
  -p "$HOME/nginx-upstream-identity-poc/"

pgrep -a nginx
```

## Test 5 — Two peers

Configure:

```nginx
upstream backend_tls {
    server 127.0.0.1:9443;
    server 127.0.0.1:9444;
    upstream_identity_peer_monitor;
}
```

Run backend A on `9443` and backend B on `9444`, reload NGINX, then:

```bash
for i in $(seq 1 20); do
  curl -sS http://127.0.0.1:8080/ >/dev/null
done
```

Pass if observations correctly associate:

```text
127.0.0.1:9443 -> identity A
127.0.0.1:9444 -> identity B
```

## Test 6 — Keepalive reuse

Configure the upstream with the identity directive after `keepalive`:

```nginx
upstream backend_tls {
    server 127.0.0.1:9443;
    keepalive 16;
    upstream_identity_peer_monitor;
}
```

And in the proxy location:

```nginx
proxy_http_version 1.1;
proxy_set_header Connection "";
```

Reload and send repeated requests:

```bash
for i in $(seq 1 20); do
  curl -sS http://127.0.0.1:8080/ >/dev/null || exit 1
done
```

Inspect:

```bash
grep 'upstream_identity' "$HOME/nginx-upstream-identity-poc/logs/error.log" | tail -n 40
```

Expected experimental behavior:

- `stage="peer_free"` sees a live TLS connection before it is cached;
- a reused keepalive connection may also produce `stage="peer_get_reused"`;
- fingerprints remain associated with the correct peer.

If `peer_free` becomes unavailable only when keepalive is enabled, verify that `upstream_identity_peer_monitor;` is placed after `keepalive 16;` in the upstream block.

## Test 7 — Worker concurrency smoke test

Set:

```nginx
worker_processes 4;
```

Reload and run:

```bash
seq 1 200 | xargs -n1 -P16 -I{} \
  curl -sS http://127.0.0.1:8080/ -o /dev/null
```

Pass if NGINX remains healthy, log records remain intact, and identities stay associated with the correct peer.

## Legacy header-filter comparison

The old location directive remains available:

```nginx
upstream_identity_monitor on;
```

On the tested NGINX 1.28.0 WSL path it produced:

```text
stage="header_filter" reason=peer_connection_missing
```

That result motivated the peer-lifecycle experiment. It is not the expected observation mechanism for the current lab.

## Test results template

```text
NGINX version:
OpenSSL version:
Ubuntu version:
WSL/native Linux:

T1 peer_free single upstream: PASS/FAIL
T2 certificate replacement: PASS/FAIL
T3 repeated stability: PASS/FAIL
T4 backend unavailable: PASS/FAIL
T5 two peers: PASS/FAIL
T6 keepalive: PASS/FAIL/OBSERVATIONS
T7 concurrency smoke: PASS/FAIL

Unexpected logs/errors:
```

## Stop condition

Do not add longitudinal storage or scoring until the peer-lifecycle hook proves correct for normal requests, keepalive reuse, and failover. If the live TLS connection is still unavailable at `peer_free`, the next experiment should move closer to upstream connection establishment rather than adding product features.
