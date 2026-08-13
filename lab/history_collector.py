#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path

MARKER = "upstream_identity_change "


def parse_fields(text):
    fields = {}
    for token in text.split('" '):
        if '="' not in token:
            continue
        key, value = token.split('="', 1)
        key = key.strip().split()[-1]
        fields[key] = value.rstrip('"')
    return fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("history_path")
    args = parser.parse_args()

    history_path = Path(args.history_path)
    history_path.parent.mkdir(parents=True, exist_ok=True)

    with history_path.open("a", encoding="utf-8", buffering=1) as history:
        for line in sys.stdin:
            pos = line.find(MARKER)
            if pos < 0:
                continue
            fields = parse_fields(line[pos + len(MARKER):])
            if not all(name in fields for name in ("change", "upstream", "peer")):
                continue
            event = {
                "schema_version": 1,
                "source": "nginx_error_log",
                "nginx_timestamp": line[:19],
            }
            event.update(fields)
            history.write(json.dumps(event, separators=(",", ":"), sort_keys=True) + "\n")
            history.flush()


if __name__ == "__main__":
    main()
