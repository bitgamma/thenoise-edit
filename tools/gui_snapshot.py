#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture the real editor window (including panels) on Xvfb or an existing display."""

import argparse
import pathlib
import shutil
import socket
import sys
import tempfile

import wire_drive as wd


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, default=pathlib.Path("build/noise-edit"))
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("out/gui.png"))
    parser.add_argument("--file", type=pathlib.Path, help="optional image or .noise project to open")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--theme", choices=("dark", "light"), default="dark")
    parser.add_argument("--no-start-dialog", action="store_true")
    args = parser.parse_args()

    if not args.bin.is_file():
        parser.error(f"binary not found: {args.bin}; build noise-edit first")
    if args.file is not None and not args.file.is_file():
        parser.error(f"document not found: {args.file}")
    if reason := wd.display_skip():
        print(f"FAIL: {reason}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="noise-gui-") as temp:
        runtime = pathlib.Path(temp) / "runtime"
        runtime.mkdir()
        stderr_path = pathlib.Path(temp) / "app-stderr.log"
        with stderr_path.open("w") as stderr:
            proc = wd.start_app(args, runtime, args.width, args.height, args.file,
                                stderr, start_dialog=not args.no_start_dialog)
            wire = None
            try:
                wire = wd.Wire(wd.wire_port(runtime, proc, 30.0))
                wire.call("ui.theme", {"theme": args.theme})
                if args.file is not None:
                    wd.wait_for_fit(wire)
                wd.settle(wire, frames=10)
                capture = wire.call("screenshot", {"name": "gui"})
                args.output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(capture["path"], args.output)
                print(f"GUI screenshot: {args.output} (sha256 {capture['sha256'][:12]})")
                return 0
            except (wd.WireError, OSError, socket.timeout, TimeoutError) as exc:
                print(f"FAIL: {exc}", file=sys.stderr)
                stderr.flush()
                print(wd.app_stderr_tail(stderr_path), file=sys.stderr)
                return 1
            finally:
                wd.shutdown(proc, wire)


if __name__ == "__main__":
    sys.exit(main())
