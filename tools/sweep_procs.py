#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Kill every `noise-edit` and `Xvfb` this user owns, and say what was killed.

A probe that is killed by a test timeout, or an ad-hoc script that dies between starting the app and
cleaning up, leaves an editor holding a framebuffer and an X server running against it. They idle at
full CPU redraw on a shared box, and nothing about a finished test run shows it, so the sweep is a
command rather than a habit.

    python3 tools/sweep_procs.py            # terminate strays, exit 1 if there were any
    python3 tools/sweep_procs.py --list     # look only

Run it when nothing else is probing: it matches by program name, so a deliberate interactive session is
a target too.
"""
from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from pathlib import Path

# `Xvfb`, `xvfb-run`, and the editor; matched on the executable name, not the command line, so a script
# named after a probe is never mistaken for the thing it started.
MATCHES = {"noise-edit", "Xvfb", "xvfb-run"}
TERM_GRACE_S = 3.0


def strays() -> list[tuple[int, str]]:
    found: list[tuple[int, str]] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid == os.getpid():
            continue
        try:
            comm = (entry / "comm").read_text().strip()
            if comm not in MATCHES:
                continue
            if os.stat(entry).st_uid != os.getuid():  # /proc/<pid> is owned by the process's uid
                continue
            command = (entry / "cmdline").read_bytes().decode(errors="replace").replace("\0", " ").strip()
        except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError, ValueError):
            continue  # it exited or belongs to a thread we cannot read: not ours to kill
        found.append((pid, command or comm))
    return sorted(found)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true", help="report without killing")
    args = parser.parse_args()

    found = strays()
    if not found:
        print("clean: no noise-edit or Xvfb running")
        return 0
    for pid, command in found:
        print(f"stray {pid}: {command}")
    if args.list:
        return 1

    for pid, _ in found:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + TERM_GRACE_S
    while time.monotonic() < deadline:
        if not strays():
            break
        time.sleep(0.1)
    for pid, command in strays():
        try:
            os.kill(pid, signal.SIGKILL)
            print(f"SIGKILL {pid}: {command}")
        except ProcessLookupError:
            pass
    print(f"killed {len(found)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
