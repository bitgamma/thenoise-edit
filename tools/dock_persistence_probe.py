#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resize and set the Layers split, then verify the ImGui settings survive restart."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402

WIDTH, HEIGHT = 1280, 800
DOCK_DRAG_PX = 70
DOCK_RESIZE_TOLERANCE_PX = 5
MINIMUM_DOCK_WIDTH_PX = 280
DOCK_NARROW_DRAG_PX = 180
SPLIT_DRAG_PX = 60
SPLIT_RESIZE_TOLERANCE_PX = 20
COMMAND_SPLIT_RATIO = 0.60
SCROLL_SPLIT_RATIO = 0.75


class Probe:
    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> None:
        print(("  ok   " if ok else "  FAIL ") + claim +
              (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1


def start_app(binary: pathlib.Path, runtime: pathlib.Path, fixture: pathlib.Path, stderr) -> tuple:
    args = argparse.Namespace(bin=binary, theme="dark")
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, fixture, stderr,
                        {"THENOISE_URL": None, "THENOISE_API_KEY": None})
    wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
    return proc, wire


def dock_width(wire: wd.Wire) -> tuple[float, list[float]]:
    rect = wire.call("app.state")["layers_window"]
    return float(rect[2]), rect


def split_y(state: dict) -> int:
    add = state["layer_panel"]["add"]
    return round(add[1] + add[3] + 8.0)


def click(inj: wi.Injector, x: float, y: float) -> None:
    inj.move(x, y)
    inj.button(1, True)
    inj.button(1, False)


def drag_split(inj: wi.Injector, wire: wd.Wire, delta: int) -> dict:
    state = wire.call("app.state")
    panel = state["layers_window"]
    x = round(panel[0] + panel[2] * 0.5)
    y = split_y(state)
    inj.move(x, y)
    inj.button(1, True)
    inj.move(x, y + delta)
    inj.button(1, False)
    wd.settle(wire, 12)
    return wire.call("app.state")


def run(binary: pathlib.Path, runtime: pathlib.Path, stderr_path: pathlib.Path,
        fixture: pathlib.Path, display: str, probe: Probe) -> None:
    proc = wire = inj = None
    try:
        with stderr_path.open("ab") as stderr:
            proc, wire = start_app(binary, runtime, fixture, stderr)
            inj = wi.Injector(display)
            inj.focus_app_window(WIDTH, HEIGHT)
            wd.settle(wire, 12)
            initial_width, initial = dock_width(wire)
            probe.check(initial_width >= MINIMUM_DOCK_WIDTH_PX,
                        "fresh layout provides a usable Layers dock",
                        f"{initial_width:.0f}px")
            probe.check(initial[1] >= 27.0,
                        "Layers/Log tabs start below the application menu bar",
                        f"dock tabs begin at y={initial[1]:.0f}px")

            initial_state = wire.call("app.state")
            initial_list = initial_state["layer_panel"]["list"]
            initial_ratio = initial_state["layer_split"]["upper_ratio"]
            raised = drag_split(inj, wire, -SPLIT_DRAG_PX)
            raised_list = raised["layer_panel"]["list"]
            probe.check(raised["layer_split"]["upper_ratio"] < initial_ratio,
                        "dragging the splitter updates the reported split ratio",
                        f"{initial_ratio:.3f} → {raised['layer_split']['upper_ratio']:.3f}")
            probe.check(raised_list[3] < initial_list[3] - SPLIT_RESIZE_TOLERANCE_PX,
                        "internal horizontal splitter drags upward",
                        f"{initial_list[3]:.0f}px → {raised_list[3]:.0f}px list")
            lowered = drag_split(inj, wire, SPLIT_DRAG_PX)
            lowered_list = lowered["layer_panel"]["list"]
            probe.check(lowered["layer_split"]["upper_ratio"] > raised["layer_split"]["upper_ratio"],
                        "dragging downward increases the reported split ratio",
                        f"{raised['layer_split']['upper_ratio']:.3f} → "
                        f"{lowered['layer_split']['upper_ratio']:.3f}")
            probe.check(lowered_list[3] > raised_list[3] + SPLIT_RESIZE_TOLERANCE_PX,
                        "internal horizontal splitter drags downward",
                        f"{raised_list[3]:.0f}px → {lowered_list[3]:.0f}px list")
            wire.call("ui.layer_split", {"upper_ratio": COMMAND_SPLIT_RATIO})
            wd.settle(wire, 8)
            commanded = wire.call("app.state")
            probe.check(abs(commanded["layer_split"]["upper_ratio"] - COMMAND_SPLIT_RATIO) < 0.001,
                        "wire command sets the ratio used by the Layers layout",
                        f"ratio={commanded['layer_split']['upper_ratio']:.3f}")
            probe.check(commanded["layer_panel"]["list"][3] > lowered_list[3] +
                        SPLIT_RESIZE_TOLERANCE_PX,
                        "wire-set ratio updates the rendered upper-region height",
                        f"{lowered_list[3]:.0f}px → {commanded['layer_panel']['list'][3]:.0f}px")

            wire.call("mask.add", {})
            wd.settle(wire, 8)
            settings_header = wire.call("app.state")["tool_options"]
            click(inj, settings_header[0] + settings_header[2] * 0.5,
                  settings_header[1] + settings_header[3] * 0.5)
            wd.settle(wire, 8)
            expanded = wire.call("app.state")
            controls = expanded["tool_option_controls"]
            probe.check(len(controls) == 5,
                        "Tool Settings expands below the mask controls",
                        f"{len(controls)} brush controls")
            # Make the lower pane short *after* opening its header, so every scroll
            # test hits the lower child instead of a menu or an offscreen header.
            wire.call("ui.layer_split", {"upper_ratio": SCROLL_SPLIT_RATIO})
            wd.settle(wire, 8)
            compressed = wire.call("app.state")
            controls = compressed["tool_option_controls"]
            control_y = controls[0]["screen"][1] if controls else 0.0
            list_height = compressed["layer_panel"]["list"][3]
            panel = compressed["layers_window"]
            lower_y = (split_y(compressed) + panel[1] + panel[3]) * 0.5
            inj.move(panel[0] + panel[2] * 0.5, lower_y)
            inj.wheel(-5)
            wd.settle(wire, 8)
            scrolled_lower = wire.call("app.state")
            controls_after = scrolled_lower["tool_option_controls"]
            after_y = controls_after[0]["screen"][1] if controls_after else 0.0
            probe.check(control_y - after_y > 10.0,
                        "lower Mask and Tool Settings region scrolls independently",
                        f"first control y={control_y:.0f} → {after_y:.0f}")
            probe.check(abs(scrolled_lower["layer_panel"]["list"][3] - list_height) <= 1.0,
                        "lower scrolling leaves the layer-list allocation unchanged")
            wire.call("ui.layer_split", {"upper_ratio": COMMAND_SPLIT_RATIO})
            wd.settle(wire, 8)

            divider_x = round(initial[0] - 4.0)
            divider_y = round(initial[1] + initial[3] * 0.5)
            inj.move(divider_x, divider_y)
            inj.button(1, True)
            inj.move(divider_x - DOCK_DRAG_PX, divider_y)
            inj.button(1, False)
            wd.settle(wire, 12)
            resized_width, resized = dock_width(wire)
            probe.check(resized_width >= initial_width + DOCK_DRAG_PX * 0.5,
                        "dock divider remains resizable", f"{initial_width:.0f}px → "
                        f"{resized_width:.0f}px")

            narrow_x = round(resized[0] - 4.0)
            narrow_y = round(resized[1] + resized[3] * 0.5)
            inj.move(narrow_x, narrow_y)
            inj.button(1, True)
            inj.move(narrow_x + DOCK_NARROW_DRAG_PX, narrow_y)
            inj.button(1, False)
            wd.settle(wire, 12)
            narrowed_width, narrowed = dock_width(wire)
            probe.check(narrowed_width >= MINIMUM_DOCK_WIDTH_PX - DOCK_RESIZE_TOLERANCE_PX,
                        "right dock keeps its minimum width while dragged narrower",
                        f"{narrowed_width:.0f}px")

            restore_x = round(narrowed[0] - 4.0)
            restore_y = round(narrowed[1] + narrowed[3] * 0.5)
            restore_delta = round(resized_width - narrowed_width)
            inj.move(restore_x, restore_y)
            inj.button(1, True)
            inj.move(restore_x - restore_delta, restore_y)
            inj.button(1, False)
            wd.settle(wire, 12)
            restored_drag_width, _ = dock_width(wire)
            probe.check(abs(restored_drag_width - resized_width) <= DOCK_RESIZE_TOLERANCE_PX,
                        "outer dock can expand again after reaching its minimum",
                        f"{narrowed_width:.0f}px → {restored_drag_width:.0f}px")

            wire.call("app.quit", {"force": True})
            wd.shutdown(proc, wire)
            proc = wire = None
            if inj is not None:
                inj.close()
                inj = None

            ini = runtime / "imgui.ini"
            probe.check(ini.is_file() and "[Docking][Data]" in ini.read_text(),
                        "ImGui writes the dock layout to its existing settings file")
            settings = ini.read_text() if ini.is_file() else ""
            probe.check("[LayerPanel][Layers]" in settings and
                        f"UpperRatio={COMMAND_SPLIT_RATIO:.6f}" in settings,
                        "ImGui stores the wire-set split ratio in the same settings file")
            (runtime / "wire-port").unlink(missing_ok=True)

            with stderr_path.open("ab") as stderr:
                proc, wire = start_app(binary, runtime, fixture, stderr)
                wd.settle(wire, 12)
                restored_width, _ = dock_width(wire)
                probe.check(abs(restored_width - resized_width) <= DOCK_RESIZE_TOLERANCE_PX,
                            "resized dock width survives restart",
                            f"saved {resized_width:.0f}px, restored {restored_width:.0f}px")
                restored_state = wire.call("app.state")
                restored_y = split_y(restored_state)
                saved_y = split_y(commanded)
                probe.check(abs(restored_y - saved_y) <= DOCK_RESIZE_TOLERANCE_PX,
                            "internal split allocation survives restart",
                            f"saved y={saved_y}, restored y={restored_y}")
                restored_ratio = restored_state["layer_split"]["upper_ratio"]
                probe.check(abs(restored_ratio - COMMAND_SPLIT_RATIO) < 0.001,
                            "app.state reports the persisted split ratio after restart",
                            f"saved {COMMAND_SPLIT_RATIO:.3f}, restored {restored_ratio:.3f}")
    finally:
        if inj is not None:
            inj.close()
        wd.shutdown(proc, wire)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not args.bin.is_file():
        print(f"SKIP: no binary at {args.bin}")
        return 0
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0

    root = pathlib.Path(tempfile.mkdtemp(prefix="dock-persistence-"))
    runtime = root / "runtime"
    runtime.mkdir()
    stderr_path = root / "app.stderr"
    fixture = root / "splitter-fixture.png"
    wd.write_fixture_png(fixture, 320, 192)
    display = wd.ensure_display(WIDTH, HEIGHT)
    probe = Probe()
    try:
        run(args.bin, runtime, stderr_path, fixture, display, probe)
    except (wd.WireError, OSError, TimeoutError) as exc:
        print(f"FAIL: {exc}")
        probe.failed += 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("FAIL" if probe.failed else "ok")
    return 1 if probe.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
