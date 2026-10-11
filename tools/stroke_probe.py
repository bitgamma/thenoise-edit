#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""M1 acceptance: a 5000-dab stroke across a 2048^2 document, and the app stays drivable.

The stroke is sent as a polyline; the app turns it into dabs with the brush's own spacing, so
this drives the real paint path rather than a loop of small commands. Timings are printed for
docs/probes/ and only the behavioural bounds are asserted - this catches a paint path that
became tens of times slower or stopped undoing exactly, not one that lost 5%.

    python3 tools/stroke_probe.py --bin build/noise-edit

Skips loudly (exit 0, "SKIP:" on stdout) when it cannot render.
"""

from __future__ import annotations

import argparse
import os
import shutil
import statistics
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

DOC = 2048
BRUSH_SIZE = 20.0
SPACING = 0.25
TARGET_DABS = 5000
# A regression guard, not a benchmark: this box does the whole stroke in ~35 ms, and CI's
# llvmpipe is allowed to be several times slower.
MAX_STROKE_SECONDS = 1.0
MAX_FRAME_MS = 400.0
SAMPLE_INTERVAL = 0.002
# A pointer gesture fed to the app as fast as the wire allows, which is faster than any hand:
# the provisional repaint runs on every event, so this is the pessimistic version of a drag.
GESTURE_EVENTS = 120
GESTURE_BRUSH = 40.0
GESTURE_GAP = 0.004
MAX_GESTURE_EVENT_MS = 60.0
# How much worse the end of a drag may be than its start. 1.0 would be the truth of a per-event
# cost that depends only on the dab; the slack is the buffer growth that a stroke still pays for,
# amortised, plus whatever else the machine was doing.
MAX_GESTURE_DEGRADATION = 4.0


def serpentine(doc: int, dabs: int) -> list[list[float]]:
    """A path long enough to need `dabs` dabs at the brush's own spacing."""
    step = SPACING * BRUSH_SIZE
    run = float(doc) - 2.0 * BRUSH_SIZE
    legs = max(1, min(doc // 8, int(step * dabs / run) + 1))
    points = []
    for i in range(legs + 1):
        y = BRUSH_SIZE + run * (i / legs)
        points.append([BRUSH_SIZE if i % 2 == 0 else doc - BRUSH_SIZE, y])
    return points


class FrameSampler(threading.Thread):
    """Watches `frame` and `frame_ms` while the stroke runs, without blocking it."""

    def __init__(self, port: int) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.frames: list[int] = []
        self.frame_ms: list[float] = []
        self.stop_after = time.monotonic() + 30.0

    def run(self) -> None:
        wire = wd.Wire(self.port)
        last = 0
        while time.monotonic() < self.stop_after:
            try:
                state = wire.call("app.state")
            except (wd.WireError, OSError):
                break
            if state["frame"] != last:
                last = state["frame"]
                self.frames.append(state["frame"])
                self.frame_ms.append(state["frame_ms"])
            else:
                time.sleep(SAMPLE_INTERVAL)

    def stop(self) -> None:
        self.stop_after = time.monotonic()


def gesture_points(doc: int, events: int) -> list[list[float]]:
    """Positions of a diagonal drag, most of the way across the document."""
    margin = GESTURE_BRUSH
    span = doc - 2.0 * margin
    return [[margin + span * i / (events - 1), margin + span * 0.35 + span * 0.4 * i / (events - 1)]
            for i in range(events)]


def set_brush(wire: wd.Wire, size: float, fails: list[str]) -> None:
    """Put the app on the brush the gesture is about to be run with, and refuse to report a size
    the app did not take.

    Brush fields belong to `tool.set`; `tool.brush` selects a tool and reads no fields at all, so a
    probe that set the size through it measured the default brush and printed the size it wished
    for. Reading the state back is what keeps that from being possible again.
    """
    wire.call("tool.set", {"tool": "brush", "size": size, "hardness": 0.8, "opacity": 0.8,
                           "spacing": SPACING})
    got = wire.call("app.state")["brush"]
    if got.get("tool") != "brush" or abs(float(got.get("size", 0.0)) - size) > 1e-9:
        fails.append(f"the probe means a {size:.0f} px brush, the app is on {got}")


def measure_gesture(wire: wd.Wire, port: int, doc: int, layers: int,
                    fails: list[str]) -> None:
    """The same paint fed one pointer event at a time, and what each event costs.

    Run over two document sizes and a layered stack on purpose: a pointer event repaints the
    stroke, repairs the composite over what changed and uploads the canvas texture, so the cost
    is read against the document area and the depth of the stack. A stack that is re-blended
    whole costs tens of milliseconds per event at this size, which is what the layered run below
    holds the line on.
    """
    wire.call("doc.new", {"width": doc, "height": doc, "fill": [24, 32, 40, 255]})
    for index in range(1, layers):
        wire.call("layer.add", {"name": f"L{index}"})
    wd.wait_for_fit(wire)
    set_brush(wire, GESTURE_BRUSH, fails)
    # Let the rebuild that adding layers forces land before sampling: this measures a steady
    # drag, not the one frame after a structural change.
    wd.settle(wire)
    before = wire.call("doc.get", {})["checksum"]
    sampler = FrameSampler(port)
    sampler.start()
    event_ms: list[float] = []
    for index, at in enumerate(gesture_points(doc, GESTURE_EVENTS)):
        phase = "press" if index == 0 else "drag"
        started = time.monotonic()
        wire.call("tool.pointer", {"phase": phase, "at": at})
        event_ms.append((time.monotonic() - started) * 1000.0)
        time.sleep(GESTURE_GAP)
    during = wire.call("app.state")["stroke"]
    started = time.monotonic()
    wire.call("tool.pointer", {"phase": "release"})
    commit_ms = (time.monotonic() - started) * 1000.0
    wd.settle(wire)
    sampler.stop()
    sampler.join(timeout=5)

    painted = wire.call("doc.get", {})["checksum"]
    steps = int(wire.call("app.state", {})["document"]["history"]["undo_depth"])
    history = wire.call("edit.undo", {})
    restored = wire.call("doc.get", {})["checksum"]
    peak = max(sampler.frame_ms, default=0.0)
    # Temporal order first: the degradation check below compares the start of the drag with its
    # end, which a sorted list cannot tell anything about.
    third = max(1, len(event_ms) // 3)
    first = statistics.median(event_ms[:third])
    last = statistics.median(event_ms[-third:])
    event_ms.sort()
    median = event_ms[len(event_ms) // 2]
    worst = event_ms[-1]
    print(f"live gesture {doc}^2 x {layers} layers: {during['dabs']} dabs over "
          f"{GESTURE_EVENTS} pointer events, "
          f"brush {GESTURE_BRUSH:.0f} px: event median {median:.2f} ms, "
          f"worst {worst:.2f} ms, commit {commit_ms:.1f} ms, peak frame_ms {peak:.1f}, "
          f"history {history['stored_bytes'] >> 20} MiB in {steps} step")
    if painted == before:
        fails.append("the gesture painted nothing")
    # Not "one dab per event": dabs are spaced by the brush (`SPACING * size`), events by this
    # harness dividing the polyline into GESTURE_EVENTS steps, and either may be the denser one - a
    # 40 px brush fed 120 events along a 610 px path makes ~61 dabs and is painted correctly. What
    # would mean the gesture was not a stroke is a handful of dabs, so the floor is loose and the
    # checks above and below (something painted, one undo step, undo restoring every byte) carry the
    # rest.
    if int(during["dabs"]) < GESTURE_EVENTS // 4:
        fails.append(
            f"the gesture made {during['dabs']} dabs over {GESTURE_EVENTS} events and a "
            f"{doc}^2 document, which is a dot, not a stroke")
    if steps != 1:
        fails.append(f"one gesture must be one undo step, got {steps}")
    if restored != before:
        fails.append("undo did not restore every byte of the gesture")
    if median > MAX_GESTURE_EVENT_MS:
        fails.append(
            f"a pointer event on a {layers}-layer {doc}^2 document took {median:.1f} ms, "
            f"bound is {MAX_GESTURE_EVENT_MS} ms")
    # The shape, not just the level. An event that costs what the whole stroke has covered makes
    # a drag slower with every event - measured at 2.9 ms to 108 ms over one 40-event stroke - and
    # a median alone can look fine while the end of every stroke is a stall. Ratios survive a
    # slower machine where a fixed bound would not.
    print(f"   first {third} events median {first:.2f} ms, last {third} median {last:.2f} ms "
          f"(x{last / max(first, 0.01):.1f})")
    if last > first * MAX_GESTURE_DEGRADATION:
        fails.append(
            f"a drag got {last / max(first, 0.01):.1f}x more expensive as it grew "
            f"({first:.1f} ms -> {last:.1f} ms per event); an event must cost what its own "
            f"dabs touched, not what the stroke has already covered")


def run(args: argparse.Namespace) -> int:
    runtime = Path(tempfile.mkdtemp(prefix="noise-stroke-"))
    try:
        with (runtime / "app-stderr.log").open("w") as app_stderr:
            proc = wd.start_app(args, runtime, 900, 600, None, app_stderr)
            wire = None
            fails: list[str] = []
            sampler: FrameSampler | None = None
            try:
                port = wd.wire_port(runtime, proc, args.startup_timeout)
                wire = wd.Wire(port)
                wire.call("doc.new", {"width": DOC, "height": DOC, "fill": [24, 32, 40, 255]})
                wd.wait_for_fit(wire)
                before = wire.call("doc.get", {})["checksum"]

                sampler = FrameSampler(port)
                sampler.start()
                started = time.monotonic()
                stroke = wire.call("tool.stroke", {
                    "points": serpentine(DOC, TARGET_DABS),
                    "tool": "brush", "size": BRUSH_SIZE, "hardness": 0.8, "opacity": 1.0,
                    "flow": 1.0, "spacing": SPACING, "color": [210, 40, 90, 255]})
                elapsed = time.monotonic() - started
                wd.settle(wire)
                sampler.stop()
                sampler.join(timeout=5)

                dabs = stroke["dabs"]
                touched = stroke["touched"]
                painted = wire.call("doc.get", {})["checksum"]
                history = wire.call("edit.undo", {})
                restored = wire.call("doc.get", {})["checksum"]
                peak = max(sampler.frame_ms, default=0.0)
                print(f"{dabs} dabs over a {DOC}^2 document, brush {BRUSH_SIZE:.0f} px: "
                      f"stroke {elapsed * 1000.0:.1f} ms, touched {touched[2]}x{touched[3]}, "
                      f"history stored {history['stored_bytes'] >> 20} MiB, "
                      f"peak frame_ms seen {peak:.1f}, "
                      f"{len(set(sampler.frames))} frames presented around it")
                if dabs < TARGET_DABS * 0.9:
                    fails.append(f"the path produced {dabs} dabs, expected about {TARGET_DABS}")
                if elapsed > MAX_STROKE_SECONDS:
                    fails.append(f"the stroke took {elapsed:.3f} s, bound is "
                                 f"{MAX_STROKE_SECONDS} s")
                if peak > MAX_FRAME_MS:
                    fails.append(f"a frame took {peak:.1f} ms, bound is {MAX_FRAME_MS} ms")
                if painted == before:
                    fails.append("the stroke changed nothing")
                if restored != before:
                    fails.append("undo did not restore every byte of a 2048^2 document")
                measure_gesture(wire, port, 640, 1, fails)
                measure_gesture(wire, port, DOC, 1, fails)
                measure_gesture(wire, port, DOC, 3, fails)
            except wd.WireError as exc:
                fails.append(str(exc))
            finally:
                if sampler is not None:
                    sampler.stop()
                wd.shutdown(proc, wire)
            stderr_text = (runtime / "app-stderr.log").read_text(errors="replace")
            if "BUFFER_OVERFLOW" in stderr_text:
                fails.append("the UI overflowed its vertex buffer")
            for line in fails:
                print(f"FAIL: {line}")
            return 1 if fails else 0
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, help="path to noise-edit")
    parser.add_argument("--startup-timeout", type=float, default=40.0)
    args = parser.parse_args()
    if not args.bin.exists():
        print(f"FAIL: no binary at {args.bin}")
        return 1
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
