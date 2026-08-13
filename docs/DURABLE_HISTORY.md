# Durable History

## Goal

Make upstream identity-change evidence survive a full NGINX stop/start without adding blocking filesystem or database work to request callbacks.

The current shared-memory state remains the hot runtime baseline. Durable history is separate.

## First slice

```text
NGINX worker
  -> shared-memory classification
  -> structured change event
  -> nonblocking Unix datagram
  -> external collector
  -> append-only JSONL history
```

The worker does not open SQLite and does not persist files. Export failure must not block or fail proxy traffic.

Configuration inside `http {}`:

```nginx
upstream_identity_state_zone upstream_identity_state 1m;
upstream_identity_history_socket /tmp/nginx-upstream-identity-history.sock;
```

The history socket is optional; without it, current behavior is unchanged.

Each exported JSON event contains version, timestamp, change type, upstream, peer, previous/current certificate SHA-256, and previous/current SPKI SHA-256. `first_seen` uses empty previous hashes.

The first collector uses only the Python 3 standard library and appends one JSON object per line. Python 3 is PSF-licensed, actively maintained, used only for the lab collector, and is not linked into the NGINX module. Normal security, licensing, and similarity review remains appropriate before publication or production use.

## Durability boundary

- Hot baseline: shared memory; worker-shared and reload-preserving, reset by full stop/start.
- Durable history: external append-only evidence; survives NGINX restart.

This first slice does not restore the hot baseline from history. A first successful TLS observation after a full restart therefore still emits `first_seen`.

## Failure semantics

- collector absent: proxy request still succeeds
- socket full: nonblocking send fails immediately
- collector restart: later events can be exported again
- normal NGINX change logging remains available even when export fails

## Validation

1. A -> one durable `first_seen`.
2. Stable A -> no additional change record.
3. Same-key renewed A -> one durable `certificate_changed_same_key`.
4. B -> one durable `public_key_changed`.
5. Full NGINX restart -> old JSONL records remain.
6. New process -> new `first_seen`, demonstrating separation of history and hot baseline.
7. Collector stopped -> requests remain healthy.
8. Collector restarted -> later change events export again.

## Non-goals

Guaranteed delivery, synchronous SQLite writes in workers, baseline restore from disk, remote collectors, DNS/peer-set drift, and history signing are deferred until this minimal export boundary is validated.
