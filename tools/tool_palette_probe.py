#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Click the tool palette at its reported rectangles, not hard-coded positions."""
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402

WIDTH, HEIGHT = 1100, 760
EXPECTED = {"move", "select", "select_ellipse", "lasso", "cover", "un-cover", "brush", "eraser"}


def inside(box: list, parent: list) -> bool:
    return (box[2] > 20 and box[3] > 20 and box[0] >= parent[0] and
            box[1] >= parent[1] and box[0] + box[2] <= parent[0] + parent[2] + 1 and
            box[1] + box[3] <= parent[1] + parent[3] + 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--width", type=int, default=WIDTH)
    args = parser.parse_args()
    skip = wd.display_skip()
    if skip:
        print(f"SKIP: {skip}")
        return 0
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="tool-palette-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    display = wd.ensure_display(max(WIDTH, args.width), HEIGHT)
    proc = wd.start_app(args, runtime, args.width, HEIGHT, None, open(tmp / "app.stderr", "wb"),
                        {"THENOISE_URL": None, "THENOISE_API_KEY": None}, start_dialog=False)
    wire = None
    failures = []
    try:
        startup_begin = time.monotonic()
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0), timeout=60)
        print(f"wire ready in {time.monotonic() - startup_begin:.2f} s")
        inj = wi.Injector(display)
        inj.focus_app_window(args.width, HEIGHT)
        wire.call("doc.new", {"width": 200, "height": 120})
        wd.settle(wire)
        state = wire.call("app.state")
        palette = state["tool_palette"]
        buttons = palette["buttons"]
        ids = [b["id"] for b in buttons]
        if set(ids) != EXPECTED or len(ids) != len(EXPECTED):
            failures.append(f"palette does not show every tool exactly once: {ids}")
        window = palette["window"]
        if not all(inside(b["screen"], window) for b in buttons):
            failures.append(f"buttons outside palette: {window}, {buttons}")
        canvas = state["canvas"]
        canvas_left = canvas[0]
        if window[0] + window[2] > canvas_left + 2:
            failures.append(f"palette overlaps canvas: {window}, canvas at {canvas_left}")
        options = state["tool_options"]
        if not (options[2] > 200 and options[3] >= 80 and
                options[0] >= window[0] + window[2] - 2 and
                options[1] + options[3] <= canvas[1] + 2):
            failures.append(f"tool options must stay above canvas, after the palette: {options}, {canvas}")
        wire.call("tool.brush")
        wd.settle(wire)
        options = wire.call("app.state")
        toolbar = options["tool_options"]
        sliders = {c["id"]: c["screen"] for c in options["tool_option_controls"]}
        required = {"size", "hardness", "opacity", "flow", "spacing", "color"}
        if sliders.keys() != required:
            failures.append(f"tool options missing controls: {sliders}")
        else:
            if not all(sliders["size"][2] > sliders[key][2] * 1.2 for key in
                       ("hardness", "opacity", "flow", "spacing")):
                failures.append(f"Size must be widest: {sliders}")
            for name, rect in sliders.items():
                if not (rect[0] >= toolbar[0] - 1 and rect[1] >= toolbar[1] - 1 and
                        rect[0] + rect[2] <= toolbar[0] + toolbar[2] + 1 and
                        rect[1] + rect[3] <= toolbar[1] + toolbar[3] + 1):
                    failures.append(f"{name} clips outside toolbar: {rect}, {toolbar}")
        for button in buttons:
            if not inside(button["screen"], window):
                continue
            x, y, w, h = button["screen"]
            inj.move(round(x + w / 2), round(y + h / 2))
            inj.button(1, True)
            inj.button(1, False)
            wd.settle(wire)
            active = wire.call("app.state")["brush"]["tool"]
            if active != button["id"]:
                failures.append(f"click {button['id']} selected {active}")
        if failures:
            for failure in failures:
                print("FAIL:", failure)
            return 1
        print("Tool palette click and layout checks passed.")
        return 0
    finally:
        wd.shutdown(proc, wire)


if __name__ == "__main__":
    raise SystemExit(main())
