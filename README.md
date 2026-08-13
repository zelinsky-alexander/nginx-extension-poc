# NGINX Upstream Identity Monitor PoC

A proof of concept for testing whether an **unpatched NGINX build** can load a third-party dynamic module that observes the actual HTTPS upstream selected for a proxied request, reads its live TLS identity, and detects identity changes over time.

The original peer-lifecycle feasibility work is now extended with a worker-shared longitudinal baseline. The module still does not block traffic or assign trust scores.

## Current capability

For successful HTTPS `proxy_pass` traffic, the module observes:

- actual selected upstream socket address;
- TLS protocol version;
- selected ALPN protocol;
- leaf certificate SHA-256 fingerprint;
- leaf SPKI SHA-256 fingerprint;
- certificate issuer;
- DNS SAN values.

When a shared state zone is configured, successful **fresh** TLS connections are compared with the previous baseline for the same `(upstream name, peer address)` pair. The module emits evidence-oriented change events for:

- `first_seen`;
- `certificate_changed_same_key`;
- `public_key_changed`.

Reused keepalive sessions remain observable but do not rewrite the longitudinal baseline.

## Configuration

```nginx
http {
    upstream_identity_state_zone upstream_identity_state 1m;

    upstream backend_tls {
        server 127.0.0.1:9443;
        keepalive 16;
        upstream_identity_peer_monitor;
    }

    server {
        listen 8080;

        location / {
            proxy_ssl_verify off;
            proxy_ssl_server_name on;
            proxy_ssl_name backend.test;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_pass https://backend_tls;
        }
    }
}
```

The state zone is shared across workers and is reused across NGINX reloads when its name and size stay unchanged. It is not yet durable across a full NGINX stop/start.

See [docs/LONGITUDINAL_STATE.md](docs/LONGITUDINAL_STATE.md) for the state model, classifications, concurrency design, limitations, and next tests.

## Repository layout

```text
.
├── config
├── src/
│   ├── ngx_http_upstream_identity_module.c
│   ├── ngx_http_upstream_identity_state.c
│   └── ngx_http_upstream_identity_state.h
├── lab/
│   └── nginx.conf
├── scripts/
│   └── generate-test-certs.sh
└── docs/
    ├── LAB_SETUP.md
    ├── TESTS.md
    ├── DESIGN_NOTES.md
    └── LONGITUDINAL_STATE.md
```

## Fast path

On Ubuntu/WSL, follow [docs/LAB_SETUP.md](docs/LAB_SETUP.md). The lab builds a local NGINX from the official NGINX source tarball with this repository added via `--add-dynamic-module`. NGINX itself is not patched.

The topology is:

```text
curl :8080
    |
    v
NGINX + ngx_http_upstream_identity_module.so
    |
    | HTTPS proxy_pass
    v
TLS upstream
```

Run the existing peer-lifecycle tests in [docs/TESTS.md](docs/TESTS.md), then the longitudinal tests in [docs/LONGITUDINAL_STATE.md](docs/LONGITUDINAL_STATE.md).

## Scope and safety

The module records public certificate metadata and connection identity only. It does not inspect request bodies, cookies, authorization headers, or application payloads.

The sample lab may set `proxy_ssl_verify off` because generated backend certificates are self-signed. That is a lab convenience, not a production recommendation.

This milestone deliberately avoids synchronous SQLite/file writes from NGINX request workers. Durable history should be added later through a decoupled sink rather than blocking the request-critical path.

## Implementation and licensing notes

The module implementation in this repository is newly written for this PoC and uses NGINX/OpenSSL public headers and conventional module APIs. It should receive normal code, security, compatibility, licence, and similarity review before publication or production use. No claim is made that generated code is legally cleared or similarity-free.

Dependencies:

- **NGINX** — 2-clause BSD licence; reverse proxy, module host, shared-memory/slab/rbtree primitives; actively maintained. Main concern: third-party modules depend on NGINX module ABI/build compatibility.
- **OpenSSL** — Apache License 2.0; TLS and X.509 APIs; actively maintained. Main concern: production deployments must stay current on OpenSSL security fixes and use supported versions.
- **GCC/Clang + GNU make** — build tooling supplied by the Ubuntu environment; actively maintained. Compiler/runtime licensing does not add project source-code dependencies, but release packaging should still receive normal licence review.

No GPL/AGPL/SSPL/source-available source code is incorporated into this implementation.
