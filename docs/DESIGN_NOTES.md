# Design notes

## Purpose

This repository is a feasibility probe, not yet a production security product.

The central question is:

> Can an external dynamic NGINX module, built against unmodified NGINX, reliably observe the selected HTTPS upstream peer and its TLS identity on real proxied traffic?

If the answer is yes, longitudinal state can be added later in a separate agent without putting database work into NGINX workers.

## Result from the first observation point

The original PoC installed an HTTP header filter and attempted to inspect:

```text
ngx_http_request_t
    -> upstream
        -> peer.connection
            -> ssl
```

A real WSL/Ubuntu test with NGINX 1.28.0 successfully proxied an HTTPS request but produced:

```text
upstream_identity unavailable reason=peer_connection_missing
```

That result is useful: the downstream response-header filter is too late to rely on the live upstream connection.

## Current experiment: upstream peer lifecycle

The `experiment/upstream-peer-free-hook` branch adds an upstream-context directive:

```nginx
upstream backend_tls {
    server 127.0.0.1:9443;
    upstream_identity_peer_monitor;
}
```

The module chains the documented NGINX upstream peer callbacks. It preserves the existing peer implementation and wraps its per-request `get`, `free`, notification, and TLS session callbacks.

The important observation point is immediately before delegating to the original `free` callback:

```text
peer selected
    -> connect
    -> TLS handshake
    -> proxy request/response
    -> module peer_free wrapper
         -> inspect live ngx_connection_t / SSL*
         -> original peer_free callback
```

If a keepalive layer returns an already-open connection from `get`, the experiment also records a `peer_get_reused` observation.

This follows the public upstream callback model documented by NGINX. It does not patch NGINX core.

## Directive ordering and keepalive

Upstream modules can chain their peer callbacks. For this experiment, if `keepalive` is enabled, configure the identity directive after it:

```nginx
upstream backend_tls {
    server 127.0.0.1:9443;
    keepalive 16;
    upstream_identity_peer_monitor;
}
```

That ordering makes the identity wrapper the outer layer, so it can inspect `pc->connection` before the keepalive module moves the connection into its cache and clears the peer's connection pointer.

This ordering requirement is an experimental limitation, not yet a production interface guarantee.

## Legacy header-filter probe

The original location directive remains available for comparison:

```nginx
upstream_identity_monitor on;
```

It still probes the response-header stage and is expected to report `peer_connection_missing` in the tested NGINX 1.28.0 path. The lab configuration now uses the peer-lifecycle directive instead.

## Deliberate non-goals

The PoC does not implement:

- SQLite or another durable database;
- an external trust/identity agent;
- shared-memory history;
- first-seen / last-seen tracking;
- novelty classification;
- certificate rotation policy;
- ASN/network-owner enrichment;
- Prometheus/OpenTelemetry export;
- request blocking;
- HTTP response-body fingerprinting;
- request/header/body capture.

## Security boundaries

The module treats certificate strings as untrusted input.

Issuer and SAN strings are copied into bounded buffers and log-sensitive/non-printable characters are replaced before logging. Fingerprints are represented as fixed-size hexadecimal SHA-256 values.

The module does not log credentials, cookies, request bodies, or application response bodies.

The production design should avoid doing expensive X.509 parsing once per HTTP request. The likely target is either one observation per newly established upstream TLS connection or a cheap connection-level deduplication mechanism.

## Expected production architecture if feasibility is proven

```text
NGINX worker
    |
    | compact observation / change signal
    v
bounded non-blocking transport
    |
    v
local identity agent
    |
    +-- canonicalization
    +-- durable history
    +-- first/last seen
    +-- novelty detection
    +-- correlation
    +-- export/alerts
```

NGINX should not synchronously write SQLite, perform ASN lookups, or block on a monitoring consumer.

## Candidate longitudinal identity

A later version may model an observed upstream state using:

```text
routing identity
    selected peer address
    resolved address/network metadata

cryptographic identity
    leaf certificate SHA-256
    SPKI SHA-256
    issuer identity
    SAN set
    verification result

transport/protocol identity
    TLS version
    ALPN
    possibly HTTP upstream protocol
```

A new certificate or SPKI is not automatically malicious. Normal certificate and key rotation are expected, so a future monitor should record correlated change rather than label a single changed fingerprint as compromise.

## Next acceptance decision

The peer-lifecycle experiment succeeds if a normal HTTPS proxy request produces a `stage="peer_free"` log containing the actual peer, TLS version, certificate fingerprint and SPKI fingerprint, and if replacing the backend certificate changes those fingerprints without breaking proxy traffic.

After that, test keepalive reuse and multi-peer failover before adding longitudinal state.

## Implementation provenance

The module code is newly written for this PoC using NGINX/OpenSSL public headers and documented upstream callback concepts. The callback-chaining design follows the NGINX development guide and the API model used by official upstream modules; no third-party module source is incorporated.

Before a public/production release, manually review NGINX/OpenSSL compatibility, compiler warnings, sanitizers, licensing/attribution, source similarity, log safety, and load behavior.
