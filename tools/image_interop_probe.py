#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The flat image formats, handled by someone else's codecs.

Our encoder agreeing with our decoder proves the pair is self-consistent, which is worth little: a
header written wrong in the same way on both sides keeps every in-house test green while the file
opens as garbage anywhere else. So the files cross to Pillow - an implementation that has never
heard of us - and come back, in both directions, through the running app.

    python3 tools/image_interop_probe.py --bin build/noise-edit

Skips loudly (exit 0, "SKIP:" on stdout) when it cannot render, or when Pillow or numpy is missing.
"""

from __future__ import annotations

import argparse
import base64
import io
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as _error:  # the probe is an extra; a machine without them must skip, not fail
    np = None
    Image = None
    MISSING_IMAGE_STACK = str(_error)
else:
    MISSING_IMAGE_STACK = ""

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

WIDTH = 96
HEIGHT = 64
# Regression bounds, not quality claims: what this box measures is an order of magnitude inside
# them (a smooth gradient round-trips JPEG at about 0.7 per channel). They are here to catch a codec
# wired to the wrong colour space, a flipped axis, or alpha treated as premultiplied - each of which
# measures tens or hundreds.
MAX_LOSSY_MEAN_DIFF = 8.0
# Pixels that went in fully transparent must not come back as paint.
MAX_TRANSPARENT_ALPHA = 24.0


def paint(wire: wd.Wire) -> None:
    """Something with soft edges and partial coverage, not a flat fill."""
    wire.call("doc.new", {"width": WIDTH, "height": HEIGHT, "fill": [0, 0, 0, 0]})
    wd.wait_for_fit(wire)
    wire.call("tool.stroke", {
        "points": [[8.0 + i, 12.0 + i * 0.6] for i in range(WIDTH - 16)],
        "tool": "brush", "size": 14.0, "hardness": 0.4, "opacity": 0.75, "flow": 0.9,
        "spacing": 0.3, "color": [230, 40, 90, 255]})


def ours(wire: wd.Wire) -> np.ndarray:
    """What the app believes its composite is, as RGBA in int32."""
    got = wire.call("image.get", {"base64": True})
    with Image.open(io.BytesIO(base64.b64decode(got["png_base64"]))) as image:
        return np.asarray(image.convert("RGBA"), dtype=np.int32)


def flatten_over_white(pixels: np.ndarray) -> np.ndarray:
    """What a format with no alpha channel shows of `pixels`, in int32: `255 * 255` does not fit in
    a short, and computing this in int16 is a bug that shows up as a mostly-black reference."""
    alpha = pixels[..., 3:4]
    rgb = (pixels[..., :3] * alpha + 255 * (255 - alpha) + 127) // 255
    return np.concatenate([rgb, np.full_like(rgb[..., :1], 255)], axis=-1)


def mean_diff(a: np.ndarray, b: np.ndarray) -> float:
    if a.shape != b.shape:
        return 1e9
    return float(np.abs(a - b).mean())


def colour_diff(got: np.ndarray, reference: np.ndarray, where: np.ndarray) -> float:
    """Mean colour difference over the pixels `where` selects; alpha is not part of it."""
    if not where.any():
        return 0.0
    return float(np.abs(got[..., :3][where] - reference[..., :3][where]).mean())


def check_we_write_they_read(wire: wd.Wire, work: Path, fails: list[str]) -> None:
    reference = ours(wire)
    for extension in ("png", "jpg", "jpeg", "webp"):
        path = work / f"exported.{extension}"
        wire.call("doc.export", {"path": str(path)})
        try:
            with Image.open(path) as image:
                image.load()
                pil_mode = image.mode
                got = np.asarray(image.convert("RGBA"), dtype=np.int32)
        except Exception as error:  # an unreadable file is the finding, whatever it raises
            fails.append(f"Pillow cannot open our {extension}: {error!r}")
            continue
        if got.shape[:2] != (HEIGHT, WIDTH):
            fails.append(f"our {extension} came back {got.shape[:2]}, expected {(HEIGHT, WIDTH)}")
            continue
        # A file that decoded to almost one colour means the pixels never crossed properly.
        if len(np.unique(got.reshape(-1, 4), axis=0)) < 16:
            fails.append(f"our {extension} decoded to almost one flat colour")

        if extension == "png":
            diff = mean_diff(got, reference)
            if diff != 0.0:
                fails.append(f"a png round trip lost {diff:.3f} per channel; PNG is lossless")
            print(f"  we wrote {extension:>4}: {path.stat().st_size:>6} B, {pil_mode:>4}, "
                  f"byte-exact against the composite")
            continue

        if extension in ("jpg", "jpeg"):
            if pil_mode != "RGB":
                fails.append(f"a jpeg came back in mode {pil_mode!r}; JPEG has no alpha to lose")
            diff = mean_diff(got[..., :3], flatten_over_white(reference)[..., :3])
            if diff > MAX_LOSSY_MEAN_DIFF:
                fails.append(f"our jpeg differs from the white-flattened composite by {diff:.1f} "
                             f"per channel")
            print(f"  we wrote {extension:>4}: {path.stat().st_size:>6} B, {pil_mode:>4}, "
                  f"{diff:5.2f} against the composite flattened over white")
            continue

        # WebP is lossy and stores premultiplied samples internally, so the colour of a nearly
        # transparent pixel is divided up and rounded away: checking it would fail on correct
        # behaviour. Where a pixel is opaque, colour has to hold, and alpha holds everywhere.
        opaque = reference[..., 3] >= 250
        diff = colour_diff(got, reference, opaque)
        alpha_diff = float(np.abs(got[..., 3] - reference[..., 3]).mean())
        if diff > MAX_LOSSY_MEAN_DIFF:
            fails.append(f"our webp differs from the composite by {diff:.1f} per channel where the "
                         f"pixel is opaque")
        if alpha_diff > MAX_LOSSY_MEAN_DIFF:
            fails.append(f"our webp differs from the composite by {alpha_diff:.1f} in alpha")
        print(f"  we wrote {extension:>4}: {path.stat().st_size:>6} B, {pil_mode:>4}, "
              f"{diff:5.2f} colour where opaque, {alpha_diff:5.2f} alpha")


def write_their_files(work: Path) -> tuple[np.ndarray, np.ndarray]:
    """A deterministic RGBA pattern, written by Pillow. Returns (lossless PNG bytes, the pattern)."""
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]  # mgrid yields the row grid first
    pattern = np.stack([
        (x * 3 + y) % 256,
        (y * 5) % 256,
        (x * 2 + y * 2) % 256,
        np.where((x + y) % 7 < 3, 0, 200 + ((x * 3 + y) % 56)),
    ], axis=-1).astype(np.uint8)
    Image.fromarray(pattern, "RGBA").save(work / "theirs.png", "PNG")
    Image.fromarray(pattern[..., :3], "RGB").save(work / "theirs.jpg", "JPEG", quality=92)
    Image.fromarray(pattern, "RGBA").save(work / "theirs.webp", "WEBP", quality=92)
    return pattern.astype(np.int32), pattern[..., 3] < 8


def check_they_write_we_read(wire: wd.Wire, work: Path, fails: list[str]) -> None:
    expected, transparent = write_their_files(work)

    for name in ("theirs.png", "theirs.jpg", "theirs.webp"):
        wire.call("doc.open", {"path": str(work / name)})
        got = ours(wire)
        if got.shape[:2] != (HEIGHT, WIDTH):
            fails.append(f"{name}: opened as {got.shape[:2]}, expected {(HEIGHT, WIDTH)}")
            continue
        if name.endswith(".png"):
            diff = mean_diff(got, expected)
            if diff != 0.0:
                fails.append(f"{name}: a png we opened differs from Pillow's own bytes by {diff}")
            print(f"  they wrote {name:>10}: read byte-exact")
        elif name.endswith(".jpg"):
            if not bool((got[..., 3] == 255).all()):
                fails.append(f"{name}: a jpeg came back with transparent pixels")
            diff = colour_diff(got, expected, np.ones(expected.shape[:2], dtype=bool))
            if diff > MAX_LOSSY_MEAN_DIFF:
                fails.append(f"{name}: we read it {diff:.1f} per channel away from Pillow")
            print(f"  they wrote {name:>10}: {diff:5.2f} per channel from Pillow's pixels, "
                  f"opaque throughout")
        else:
            diff = colour_diff(got, expected, expected[..., 3] >= 250)
            if diff > MAX_LOSSY_MEAN_DIFF:
                fails.append(f"{name}: we read its colour {diff:.1f} per channel away from Pillow")
            # Straight alpha, not premultiplied: a pixel that was fully transparent and coloured
            # stays transparent instead of turning into opaque black.
            if transparent.any():
                mean_hidden = float(got[..., 3][transparent].mean())
                if mean_hidden > MAX_TRANSPARENT_ALPHA:
                    fails.append(
                        f"{name}: pixels that were fully transparent came back at "
                        f"{mean_hidden:.1f}/255 alpha - that is premultiplied, not straight")
            print(f"  they wrote {name:>10}: {diff:5.2f} per channel where opaque, "
                  f"transparent pixels stayed at {float(got[..., 3][transparent].mean()):.1f}")

    # The name is a hint, not a contract: the bytes decide what a file is.
    renamed = work / "theirs-renamed.bin"
    shutil.copyfile(work / "theirs.png", renamed)
    wire.call("doc.open", {"path": str(renamed)})
    if ours(wire).shape[:2] != (HEIGHT, WIDTH):
        fails.append("a PNG named .bin did not open; reading decides by the bytes")
    else:
        print("  a png renamed .bin : opened by content")


def run(args: argparse.Namespace) -> int:
    runtime = Path(tempfile.mkdtemp(prefix="noise-interop-"))
    work = Path(tempfile.mkdtemp(prefix="noise-interop-files-"))
    env = dict(os.environ)
    env["NOISE_RUNTIME_DIR"] = str(runtime)
    env.pop("NOISE_CONFIG_DIR", None)
    fails: list[str] = []
    with (runtime / "app-stderr.log").open("w") as app_stderr:
        proc = wd.start_app(args, runtime, 700, 500, None, app_stderr)
        wire = None
        try:
            wire = wd.Wire(wd.wire_port(runtime, proc, args.startup_timeout))
            paint(wire)
            print(f"flat image interop, {WIDTH}x{HEIGHT} with a soft stroke:")
            check_we_write_they_read(wire, work, fails)
            check_they_write_we_read(wire, work, fails)
        except Exception as error:
            fails.append(f"probe could not run: {error!r}")
        finally:
            shutdown(proc, wire)
    for failure in fails:
        print(f"FAIL: {failure}")
    print(f"interop: {len(fails)} failure(s)")
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(runtime, ignore_errors=True)
    return 1 if fails else 0


def shutdown(proc: subprocess.Popen, wire: wd.Wire | None) -> None:
    if wire is not None:
        try:
            wire.call("app.quit", {})
        except Exception:
            pass
        wire.close()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=Path("build/noise-edit"))
    parser.add_argument("--startup-timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not Path(args.bin).exists():
        print(f"SKIP: {args.bin} does not exist")
        return 0
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0
    if MISSING_IMAGE_STACK:
        print(f"SKIP: no numpy/Pillow to read pixels with: {MISSING_IMAGE_STACK}")
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
