#!/usr/bin/env python3
"""Look at what the model is actually shown.

Two claims, whole-image, on the bytes that would go on the wire:

  - an in-place edit (`inpaint-replace`) uploads the region *untouched*. It used to upload a blurred one,
    which is why a Replace came back as unrelated smear: the model was shown a destroyed subject and
    told to change its eyes.
  - a reconstruction (`heal`, `inpaint-remove`, `region-fill`) destroys the selected area only, and every
    pixel outside it - the context that is the whole instruction - is byte-identical to the document.

Recorded transport: no network, no images spent.

  python3 tools/prep_proof.py [--bin build/noise-edit] [--out /tmp/prep-proof]
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

import numpy as np
from PIL import Image

import wire_drive as wd

REPO = pathlib.Path(__file__).resolve().parent.parent
# Enough structure that a blur has something to destroy: a flat area looks the same blurred, and a test
# that cannot tell those apart proves nothing.
DOC = (320, 256)
SELECT = (96, 64, 128, 128)

SAMPLE = {
    "target": "the eyes",
    "replacement": "green eyes",
    "context_desc": "the surrounding skin",
    "subject": "the face",
    "style": "anime",
    "palette": "warm",
}


def scene() -> np.ndarray:
    x, y = np.meshgrid(np.arange(DOC[1]), np.arange(DOC[0]))
    rgb = np.zeros((DOC[0], DOC[1], 3), dtype=np.uint8)
    rgb[..., 0] = (30 + x // 2) % 256
    rgb[..., 1] = (90 + y) % 256
    rgb[..., 2] = 200 - (x // 3) % 160
    # The "object": a hard-edged thing with high-contrast detail inside it.
    cy, cx = SELECT[1] + SELECT[3] // 2, SELECT[0] + SELECT[2] // 2
    disc = (x - cx) ** 2 + (y - cy) ** 2 < 48**2
    rgb[disc] = (220, 40, 60)
    for ex in (cx - 22, cx + 22):
        eye = (x - ex) ** 2 + (y - cy) ** 2 < 9**2
        rgb[eye] = (10, 10, 10)
    rgb[disc & (y % 4 == 0)] = (255, 200, 0)
    return rgb


def prep_for(wire: wd.Wire, recipe: str, ask: list[str]) -> tuple[pathlib.Path, dict]:
    vars_ = {name: SAMPLE.get(name, "the thing") for name in ask}
    # Steps and cfg given so the compile does not need a resolved model: what is uploaded does not
    # depend on them, and a tool that spends a handshake to look at pixels would be slower for no reason.
    reply = wire.call("ai.preview", {"recipe": recipe, "vars": vars_, "seed": 4021,
                                      "steps": 20, "cfg_scale": 4.0})
    return pathlib.Path(reply["prepped"]), reply


def compare(truth: np.ndarray, prepped: np.ndarray, hole: tuple[int, int, int, int]) -> dict:
    """Difference inside and outside the hole, as numbers a reader can act on."""
    mask = np.zeros(truth.shape[:2], dtype=bool)
    mask[hole[1]:hole[1] + hole[3], hole[0]:hole[0] + hole[2]] = True
    equal = np.all(prepped == truth, axis=-1)
    return {
        "context_changed": int(np.count_nonzero(~equal & ~mask)),
        "hole_changed": int(np.count_nonzero(~equal & mask)),
        "hole_total": int(np.count_nonzero(mask)),
        "context_mean_abs": float(np.abs(prepped[~mask].astype(int) -
                                         truth[~mask].astype(int)).mean()),
        "hole_mean_abs": float(np.abs(prepped[mask].astype(int) -
                                      truth[mask].astype(int)).mean()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=pathlib.Path("build/noise-edit"))
    parser.add_argument("--fixtures", default=REPO / "tests/fixtures/http")
    parser.add_argument("--out", default=pathlib.Path(tempfile.mkdtemp(prefix="prep-proof-")))
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    doc_png = out / "scene.png"
    Image.fromarray(scene(), "RGB").save(doc_png)

    runtime = out / "runtime"
    runtime.mkdir()
    stderr = open(runtime / "app.stderr", "wb")
    # Recorded, and pointed away from a live server: this tool spends nothing and must never spend.
    proc = wd.start_app(argparse.Namespace(bin=pathlib.Path(args.bin)), runtime, 900, 700, doc_png,
                        stderr, {"NOISE_AI_FIXTURES": str(args.fixtures),
                                 "THENOISE_URL": None, "THENOISE_API_KEY": None})
    failures: list[str] = []
    rows: list[str] = []
    strip: list[Image.Image] = []
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 40.0))
        wd.wait_for_fit(wire)
        wire.call("select.rect", {"rect": list(SELECT)})

        recipes = {r["id"]: r for r in wd.field(wire.call("ai.recipes"), "recipes")}

        for recipe in ("inpaint-replace", "heal", "inpaint-remove", "region-fill"):
            info = recipes.get(recipe)
            if info is None:
                failures.append(f"{recipe}: not in the recipe table")
                continue
            prepped_path, reply = prep_for(wire, recipe, info.get("ask", []))
            crop = wd.field(reply, "region")["crop"]
            requested = wd.field(reply, "region")["requested"]
            # The truth is the composite over exactly the crop the model is being shown, at the size the
            # compiler cut it, so "byte-identical" compares the same pixels.
            truth = wire.call("image.get", {"region": crop})
            truth_img = Image.open(wd.field(truth, "png_path")).convert("RGB")
            if not prepped_path.exists():
                failures.append(f"{recipe}: no upload written at {prepped_path}")
                continue
            prepped_img = Image.open(prepped_path).convert("RGB")
            if tuple(requested) != prepped_img.size:
                failures.append(f"{recipe}: upload is {prepped_img.size}, plan asked {requested}")
                continue
            if list(prepped_img.size) != [crop[2], crop[3]]:
                # Any resample between the crop and the upload makes "byte-identical" meaningless, so
                # the tool says so instead of comparing across a scale factor it cannot undo.
                rows.append(f"{recipe:16s} operation={info['operation']:12s} prep={info['prep']:9s} "
                            f"resampled crop {crop[2]}x{crop[3]} -> {requested[0]}x{requested[1]}")
                continue

            area = wd.field(truth, "region")
            hole = (SELECT[0] - area[0], SELECT[1] - area[1], SELECT[2], SELECT[3])
            stats = compare(np.asarray(truth_img), np.asarray(prepped_img), hole)
            reconstruct = info["operation"] == "reconstruct"

            if not reconstruct and stats["context_changed"] + stats["hole_changed"] != 0:
                failures.append(
                    f"{recipe}: an edit uploaded {stats['context_changed']} context and "
                    f"{stats['hole_changed']} subject pixels changed - it must upload the region as it is")
            if reconstruct:
                if stats["context_changed"] != 0:
                    failures.append(f"{recipe}: {stats['context_changed']} context pixels changed - "
                                    "the surroundings are the instruction and must go up untouched")
                if stats["hole_changed"] == 0:
                    failures.append(f"{recipe}: the hole came back identical, so nothing was destroyed "
                                    "and the model is being asked to replace what it can still see")

            rows.append(
                f"{recipe:16s} operation={info['operation']:12s} prep={info['prep']:9s} "
                f"context_changed={stats['context_changed']:6d} hole_changed="
                f"{stats['hole_changed']:6d}/{stats['hole_total']:6d} "
                f"hole_mean_abs={stats['hole_mean_abs']:6.1f}")

            strip.append(truth_img.resize((240, 240), Image.NEAREST))
            strip.append(prepped_img.resize((240, 240), Image.NEAREST))
    finally:
        wd.shutdown(proc, wire)

    if strip:
        canvas = Image.new("RGB", (240 * 2, 264 * len(strip) // 2), (24, 24, 24))
        for i, tile in enumerate(strip):
            canvas.paste(tile, ((i % 2) * 240, (i // 2) * 264))
        canvas.save(out / "upload-strip.png")

    print("\n".join(rows))
    print(f"\nstrip: {out / 'upload-strip.png'}   (left: the document as it is, right: the upload)")
    for failure in failures:
        print(f"RED {failure}")
    print("prep proof: " + ("ok" if not failures else f"{len(failures)} problem(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
