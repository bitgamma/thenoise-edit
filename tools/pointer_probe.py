#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does the ring on the canvas measure the brush the app says it has?

Brush size was unreadable: the cursor was the desktop arrow whatever tool was selected, so the only way
to know how wide a brush was is to press `[` and read a number in a panel. Now a dab tool hides the OS
cursor and draws the dab's footprint instead - so the claim worth a capture is that the ring on the
screen and the size in the state are the same number, at the zoom in force.

The ring is found by subtracting a capture with the overlay forced off from one with it forced on, so
nothing but the overlay can account for a changed pixel. The pointer is moved with real motion events,
which is also what puts the app's reported `pointer.at` under test.

    timeout 600 env -u DISPLAY python3 tools/pointer_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import math
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

WIDTH, HEIGHT = 1100, 760
DOC = (400, 300)
POINT = (200, 150)  # document pixels, away from every edge
# The ring is a one-pixel line straddling the rim, and the antialiasing that keeps it visible reaches a
# pixel beyond that. Anything wider is the overlay measuring something other than the brush.
RADIUS_TOLERANCE_PX = 1.5


class Probe:
    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> bool:
        print(("  ok   " if ok else "  FAIL ") + claim + (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def screen_of(state: dict, x: float, y: float) -> tuple[float, float]:
    left, top, _, _ = wd.document_screen_rect(state)
    scale = state["view"]["scale"]
    return left + x * scale, top + y * scale


def doc_capture(wire) -> tuple[Image.Image, pathlib.Path]:
    """The document's own rect, with no panel chrome that could change for other reasons."""
    state = wire.call("app.state")
    left, top, right, bottom = wd.document_screen_rect(state)
    shot = wire.call("screenshot", {"region": [round(left), round(top),
                                               round(right - left), round(bottom - top)],
                                    "name": "pointer"})
    path = pathlib.Path(shot["path"])
    return Image.open(path).convert('RGB'), path


def ring_measurement(before: Image.Image, after: Image.Image, at: tuple[float, float],
                     reach: float) -> tuple[float, int]:
    """The diameter of what the overlay added, measured from the pointer, and how many pixels changed.

    Only pixels within `reach` of the pointer are considered: the overlay is a circle of about the
    expected radius, and anything further out is not it - and must not be, which the caller checks
    separately.
    """
    diff = ImageChops.difference(before, after)
    raw = diff.tobytes()
    far = 0
    radius = 0.0
    changed = 0
    for y in range(after.height):
        for x in range(after.width):
            offset = 3 * (y * after.width + x)
            if raw[offset:offset + 3] == b'\x00\x00\x00':
                continue
            distance = math.hypot(x - at[0], y - at[1])
            changed += 1
            if distance > reach:
                far += 1
            radius = max(radius, distance)
    return 2.0 * radius, changed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="pointer-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    p = Probe()
    print("the pointer says which tool it is, and how wide the brush is")
    skip = wd.display_skip()
    if skip:
        print(f"SKIP: {skip}")
        return 0
    display = wd.ensure_display(WIDTH, HEIGHT)
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, open(tmp / "app.stderr", "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None},
                        start_dialog=False)
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0), timeout=120)
        inj = wi.Injector(display)
        wire.call("doc.new", {"width": DOC[0], "height": DOC[1]})
        wd.wait_for_fit(wire)
        state = wire.call("app.state")
        px, py = screen_of(state, POINT[0], POINT[1])

        inj.move(px, py)
        wd.settle(wire)
        state = wire.call("app.state")
        pointer = state["pointer"]
        p.check(pointer["over_canvas"] is True, "the canvas has the pointer")
        p.check(abs(pointer["at"][0] - POINT[0]) <= 1.5 and abs(pointer["at"][1] - POINT[1]) <= 1.5,
                "and the app puts it where the motion event said",
                f"at {pointer['at']}, expected {list(POINT)}")

        # A marquee tool: the host cursor takes the shape, and nothing is drawn on the canvas - a ring
        # around a select tool would promise a dab.
        wire.call("tool.select", {})
        wd.settle(wire)
        select_state = wire.call("app.state")["pointer"]
        p.check(select_state["shape"] == "crosshair" and select_state["cursor"] == "crosshair",
                "the select tool asks the host for a crosshair", str(select_state["cursor"]))
        wire.call("pointer.footprint", {"show": False})
        wd.settle(wire)
        off_select, off_select_path = doc_capture(wire)
        wire.call("pointer.footprint", {"show": True})
        wd.settle(wire)
        forced, forced_path = doc_capture(wire)
        shutil.copy(forced_path, "/tmp/pointer_forced.png")
        forced_box = ImageChops.difference(off_select, forced).getbbox()
        p.check(forced_box is not None, "forcing the ring on draws one even for a tool that has no dab",
                str(forced_box))

        # The brush: the ring is the size, and the OS cursor stands down because the ring replaced it.
        wire.call("pointer.footprint", {"show": None})
        wire.call("tool.brush", {})
        wd.settle(wire)
        brush_state = wire.call("app.state")
        expected = brush_state["brush"]["size"] * brush_state["view"]["scale"]
        p.check(brush_state["pointer"]["cursor"] == "hidden",
                "the OS cursor stands down while the ring is on screen",
                str(brush_state["pointer"]["cursor"]))
        without, without_path = doc_capture(wire)
        wire.call("pointer.footprint", {"show": False})
        wd.settle(wire)
        plain, plain_path = doc_capture(wire)
        p.check(ImageChops.difference(without, plain).getbbox() is not None,
                "and it is the ring that is on the canvas, not the cursor",
                str(ImageChops.difference(without, plain).getbbox()))

        left, top, _, _ = wd.document_screen_rect(brush_state)
        pointer_x, pointer_y = screen_of(brush_state, *brush_state["pointer"]["at"])
        diameter, changed = ring_measurement(plain, without,
                                             (pointer_x - left, pointer_y - top),
                                             1.6 * expected)
        shutil.copy(without_path, "/tmp/pointer_brush.png")
        p.check(changed > 20, "the ring is on screen", f"{changed} px")
        p.check(abs(diameter - expected) <= 2.0 * RADIUS_TOLERANCE_PX,
                "and its diameter is the brush size at this zoom",
                f"measured {diameter:.1f} px, expected {expected:.1f} px")

        # A wider brush: the ring has to follow, or it measures nothing.
        for _ in range(6):
            wire.call("brush.larger", {})
        wd.settle(wire)
        wider = wire.call("app.state")
        expected_wide = wider["brush"]["size"] * wider["view"]["scale"]
        pointer_x, pointer_y = screen_of(wider, *wider["pointer"]["at"])
        wire.call("pointer.footprint", {"show": False})
        wd.settle(wire)
        wide_plain, wide_plain_path = doc_capture(wire)
        wire.call("pointer.footprint", {"show": True})
        wd.settle(wire)
        wide_ring, wide_ring_path = doc_capture(wire)
        wide_diameter, _ = ring_measurement(wide_plain, wide_ring,
                                           (pointer_x - left, pointer_y - top),
                                           1.6 * expected_wide)
        p.check(wide_diameter > diameter + 2.0, "a wider brush draws a wider ring",
                f"{diameter:.1f} px to {wide_diameter:.1f} px")
        growth = wide_diameter - diameter
        expected_growth = expected_wide - expected
        p.check(abs(growth - expected_growth) <= RADIUS_TOLERANCE_PX,
                "and its growth matches the size the app reports",
                f"measured +{growth:.1f} px, expected +{expected_growth:.1f} px")
        shutil.copy(wide_ring_path, "/tmp/pointer_brush_wide.png")

        # Leaving the canvas has to hand the cursor back: the brush hid it, and nothing else in the build
        # re-shows it. The panel is where a person goes next, so an invisible pointer there is the bug
        # this claim exists for.
        # Left of the canvas *window*, not left of the document: the backdrop beside the picture is
        # still the canvas, and the tool acts there, so the ring belongs there too.
        # Up out of the canvas window, into the row above it. Sideways does not work here: the pointer
        # clamps against the Xvfb framebuffer before it reaches the panel column, so a sideways claim
        # would be testing the display and not the app.
        canvas = wider["canvas"]
        inj.move(px, canvas[1] - 10.0)
        wd.settle(wire)
        gone = wire.call("app.state")["pointer"]
        p.check(gone["over_canvas"] is False and gone["cursor"] != "hidden",
                "and the cursor comes back when the canvas does not have it",
                f"over_canvas={gone['over_canvas']} cursor={gone['cursor']}")
    finally:
        wd.shutdown(proc, wire)
        shutil.rmtree(runtime, ignore_errors=True)
    print("FAIL" if p.failed else "ok")
    return 1 if p.failed else 0


sys.exit(main())
