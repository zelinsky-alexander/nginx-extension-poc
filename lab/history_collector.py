#!/usr/bin/env python3
import argparse
import json
import socket
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("socket_path")
parser.add_argument("history_path")
args = parser.parse_args()

socket_path = Path(args.socket_path)
history_path = Path(args.history_path)
history_path.parent.mkdir(parents=True, exist_ok=True)
socket_path.parent.mkdir(parents=True, exist_ok=True)

if socket_path.exists():
    socket_path.unlink()

sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(str(socket_path))

try:
    with history_path.open("a", encoding="utf-8", buffering=1) as history:
        while True:
            payload = sock.recv(8192)
            event = json.loads(payload.decode("utf-8"))
            history.write(json.dumps(event, separators=(",", ":"), sort_keys=True) + "\n")
            history.flush()
except KeyboardInterrupt:
    pass
finally:
    sock.close()
    if socket_path.exists():
        socket_path.unlink()
