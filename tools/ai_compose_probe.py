#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Look at a composite on the running app: does the object disappear, and is the rest untouched?

The fixture is a scene with a soft-edged red disc in it. The "answer" is the scene without the disc, at
exactly the crop's dimensions, so the resampler is a pass-through and what is left on screen is the
feather, the ring correction and the layer clip. The pixel-lock claim is then checked on the live
document against the coverage the app itself reported, and a deliberately mis-coloured answer shows the
ring correction pulling the patch back towards the surface around it.
"""
import argparse
import os
import pathlib
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

W, H = 480, 320
DISC = (300, 140, 48)  # cx, cy, r
CAST = np.array([0.82, 1.0, 1.18])  # a cool cast, so a correction has something to correct


def scene(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """The background: linear in both axes, with a grain. Linear on purpose - a mean/std transfer matches a
    patch to the statistics of the ring around it, and a ring whose mean is a different *surface* from the
    patch's (a horizon across the crop, say) makes that transfer wrong by construction, which would test the
    fixture rather than the compositor. The grain is what stops a flat field matching by accident."""
    u, v = 1 - x / W, 1 - y / H
    base = np.stack([50 + 55 * u + 35 * v, 65 + 45 * u + 55 * v, 95 + 30 * u + 75 * v], -1)
    grain = np.random.default_rng(7).normal(0.0, 4.0, base.shape[:2])[..., None]
    return np.clip(base + grain, 0, 255)


def disc_weight(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    cx, cy, r = DISC
    return np.clip((r - np.sqrt((x - cx) ** 2 + (y - cy) ** 2)) / 3.0, 0.0, 1.0)


def inside(shape, cx: int, cy: int, r: int) -> np.ndarray:
    y, x = np.mgrid[0:shape[0], 0:shape[1]]
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def worst_channel_error(image: np.ndarray, truth: np.ndarray, mask: np.ndarray) -> float:
    """How far the mean colour of `mask` sits from the truth, in the worst channel."""
    got = image[mask].mean(0)
    want = truth[mask].mean(0)
    return float(np.abs(got - want).max())


def depths(wire: wd.Wire) -> tuple[int, int]:
    history = wire.call("app.state")["document"]["history"]
    return int(history["undo_depth"]), int(history["redo_depth"])


def layer_checksum(wire: wd.Wire) -> str:
    """The active layer's own digest, so 'the layer has pixels' is a fact and not an inference from
    the composite, which would also change if the wrong layer had been written."""
    rows = wire.call("layer.list")["layers"]
    active = next(r for r in rows if r.get("active"))
    return active["checksum"]


def export(wire: wd.Wire, path: pathlib.Path) -> np.ndarray:
    wire.call("doc.export", {"path": str(path)})
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def lock_claims(diff: np.ndarray, reported: int, bounds, label: str) -> int:
    """Two claims, both of which the app can get wrong independently: the export changes only where the
    report says pixels were written, and nowhere else. Not equality: the report counts what was written
    onto the answer's own layer, and a pixel the model reproduced exactly is written without being a
    visible change once the layer is flattened down for the export."""
    failed = 0
    changed = int(diff.sum())
    outside = np.zeros_like(diff)
    outside[bounds[1]:bounds[1] + bounds[3], bounds[0]:bounds[0] + bounds[2]] = True
    leaked = int((diff & ~outside).sum())
    print(f"{label}: {changed} pixels differ (reported {reported}), "
          f"{leaked} of them outside the reported coverage")
    if not 0 < changed <= reported:
        print(f"FAIL {label}: {changed} pixels moved, which is not a subset of the {reported} written")
        failed += 1
    if leaked:
        print(f"FAIL {label}: pixel-lock - bytes changed where the coverage says nothing was pasted")
        failed += 1
    return failed


def skip_reason(bin_path: pathlib.Path) -> str | None:
    if not bin_path.exists():
        return f"{bin_path} does not exist"
    if reason := wd.display_skip():
        return reason
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        return "numpy/Pillow are not installed, so the fixture cannot be built"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=pathlib.Path("build/noise-edit"))
    args = parser.parse_args()
    reason = skip_reason(pathlib.Path(args.bin))
    if reason:
        print(f"SKIP: {reason}")
        return 0
    args.bin = pathlib.Path(args.bin)
    fail = 0
    grid_y, grid_x = np.mgrid[0:H, 0:W].astype(float)
    back = scene(grid_x, grid_y)
    red = np.array([232, 24, 36], dtype=float)
    with_disc = np.clip(back * (1 - disc_weight(grid_x, grid_y)[..., None]) + red * disc_weight(grid_x, grid_y)[..., None],
                        0, 255).astype(np.uint8)

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="ai-compose-"))
    doc_png = tmp / "scene.png"
    Image.fromarray(with_disc, "RGB").save(doc_png)

    runtime = tmp / "runtime"
    runtime.mkdir()
    stderr = open(tmp / "app.stderr", "wb")
    proc = wd.start_app(args, runtime, 1000, 700, doc_png, stderr)
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 40.0))
        wd.wait_for_fit(wire)
        cx, cy, r = DISC
        margin = 22
        wire.call("select.rect", {"rect": [cx - r - margin, cy - r - margin,
                                           2 * (r + margin), 2 * (r + margin)]})
        original = export(wire, tmp / "before.png")
        before_shot = wd.capture_canvas(wire, "before")

        # An answer at a size nobody asked for is coped with, not refused - and it says so. The reply also
        # carries the plan, which is how the probe learns what size the answer should have been.
        probe = tmp / "probe.png"
        Image.fromarray(np.zeros((32, 32, 3), dtype=np.uint8), "RGB").save(probe)
        small = wire.call("ai.compose", {"image": str(probe)})
        crop = small["region"]["crop"]
        print(f"a 32x32 answer: composed={small['composite']['composed']} "
              f"answered_as_requested={small['composite']['answered_as_requested']} "
              f"-> crop={crop} requested={small['region']['requested']}")
        if small["composite"]["answered_as_requested"]:
            print("FAIL: a 32x32 answer to a larger crop claimed the size asked for")
            fail += 1

        patch = back[crop[1]:crop[1] + crop[3], crop[0]:crop[0] + crop[2]].astype(np.uint8)
        patch_png = tmp / "answer.png"
        Image.fromarray(patch, "RGB").save(patch_png)

        preview = wire.call("ai.compose", {"image": str(patch_png)})
        comp = preview["composite"]
        print(f"preview: feather={comp['feather_px']}px changed={comp['pixels_changed']} "
              f"coverage={comp['coverage_pixels']} px / area {comp['coverage_area']} "
              f"answered_as_requested={comp['answered_as_requested']} "
              f"colour_corrected={comp['colour_corrected']}")
        print(f"preview png: {preview.get('preview')}")
        if preview["applied"]:
            print("FAIL: a preview applied itself")
            fail += 1
        preview_shot = wd.capture_canvas(wire, "preview")
        if not np.array_equal(export(wire, tmp / "preview-check.png"), original):
            print("FAIL: the preview changed the document")
            fail += 1

        applied = wire.call("ai.compose", {"image": str(patch_png), "apply": True})
        print(f"history after apply: undo/redo={depths(wire)}")
        after_shot = wd.capture_canvas(wire, "after")
        landed = export(wire, tmp / "after.png")
        fail += lock_claims((landed != original).any(-1), applied["composite"]["pixels_changed"],
                            applied["composite"]["coverage_bounds"], "exact answer")

        core = inside((H, W), cx, cy, r - 10)
        print(f"disc centre: before={original[core].mean(0).round(1)} "
              f"after={landed[core].mean(0).round(1)} "
              f"background={back[core].mean(0).round(1)}")
        if original[core].mean(0)[0] - original[core].mean(0)[2] < 40:
            print("FAIL: the fixture's disc is not red; nothing here proves removal")
            fail += 1
        gap = worst_channel_error(landed, back, core)
        print(f"worst channel error against the true background: {gap:.2f}")
        if gap > 12:
            print("FAIL: the object is still there, or the answer landed somewhere else")
            fail += 1

        # The ring correction: an answer that comes back the wrong colour, applied with the policy off and
        # then on, against the same pristine ring.
        cast_png = tmp / "answer-cast.png"
        Image.fromarray(np.clip(patch * CAST, 0, 255).astype(np.uint8), "RGB").save(cast_png)
        wire.call("edit.undo")
        print(f"history after undo: undo/redo={depths(wire)}")
        if not np.array_equal(export(wire, tmp / "undone.png"), original):
            print("FAIL: undo did not put every byte back, so the A/B below compares nothing")
            fail += 1

        off = wire.call("ai.compose",
                        {"image": str(cast_png), "colour_match": False, "apply": True})
        off_error = worst_channel_error(export(wire, tmp / "cast-off.png"), back, core)
        print(f"colour_match off: corrected={off['composite']['colour_corrected']} "
              f"changed={off['composite']['pixels_changed']} error={off_error:.2f}")
        wire.call("edit.undo")

        on = wire.call("ai.compose",
                       {"image": str(cast_png), "colour_match": True, "apply": True})
        corrected = export(wire, tmp / "cast-on.png")
        on_error = worst_channel_error(corrected, back, core)
        print(f"colour_match on:  corrected={on['composite']['colour_corrected']} "
              f"changed={on['composite']['pixels_changed']} error={on_error:.2f}")
        fail += lock_claims((corrected != original).any(-1),
                            on["composite"]["pixels_changed"], on["composite"]["coverage_bounds"],
                            "corrected answer")
        if not on["composite"]["colour_corrected"] or off["composite"]["colour_corrected"]:
            print("FAIL: the correction did not follow the policy")
            fail += 1
        if on_error > off_error * 0.4:
            print(f"FAIL: the ring correction left the patch's mean {off_error:.1f} -> {on_error:.1f} "
                  "from the truth, which is not a correction")
            fail += 1

        # The other thing an answer can be: pixels the document never had. There is no option for where
        # they go - every answer arrives on its own layer - so the claim is about that layer, and about
        # the one undo that takes the layer away with it.
        wire.call("layer.add")  # an empty layer on top: nothing there to receive anything
        blank = layer_checksum(wire)
        layers_before = len(wire.call("layer.list")["layers"])
        doc_before = export(wire, tmp / "gen-before.png")
        wire.call("select.rect", {"rect": [cx - 40, cy - 40, 80, 80]})

        landed = wire.call("ai.compose", {"image": str(patch_png), "apply": True})
        layers_after = len(wire.call("layer.list")["layers"])
        print(f"answer over an empty layer: layer={landed.get('layer')} "
              f"name={landed.get('layer_name')!r} changed={landed['composite']['pixels_changed']}")
        if layers_after != layers_before + 1:
            print("FAIL: the answer landed on a layer that already existed")
            fail += 1
        if landed.get("layer") != layers_after - 1:
            print("FAIL: the answer's layer is not the one directly above the source")
            fail += 1
        if not landed["composite"]["pixels_changed"]:
            print("FAIL: an answer wrote nothing at all")
            fail += 1
        if layer_checksum(wire) == blank:
            print("FAIL: the top layer still holds no pixels after an answer")
            fail += 1
        if np.array_equal(export(wire, tmp / "gen-after.png"), doc_before):
            print("FAIL: the document looks exactly as it did before the answer")
            fail += 1

        wire.call("edit.undo")
        if len(wire.call("layer.list")["layers"]) != layers_before:
            print("FAIL: undo left the answer's layer behind")
            fail += 1
        if not np.array_equal(export(wire, tmp / "gen-undone.png"), doc_before):
            print("FAIL: undo left the answer's pixels behind")
            fail += 1

        # Forced: this document is scratch, and holding a quit that discards it is the guard working
        # as designed - a probe that means to leave says so.
        wire.call("app.quit", {"force": True})
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        stderr.close()
    for shot in (before_shot, preview_shot, after_shot):
        print(f"capture: {shot['path']}")
    print("FAILURES" if fail else "all claims held", f"({tmp})")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
