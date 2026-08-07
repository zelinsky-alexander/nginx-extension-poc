# Design notes

## Purpose

This repository is a feasibility probe, not yet a production security product.

The central question is:

> Can an external dynamic NGINX module, built against unmodified NGINX, reliably observe the selected HTTPS upstream peer and its TLS identity on real proxied traffic?

If the answer is yes, longitudinal state can be added later in a separate agent without putting database work into NGINX workers.

## Current observation point

The PoC installs an HTTP header filter and inspects:

```text
ngx_http_request_t
    -> upstream
        -> peer.connection
            -> sockaddr
            -> ssl
                -> connection (OpenSSL SSL*)
```

This is intentionally the easiest first experiment.

It is not assumed to be the final production hook.

## Why a header filter first?

Advantages:

- normal third-party HTTP module mechanism;
- no NGINX core patch;
- executes after a successful upstream response has begun;
- simple to test with `curl` and a local TLS backend.

Unknowns that the PoC must answer:

- whether the upstream connection remains available for all relevant request paths;
- behavior with upstream keepalive;
- behavior with retries and failover;
- whether observation should move to connection establishment for correctness and lower overhead.

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

For the PoC, issuer and SAN strings are copied into fixed-size buffers and non-printable/log-sensitive characters are replaced before logging. Fingerprints are represented as fixed-size hexadecimal SHA-256 values.

The module does not log credentials, cookies, request bodies, or application response bodies.

The production design should avoid doing expensive X.509 parsing once per HTTP request. The likely target is either:

```text
one observation per newly established upstream TLS connection
```

or a very cheap per-worker/per-connection deduplication scheme.

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

The important semantic distinction is not only `changed`, but:

```text
new state
known state
known state returned
new component inside otherwise-known identity
correlated multi-component transition
identity churn
```

## Certificate rotation caution

A new certificate or SPKI is not automatically malicious. Short-lived identity systems and normal key rotation can legitimately change cryptographic material.

A future monitor should therefore report observed evidence and correlated change rather than claim compromise solely from one changed fingerprint.

## Performance direction

If the PoC succeeds, performance testing should compare at least:

```text
unmodified NGINX baseline
module loaded but monitor disabled
monitor enabled, stable upstream
monitor enabled, high identity-change rate
```

Important measurements include throughput, p99/p99.9 latency, worker CPU, memory, contention, event drops, and behavior when the external agent is unavailable.

A production monitor must fail open with respect to telemetry: loss of the monitoring agent must not stop proxy traffic.

## Implementation provenance

The C implementation here is an original PoC written for this repository using NGINX and OpenSSL APIs and conventional NGINX module patterns. No source from third-party NGINX modules was intentionally copied into it.

Before a public/production release, manually review:

- NGINX version/API compatibility;
- OpenSSL compatibility;
- compiler warnings and sanitizers;
- licensing/attribution;
- source similarity;
- security boundaries and log-safety;
- load/performance behavior.
