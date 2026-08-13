# Longitudinal Identity Monitor — Functional Validation

Validated on 2026-08-11 against the WSL NGINX 1.28.0 lab on branch `feature/longitudinal-identity-monitor`.

This is an engineering validation record, not a production-readiness claim.

## Validation matrix

| Scenario | Expected behavior | Observed behavior | Result |
|---|---|---|---|
| First successful TLS observation | Emit one `change="first_seen"` event | One `first_seen` event was emitted for `backend_tls` / `127.0.0.1:9443` | PASS |
| Stable certificate A | No additional change event | Repeated requests produced identity observations without another change event | PASS |
| New leaf certificate using the same key | Emit `certificate_changed_same_key` | Leaf certificate changed while SPKI stayed the same | PASS |
| Switch to certificate B with a different key | Emit `public_key_changed` | Certificate and SPKI changed | PASS |
| Concurrent four-worker transition | Emit one shared change event | Shared state produced one transition event across the worker set | PASS |
| Ordinary NGINX reload | Preserve baseline | No new `first_seen` event appeared after reload | PASS |

## Lab identities

Certificate A:

```text
leaf SHA-256: 9bd0d02755e6973c75c125e5d3e348b583a0a2a98449f48fcea4df57261cd17b
SPKI SHA-256: 01fcad43617cac0c3ddefe1218cd1bb8d38c68c14ed7c2f87212d4cc32acd1eb
```

Certificate B:

```text
leaf SHA-256: db0762c45388307ea8f030f776e75c6b08493f886e90afd079c49d9d3b424ed1
SPKI SHA-256: d5a3e1cf86b0cba4e5502fcd76ed1c015a7b74f99f1d403c6aa03600c2a58590
```

## Runtime behavior validated

The current implementation keeps the hot baseline in an NGINX shared-memory zone keyed by upstream name and peer address. Successful fresh TLS connections update longitudinal state. Reused keepalive connections can be observed but do not rewrite the baseline.

The state is shared across workers and survives an ordinary configuration reload when the shared-memory zone name and size remain unchanged. A full NGINX stop/start does not preserve the current in-memory baseline; the next successful observation is expected to become `first_seen`.

## Deployment/debugging lesson

During validation, one run entered a worker `SIGSEGV` restart loop while multiple NGINX masters were present after a newly built dynamic module had been replaced and reloads attempted. After all NGINX processes were stopped and a clean process was started with the new module, the failure did not reproduce. Clean single-process GDB testing and the subsequent normal four-worker run were stable.

The incident is therefore recorded as a deployment/runtime-state hazard rather than a proven source-code root cause. Development builds should not replace a dynamic-module binary while an NGINX process is still using it; use a clean stop, replace, and fresh start. Ordinary reload remains appropriate for testing configuration/shared-memory reload behavior when the module binary itself is unchanged.

## Scope

Validated classifications:

```text
first observation              -> first_seen
same identity                  -> no change event
new certificate, same key      -> certificate_changed_same_key
new public key                 -> public_key_changed
configuration reload           -> baseline preserved
```

Not yet implemented or validated: durable history across full restart, DNS/address-set drift, peer appearance/disappearance history, external event export, or a production persistence backend.

## Next milestone

Build durable, nonblocking longitudinal history while keeping the request/upstream callback path free of synchronous filesystem or SQLite writes. Shared memory should remain the hot runtime baseline, with change events exported through a bounded nonblocking boundary to a persistence or consumer component outside the latency-sensitive worker path.
