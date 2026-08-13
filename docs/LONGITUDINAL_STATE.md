# Longitudinal Upstream Identity State

This branch moves the PoC from per-request observation toward an actual upstream identity monitor.

## Scope of this milestone

The module now keeps a worker-shared identity baseline for each `(upstream name, selected peer address)` pair and compares newly established TLS connections with the previous baseline.

State is stored in an NGINX shared-memory zone, so all workers in the same NGINX instance see the same baseline. The zone is reused across configuration reloads when its name and size are unchanged. It is intentionally **not durable across a full NGINX stop/start**; durable history is a later milestone and should not be implemented with blocking file I/O in request workers.

Only successful fresh TLS connections update longitudinal state. Reused keepalive connections are still observed, but do not rewrite the baseline. This avoids treating repeated observations of an already-established TLS session as a new identity event.

## Configuration

Add one shared state zone in the `http` block:

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

The zone size must be at least eight NGINX pages. `1m` is intentionally generous for the lab.

## Change events

The existing `upstream_identity` observation log remains unchanged. Longitudinal transitions add a separate structured event:

```text
upstream_identity_change change="first_seen" ...
```

The current classifications are deliberately evidence-oriented:

- `first_seen` — first fresh TLS identity observed for this upstream/peer pair.
- `certificate_changed_same_key` — leaf certificate fingerprint changed while the SPKI fingerprint stayed the same. This is consistent with certificate renewal/rotation using the same public key, but the module does not claim the operational reason.
- `public_key_changed` — SPKI fingerprint changed. The certificate fingerprint may also change. This is a stronger identity transition, but the module does not claim compromise or ownership change.

Stable observations do not emit `upstream_identity_change` events.

## Why state is keyed by upstream plus peer

A single upstream may legitimately contain multiple active peers. Treating the most recently selected address as one global upstream identity would create false drift every time normal load balancing alternated between peers.

This milestone therefore tracks TLS identity independently for each `(upstream, peer)` pair. Peer-set additions/removals and DNS/address-set drift require a separate model and are intentionally deferred.

## Concurrency model

The state store uses an NGINX shared-memory zone, slab allocation, an rbtree index, and the shared slab mutex. The critical section is intentionally small: lookup, compare, and baseline update. Logging happens after the shared-memory lock is released.

The state entry keeps:

- upstream name;
- selected peer address;
- current certificate SHA-256;
- current SPKI SHA-256;
- first/last observation timestamps;
- observation count;
- change count.

The counters are internal in this milestone; no status endpoint is exposed yet.

## Next tests

1. Start certificate A and make one fresh request. Expect `first_seen`.
2. Make repeated requests over keepalive. Expect ordinary observation logs but no new change event.
3. Restart the same peer with a different certificate using the **same key**. Expect `certificate_changed_same_key` on the first fresh connection.
4. Restart the same peer with a certificate using a **different key**. Expect `public_key_changed`.
5. Repeat with four workers and confirm the change event is emitted only once for the shared baseline transition.
6. Reload NGINX without changing the zone name/size and verify the baseline survives the reload.
7. Full stop/start should currently reset the baseline and produce `first_seen` again; this is an explicit limitation, not a persistence guarantee.

## Durability boundary

Do not add SQLite/file writes directly to the peer callbacks. A later durable-history milestone should export bounded change events to a nonblocking or decoupled sink and persist them outside the request-critical path.
