#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What one pointer event costs in the running app, mask against pixels.

The debug wire is served by the frame loop, so the reply to `tool.pointer` arrives when the frame that
applied it ends: the round trip is the frame, which is what the hand feels. No unit test can see that -
a test measures a function, and the cost that mattered was a scan the panel triggered once per frame.

Skips loudly when there is no display: a missing display is an unavailable resource, not a regression.

    timeout 600 env -u DISPLAY python3 tools/mask_speed_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

SIDE = 2048
EVENTS = 40
# A mask stroke shares its frame with the counts the Layers panel reads, so it is allowed to be a
# little dearer; the regression this catches was 10x, from re-deriving those counts over the whole
# plane on every event.
MASK_PARITY = 2.0
# Dragging a marquee re-outlines the selection every event. Measured with the boundary walk restricted
# to the selection's bounds this sits near 2x a pixel event; it was 18x when the walk scanned the
# canvas each time.
MARQUEE_PARITY = 4.0


def timed(wire, label, events):
    samples = []
    for one in events:
        before = time.perf_counter()
        one()
        samples.append((time.perf_counter() - before) * 1000.0)
    samples.sort()
    median = samples[len(samples) // 2]
    mean = sum(samples) / len(samples)
    print(f'  {label:<40} median {median:7.2f}  mean {mean:7.2f}  worst {samples[-1]:7.2f} ms')
    return median, mean


def drag_events(wire, x0, y0, dx, dy):
    events = []

    def one(press: bool, move: tuple[float, float], step: int):
        def run():
            if press:
                wire.call('tool.pointer', {'phase': 'press', 'at': list(move)})
            elif step == EVENTS - 1:
                wire.call('tool.pointer', {'phase': 'release'})
            else:
                wire.call('tool.pointer', {'phase': 'drag', 'at': list(move)})

        return run

    for step in range(EVENTS):
        events.append(one(step == 0, (x0 + dx * step, y0 + dy * step), step))
    return events


def run(args: argparse.Namespace) -> int:
    runtime = Path(tempfile.mkdtemp(prefix='noise-speed-'))
    fixture = runtime / 'fixture.png'
    wd.write_fixture_png(fixture, SIDE, SIDE)
    with (runtime / 'stderr.log').open('w') as log:
        proc = wd.start_app(args, runtime, 1280, 800, fixture, log)
    fails: list[str] = []
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, args.startup_timeout))
        wire.call('layer.add', {'name': 'masked'})
        wire.call('mask.add')
        wire.call('mask.fill', {'to': 'hide'})
        # A hidden mask, which is what a user reveals into: the stroke itself makes the region it
        # reaches partly covered, so the repair that follows pays the masked arithmetic rather than
        # either shortcut.
        wire.call('tool.set', {'tool': 'brush', 'size': 300, 'hardness': 0.6, 'opacity': 0.8})

        print(f'{SIDE}x{SIDE} document, 300 px brush, {EVENTS} events per drag')
        wire.call('coverage.target', {'to': 'marked'})
        pixels = timed(wire, 'painting the layer', drag_events(wire, 300, 400, 8, 4))
        wire.call('edit.undo')
        wire.call('tool.set', {'tool': 'cover', 'size': 300, 'hardness': 0.6, 'opacity': 0.8})
        wire.call('coverage.target', {'to': 'mask'})
        masked = timed(wire, 'painting the layer mask', drag_events(wire, 300, 900, 8, 4))
        ratio = masked[1] / pixels[1]
        print(f'  a mask event costs {ratio:.2f}x a pixel event')
        if ratio > MASK_PARITY:
            fails.append(f'a mask event costs {ratio:.2f}x a pixel event (limit {MASK_PARITY})')

        wire.call('select.clear')
        wire.call('tool.set', {'tool': 'select'})
        # Back to the transient lifetime: aimed at the mask the same drag re-rasterises the mask and
        # diffs the composite every event, which is a different claim and a different number.
        wire.call('coverage.target', {'to': 'marked'})
        marquee = timed(wire, 'dragging a marquee', drag_events(wire, 200, 200, 10, 6))
        ratio = marquee[1] / pixels[1]
        print(f'  a marquee event costs {ratio:.2f}x a pixel event')
        if ratio > MARQUEE_PARITY:
            fails.append(f'a marquee event costs {ratio:.2f}x a pixel event '
                         f'(limit {MARQUEE_PARITY})')
        wire.call('select.clear')
    except wd.WireError as exc:
        fails.append(str(exc))
    finally:
        wd.shutdown(proc, None)
        shutil.rmtree(runtime, ignore_errors=True)
    for line in fails:
        print(f'FAIL: {line}')
    return 1 if fails else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True, help='path to noise-edit')
    parser.add_argument('--startup-timeout', type=float, default=30.0)
    args = parser.parse_args()
    if not args.bin.exists():
        print(f'FAIL: no binary at {args.bin}')
        return 1
    return run(args)


sys.exit(main())
