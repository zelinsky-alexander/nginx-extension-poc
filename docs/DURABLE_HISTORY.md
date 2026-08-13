# Durable History

## Goal

Make upstream identity-change evidence survive a full NGINX stop/start without adding new blocking storage work to request callbacks.

The current shared-memory state remains the hot runtime baseline. Durable history is a separate concern.

## Phase 1: externalize the already validated change log

The monitor already emits one structured `upstream_identity_change` log record only when the shared state classifies a real transition. The safest first durable-history slice is therefore outside the module:

```text
NGINX worker
  -> shared-memory classification
  -> existing upstream_identity_change log record
  -> external collector
  -> append-only JSONL history
```

This deliberately avoids changing the NGINX worker hot path while we establish the durable schema and test restart/failure semantics. A direct nonblocking event transport can be evaluated later if log-based export proves insufficient.

## Collector

`lab/history_collector.py` reads NGINX log lines from standard input, ignores unrelated lines, extracts `upstream_identity_change` fields, and appends one normalized JSON object per change to a JSONL file.

Example live collection:

```bash
LOG="$HOME/nginx-upstream-identity-poc/logs/error.log"
HISTORY="$HOME/nginx-upstream-identity-poc/history/upstream-identity.jsonl"

mkdir -p "$(dirname "$HISTORY")"
tail -n 0 -F "$LOG" | \
  python3 ~/source/nginx-extension-poc/lab/history_collector.py "$HISTORY"
```

Example backfill from an existing log:

```bash
grep 'upstream_identity_change' "$LOG" | \
  python3 ~/source/nginx-extension-poc/lab/history_collector.py "$HISTORY"
```

The collector uses only the Python 3 standard library. Python 3 is PSF-licensed, actively maintained, used only for the lab/external collector, and is not linked into the NGINX module. There is no material copyleft concern for the module from this dependency. Normal security, licensing, and similarity review remains appropriate before publication or production use.

## Durable event schema

Each JSONL object contains:

- `schema_version`
- `source`
- `nginx_timestamp`
- `change`
- `upstream`
- `peer`
- `cert_sha256` / `spki_sha256` for `first_seen`
- `previous_cert_sha256` / `current_cert_sha256` for a change
- `previous_spki_sha256` / `current_spki_sha256` for a change

The collector preserves evidence semantics. `public_key_changed` means only that the observed SPKI hash changed for the same `(upstream, peer)` runtime state key. It does not claim compromise or assign operational cause.

## Durability boundary

- **Hot baseline**: NGINX shared memory; shared by workers and preserved across reload, but reset by a full stop/start.
- **Durable history**: append-only external JSONL; survives NGINX stop/start independently of the shared-memory lifetime.

Phase 1 does not restore the hot baseline from durable history. Therefore the first successful TLS observation after a full restart still emits `first_seen`. The durable history can show that the same identity was observed in a previous NGINX process lifetime.

Baseline restoration is deliberately separate because it changes classification semantics and needs explicit policy for stale history, retired peers, corrupted history, and startup failure handling.

## Failure semantics

- collector absent: NGINX proxying and identity classification continue unchanged
- collector stops: change events remain in the normal NGINX error log while that log is retained
- collector restart: live collection can resume; retained logs can be replayed for a missed interval
- JSONL write failure affects the collector, not NGINX request processing

Phase 1 is not an exactly-once pipeline. Replaying overlapping log ranges can create duplicate JSONL records. A later cursor/event-id design should address deduplication before this is treated as a production persistence layer.

## Validation plan

1. Start the collector against a fresh JSONL history file.
2. Observe A and verify one durable `first_seen` object.
3. Send stable A requests and verify no extra change object.
4. Rotate to a renewed certificate with the same key and verify one `certificate_changed_same_key` object.
5. Rotate to B and verify one `public_key_changed` object.
6. Fully stop NGINX and start it again.
7. Verify the earlier JSONL records still exist.
8. Verify the new NGINX process emits a new `first_seen` and the collector appends it.
9. Stop the collector, trigger another identity change, and verify NGINX remains healthy and the change is present in `error.log`.
10. Replay that missed log range into the collector and verify it is persisted.

## Next step after Phase 1

Once the durable schema and recovery workflow are validated, decide between:

- a checkpointed log follower with deduplication, or
- a direct bounded/nonblocking event-export boundary from the module.

Do not add synchronous SQLite or filesystem writes to request callbacks.

## Non-goals for Phase 1

Guaranteed delivery, exactly-once persistence, automatic baseline restoration, remote collectors, DNS/peer-set drift, and history signing are deferred.
