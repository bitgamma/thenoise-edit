#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Do the ants still outline a selection that sits far from the origin of a large document?

The boundary walk is windowed to the selection's bounds now, so the claim worth checking with a
capture is that a small selection inside a 2048² canvas has all four of its sides drawn.

The mapping from document to screen is measured from the app rather than assumed: a selection of the
whole canvas puts dashes on the document's own edges, and those edges give the scale and origin that
every later rectangle is checked against. A `view.set` issued before the first fit is overridden by
that fit, which is why this calibrates instead of trusting the reply.

    timeout 600 env -u DISPLAY python3 tools/ants_window_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

SIDE = 2048
BOX = (1200, 1300, 1450, 1500)  # document pixels, far from the origin
SIDE_COVERAGE = 0.4  # dashes alternate, so a drawn side is never a solid line


def changed_pixels(image_a: Image.Image, image_b: Image.Image) -> list[tuple[int, int]]:
    diff = ImageChops.difference(image_a, image_b).convert('RGB')
    raw = diff.tobytes()
    width = image_a.width
    points = []
    for pixel in range(width * image_a.height):
        offset = 3 * pixel
        if raw[offset:offset + 3] != b'\x00\x00\x00':
            points.append((pixel % width, pixel // width))
    return points


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True, help='path to noise-edit')
    parser.add_argument('--startup-timeout', type=float, default=30.0)
    args = parser.parse_args()
    if not args.bin.exists():
        print(f'FAIL: no binary at {args.bin}')
        return 1
    binary = args.bin.resolve()
    runtime = Path(tempfile.mkdtemp(prefix='noise-ants-'))
    fixture = runtime / 'fixture.png'
    wd.write_fixture_png(fixture, SIDE, SIDE)
    with (runtime / 'stderr.log').open('w') as log:
        proc = wd.start_app(args, runtime, 1280, 800, fixture, log)
    fails: list[str] = []
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, args.startup_timeout))
        wire.call('ants.freeze', {'phases': [0.0]})
        wire.call('select.clear')
        plain = Path(wire.call('screenshot', {'name': 'plain'})['path'])

        # Calibrate: the whole-canvas selection's dashes sit on the document's own edges.
        wire.call('select.all')
        whole = Path(wire.call('screenshot', {'name': 'whole'})['path'])
        image = Image.open(plain)
        points = changed_pixels(Image.open(whole), image)
        if len(points) < 200:
            fails.append(f'nothing was drawn for a selection of the whole canvas ({points[:5]})')
            raise SystemExit
        columns = Counter(x for x, _ in points)
        rows = Counter(y for _, y in points)
        # The two edges are the lines with the most pixels; panel text changes are scattered.
        # An edge is a line hundreds of pixels long; panel text is a handful of pixels per column, so
        # "at least half as long as the longest" separates them.
        best_column = max(columns.values())
        best_row = max(rows.values())
        tall = sorted(c for c, n in columns.items() if n >= best_column * 0.5)
        wide = sorted(r for r, n in rows.items() if n >= best_row * 0.5)
        if len(tall) < 2 or len(wide) < 2:
            fails.append(f'could not find the document edges: columns {tall[:4]}, rows {wide[:4]}')
            raise SystemExit
        left, right = tall[0], tall[-1]
        top, bottom = wide[0], wide[-1]
        scale = (right - left) / float(SIDE)
        print(f'document on screen: {left},{top} to {right},{bottom} (scale {scale:.4f})')

        wire.call('select.clear')
        wire.call('tool.set', {'tool': 'select'})
        wire.call('tool.pointer', {'phase': 'press', 'at': [BOX[0], BOX[1]]})
        wire.call('tool.pointer', {'phase': 'drag', 'at': [BOX[2], BOX[3]]})
        wire.call('tool.pointer', {'phase': 'release'})
        got = wire.call('select.get')
        print(f'selection: {got}')
        marquee = Path(wire.call('screenshot', {'name': 'marquee'})['path'])
        shutil.copy(marquee, '/tmp/ants_far_box.png')

        points = changed_pixels(Image.open(marquee), image)
        want = [round(left + scale * BOX[0]), round(top + scale * BOX[1]),
                round(left + scale * BOX[2]), round(top + scale * BOX[3])]
        near = [p for p in points
                if want[0] - 3 <= p[0] <= want[2] + 3 and want[1] - 3 <= p[1] <= want[3] + 3]
        print(f'ants expected at {want[0]},{want[1]} to {want[2]},{want[3]}; '
              f'{len(near)} px changed there')
        if not near:
            fails.append('the selection far from the origin drew nothing where it is')
        else:
            pad = 2
            sides = {
                'left': (len([p for p in near if abs(p[0] - want[0]) <= pad]),
                         want[3] - want[1]),
                'right': (len([p for p in near if abs(p[0] - want[2]) <= pad]), want[3] - want[1]),
                'top': (len([p for p in near if abs(p[1] - want[1]) <= pad]), want[2] - want[0]),
                'bottom': (len([p for p in near if abs(p[1] - want[3]) <= pad]),
                           want[2] - want[0]),
            }
            for name, (count, span) in sides.items():
                print(f'  {name} side: {count} px along a {span} px side')
                if count < span * SIDE_COVERAGE:
                    fails.append(f'the {name} side is not outlined')
    except SystemExit:
        pass
    finally:
        wd.shutdown(proc, None)
        shutil.rmtree(runtime, ignore_errors=True)
    for line in fails:
        print(f'FAIL: {line}')
    return 1 if fails else 0


sys.exit(main())
