#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Docked AI Workspace is command-driven, closeable, reopenable and persistent."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

WIDTH, HEIGHT = 1280, 800


def require(value: bool, detail: str) -> None:
    if not value:
        raise AssertionError(detail)
    print(f"ok: {detail}")


def open_app(binary: pathlib.Path, runtime: pathlib.Path, stderr):
    args = argparse.Namespace(bin=binary, theme="dark")
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, stderr,
                        {"THENOISE_URL": None, "THENOISE_API_KEY": None},
                        start_dialog=False)
    return proc, wd.Wire(wd.wire_port(runtime, proc, 60.0))


def run(binary: pathlib.Path, runtime: pathlib.Path, stderr_path: pathlib.Path) -> None:
    proc = wire = None
    try:
        with stderr_path.open("ab") as stderr:
            proc, wire = open_app(binary, runtime, stderr)
            wd.settle(wire, 12)
            initial = wire.call("app.state")
            ai = initial["ai_workspace"]
            require(ai["open"] and ai["docked"], "AI Workspace starts in the dock")
            require(ai["window"][2] >= 280, "docked workspace has useful width")
            require(initial["layers_window"][2] >= 280 and not ai["visible"],
                    "Layers is the visible default tab; AI Workspace stays docked")
            response = wire.call("ui.ai_workspace", {"open": True, "focus": True})
            require(response["open"], "open/focus command succeeds")
            wd.settle(wire, 8)
            focused = wire.call("app.state")["ai_workspace"]
            require(focused["visible"], "focus selects the AI Workspace tab")
            require(focused["docked"], "focusing does not float the workspace")
            require(wire.call("app.state")["layers_window"][2] == 0,
                    "the inactive Layers tab is not rendered over the AI workspace")
            wire.call("ui.ai_workspace", {"open": False})
            wd.settle(wire, 8)
            hidden = wire.call("app.state")["ai_workspace"]
            require(not hidden["open"] and not hidden["visible"],
                    "closing hides the workspace without affecting the dock")
            require(wire.call("ui.ai_workspace", {})["open"] is False,
                    "wire reads the current open state")
            wire.call("app.quit", {"force": True})
            wd.shutdown(proc, wire)
            proc = wire = None
            ini = (runtime / "imgui.ini").read_text()
            require("[Workspace][AI]\nOpen=0" in ini,
                    "closed state is saved in the existing ImGui settings")
            require("[Docking][Data]" in ini, "dock tree is saved")
            (runtime / "wire-port").unlink(missing_ok=True)
            proc, wire = open_app(binary, runtime, stderr)
            wd.settle(wire, 12)
            require(not wire.call("app.state")["ai_workspace"]["open"],
                    "closed state survives restart")
            wire.call("ui.ai_workspace", {"open": True, "focus": True})
            wd.settle(wire, 8)
            reopened = wire.call("app.state")["ai_workspace"]
            require(reopened["open"] and reopened["visible"] and reopened["docked"],
                    "reopening returns AI Workspace to its saved tab")
    finally:
        if proc is not None:
            wd.shutdown(proc, wire)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not args.bin.is_file():
        print(f"SKIP: no binary at {args.bin}")
        return 0
    reason = wd.display_skip()
    if reason:
        print(f"SKIP: {reason}")
        return 0
    base = pathlib.Path(tempfile.mkdtemp(prefix="ai-workspace-dock-"))
    runtime = base / "runtime"
    runtime.mkdir()
    wd.ensure_display(WIDTH, HEIGHT)
    try:
        run(args.bin, runtime, base / "app.stderr")
    except (wd.WireError, OSError, AssertionError, TimeoutError) as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        shutil.rmtree(base, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
