#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""One command to the running app: `python3 tools/wire_call.py server.connect`.

The debug wire is the window's own command table, so this is not a side door: it is the same
call a button makes. The port comes from `<runtime>/wire-port`, which the app writes at start.
"""
import argparse
import json
import os
import pathlib
import socket
import sys


def runtime_dir() -> pathlib.Path:
    if os.environ.get("NOISE_RUNTIME_DIR"):
        return pathlib.Path(os.environ["NOISE_RUNTIME_DIR"])
    base = os.environ.get("XDG_RUNTIME_DIR") or pathlib.Path.home() / ".cache"
    return pathlib.Path(base) / "noise-edit"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command")
    parser.add_argument("args", nargs="*", help="key=value; a value is parsed as JSON when it can be")
    parser.add_argument("--port", type=int, help="wire port (default: read <runtime>/wire-port)")
    parser.add_argument("--runtime", type=pathlib.Path, help="the app's runtime directory")
    opts = parser.parse_args()

    port = opts.port
    if port is None:
        endpoint = (opts.runtime or runtime_dir()) / "wire-port"
        try:
            port = int(endpoint.read_text().strip())
        except OSError:
            print(f"no {endpoint}: is noise-edit running with the debug wire on?", file=sys.stderr)
            return 2

    args = {}
    for item in opts.args:
        key, _, raw = item.partition("=")
        try:
            args[key] = json.loads(raw)
        except json.JSONDecodeError:
            args[key] = raw

    with socket.create_connection(("127.0.0.1", port)) as sock:
        stream = sock.makefile("rwb")
        stream.write((json.dumps({"id": 1, "cmd": opts.command, "args": args}) + "\n").encode())
        stream.flush()
        reply = json.loads(stream.readline())
    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
