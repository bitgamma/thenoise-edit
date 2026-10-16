#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Is marked coverage a wash over the area, and only over the area?

`view.coverage` chooses boundary, wash, or both. The claim a capture can make and a unit test cannot:
turning the wash on changes every covered pixel and leaves the rest of the canvas exactly as it was. A
haze over the whole document, or a wash that misses the part of the mark away from the origin, both fail.

The ants are frozen at one phase first, so the only difference between the two captures is the wash.

    timeout 600 xvfb-run -a python3 tools/coverage_view_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

DOC = (320, 240)
MARK = (40, 30, 180, 120)  # document pixels: x, y, w, h
WASH_FRACTION = 0.6  # antialiasing and the ants' own pixels account for the rest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True, help='path to noise-edit')
    parser.add_argument('--startup-timeout', type=float, default=30.0)
    args = parser.parse_args()
    if not args.bin.exists():
        print(f'FAIL: no binary at {args.bin}')
        return 1
    runtime = Path(tempfile.mkdtemp(prefix='noise-coverage-view-'))
    fixture = runtime / 'fixture.png'
    wd.write_fixture_png(fixture, *DOC)
    fails: list[str] = []
    with (runtime / 'stderr.log').open('w') as log:
        proc = wd.start_app(args, runtime, 1100, 760, fixture, log)
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, args.startup_timeout))
        wd.wait_for_fit(wire)
        wire.call('ants.freeze', {'phases': [0.0]})
        wire.call('select.rect', {'rect': list(MARK)})
        got = wire.call('select.get')
        print(f'mark: {got}')

        state = wire.call('app.state')
        left, top, right, bottom = wd.document_screen_rect(state)
        scale = (right - left) / float(state['view']['image'][0])
        box = [round(left + scale * MARK[0]), round(top + scale * MARK[1]),
               round(left + scale * (MARK[0] + MARK[2])), round(top + scale * (MARK[1] + MARK[3]))]
        print(f'the mark on screen: {box[0]},{box[1]} to {box[2]},{box[3]} (scale {scale:.3f})')

        wire.call('view.coverage', {'mode': 'ants'})
        if wire.call('app.state')['view']['coverage'] != 'ants':
            fails.append('view.coverage did not take the mode')
        boundary_only = wd.capture_canvas(wire, 'boundary-only')

        wire.call('view.coverage', {'mode': 'both'})
        with_wash = wd.capture_canvas(wire, 'with-wash')

        # The capture is local to its own region, so the mark has to be moved out of framebuffer
        # coordinates before any pixel is judged.
        ox, oy = boundary_only['region'][0], boundary_only['region'][1]
        box = [box[0] - ox, box[1] - oy, box[2] - ox, box[3] - oy]

        before = Image.open(boundary_only['path']).convert('RGB')
        after = Image.open(with_wash['path']).convert('RGB')
        diff = ImageChops.difference(before, after)
        raw = diff.tobytes()
        width = before.width
        inside = 0
        outside = 0
        for pixel in range(width * before.height):
            offset = 3 * pixel
            if raw[offset:offset + 3] == b'\x00\x00\x00':
                continue
            x, y = pixel % width, pixel // width
            if box[0] <= x < box[2] and box[1] <= y < box[3]:
                inside += 1
            else:
                outside += 1
        area = max(1, (box[2] - box[0]) * (box[3] - box[1]))
        print(f'wash: {inside} px changed inside a {area} px mark, {outside} px changed outside it')
        shutil.copy(with_wash['path'], '/tmp/coverage_wash.png')
        shutil.copy(boundary_only['path'], '/tmp/coverage_boundary.png')
        if inside < area * WASH_FRACTION:
            fails.append(f'the wash did not cover the mark ({inside} of {area} px)')
        if outside:
            fails.append(f'the wash reached {outside} px outside the mark')
    finally:
        wd.shutdown(proc, None)
        shutil.rmtree(runtime, ignore_errors=True)
    for line in fails:
        print(f'FAIL: {line}')
    return 1 if fails else 0


sys.exit(main())
