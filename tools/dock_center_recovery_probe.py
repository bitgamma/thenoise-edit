#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A saved Layers tab inside the Canvas no-tab node must recover without resetting the dock."""
from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

WIDTH, HEIGHT = 1280, 800


def window_section(text: str, name: str) -> re.Match[str]:
    pattern = rf"(?ms)^(\[Window\]\[{re.escape(name)}\]\n)(.*?)(?=^\[|\Z)"
    match = re.search(pattern, text)
    if match is None:
        raise AssertionError(f"missing ImGui window settings for {name}")
    return match


def dock_id(text: str, name: str) -> str:
    section = window_section(text, name).group(2)
    match = re.search(r"(?m)^DockId=(0x[0-9A-Fa-f]+)", section)
    if match is None:
        raise AssertionError(f"missing saved DockId for {name}")
    return match.group(1)


def corrupt_layers_dock(text: str) -> str:
    canvas = dock_id(text, "Canvas")
    layers = window_section(text, "Layers")
    body = layers.group(2)
    changed = re.sub(r"(?m)^DockId=0x[0-9A-Fa-f]+", "DockId=" + canvas, body, count=1)
    if changed == body:
        raise AssertionError("could not reproduce the saved central dock layout")
    return text[:layers.start(2)] + changed + text[layers.end(2):]


def start(binary: pathlib.Path, runtime: pathlib.Path, stderr):
    args = argparse.Namespace(bin=binary, theme="dark")
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, stderr,
                        {"THENOISE_URL": None, "THENOISE_API_KEY": None},
                        start_dialog=False)
    return proc, wd.Wire(wd.wire_port(runtime, proc, 60.0))


def stop(proc, wire) -> None:
    wire.call("app.quit", {"force": True})
    wd.shutdown(proc, wire)


def assert_layers_accessible(wire) -> None:
    wd.settle(wire, 20)
    state = wire.call("app.state")
    layer = state["layers_window"]
    canvas = state["canvas"]
    if layer[2] <= 200 or layer[2] >= canvas[2] * 0.75:
        raise AssertionError(f"Layers remains merged into the hidden Canvas tab: {layer}")
    if not state["ai_workspace"]["open"]:
        raise AssertionError("repair unexpectedly closed AI Workspace")


def run(binary: pathlib.Path, runtime: pathlib.Path, stderr_path: pathlib.Path) -> None:
    proc = wire = None
    ini = runtime / "imgui.ini"
    try:
        with stderr_path.open("ab") as stderr:
            proc, wire = start(binary, runtime, stderr)
            wd.settle(wire, 15)
            stop(proc, wire)
            proc = wire = None
            original = ini.read_text()
            if dock_id(original, "Canvas") == dock_id(original, "Layers"):
                raise AssertionError("fresh default unexpectedly docks Layers into Canvas")
            ini.write_text(corrupt_layers_dock(original))
            (runtime / "wire-port").unlink(missing_ok=True)
            proc, wire = start(binary, runtime, stderr)
            assert_layers_accessible(wire)
            stop(proc, wire)
            proc = wire = None
            repaired = ini.read_text()
            if dock_id(repaired, "Canvas") == dock_id(repaired, "Layers"):
                raise AssertionError("central-node repair was not persisted")
            if dock_id(repaired, "Layers") != dock_id(repaired, "Log"):
                raise AssertionError("recovery did not reuse the existing right dock")
            print("ok: invalid saved Layers center-tab repaired into existing right dock")
            (runtime / "wire-port").unlink(missing_ok=True)
            proc, wire = start(binary, runtime, stderr)
            assert_layers_accessible(wire)
            print("ok: recovered arrangement survives restart")
    finally:
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
    base = pathlib.Path(tempfile.mkdtemp(prefix="dock-center-recovery-"))
    try:
        run(args.bin, base / "runtime", base / "app.stderr")
    except (wd.WireError, OSError, AssertionError, TimeoutError) as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        shutil.rmtree(base, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
