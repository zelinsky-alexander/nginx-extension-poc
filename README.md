# NGINX Upstream Identity Monitor PoC

A minimal proof of concept for testing whether an **unpatched NGINX build** can load a third-party dynamic module that observes the actual HTTPS upstream selected for a proxied request and reads its TLS identity.

This repository intentionally does **not** implement longitudinal storage, scoring, blocking, ASN enrichment, or an external agent. The first goal is only to prove the NGINX integration point.

## Acceptance criterion

For a successful HTTPS `proxy_pass`, the module should emit a log entry containing as much of the following as the stock NGINX request/upstream structures expose at the header-filter phase:

- actual upstream socket address;
- TLS protocol version;
- selected ALPN protocol;
- leaf certificate SHA-256 fingerprint;
- leaf SPKI SHA-256 fingerprint;
- certificate issuer;
- DNS SAN values.

The critical test is to restart the same backend address with a different certificate and observe that the module reports a different certificate/SPKI identity while ordinary HTTP traffic continues to succeed.

## Repository layout

```text
.
├── config                         NGINX dynamic-module build descriptor
├── src/
│   └── ngx_http_upstream_identity_module.c
├── lab/
│   └── nginx.conf                 minimal local reverse-proxy config
├── scripts/
│   └── generate-test-certs.sh     creates two self-signed backend identities
└── docs/
    ├── LAB_SETUP.md               exact WSL/Ubuntu build and run procedure
    ├── TESTS.md                   functional tests and expected observations
    └── DESIGN_NOTES.md            scope, limitations, and next decisions
```

## Fast path

On Ubuntu/WSL, follow [docs/LAB_SETUP.md](docs/LAB_SETUP.md). The lab builds a local NGINX from the official NGINX source tarball with this repository added via `--add-dynamic-module`. NGINX itself is not patched.

The resulting topology is:

```text
curl :8080
    |
    v
NGINX + ngx_http_upstream_identity_module.so
    |
    | HTTPS proxy_pass
    v
openssl s_server :9443
```

Run the tests in [docs/TESTS.md](docs/TESTS.md) after the first request succeeds.

## Scope and safety

This PoC logs public certificate metadata and connection identity only. It does not inspect request bodies, cookies, authorization headers, or application payloads.

The sample lab sets `proxy_ssl_verify off` only because the generated backend certificates are self-signed. That is a lab convenience, not a production recommendation.

## Implementation and licensing notes

The module implementation in this repository is newly written for this PoC and uses NGINX/OpenSSL public headers and conventional module APIs. It should receive normal code, security, compatibility, and licence review before publication or production use. No claim is made that generated code is legally cleared or similarity-free.

The PoC depends on:

- **NGINX** — 2-clause BSD licence; reverse proxy and module host; actively maintained.
- **OpenSSL** — Apache License 2.0; TLS and X.509 APIs; actively maintained.
- **GCC/Clang + GNU make** — build tooling supplied by the Ubuntu environment.

No GPL/AGPL/SSPL/source-available source code is incorporated into this implementation.
