#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The container, read by someone else's ZIP implementation.

Our own reader accepting our own writer proves very little: the end-of-directory record is the
only thing that says where the central directory ends, and a size that is a few bytes off keeps
one reader happy while another walks out of position. So this writes a project with the app and
reads it back with Python's `zipfile`, which knows nothing about us.

    python3 tools/container_probe.py --bin build/noise-edit

Skips loudly (exit 0, "SKIP:" on stdout) when it cannot render.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

MIMETYPE = b"application/x-noise+zip"
# One format is written and read, and a file of another number is refused by that number. Format 3
# added the selection plane; format 4 added a layer's own mask plane, named by that layer's row.
# Neither can be inferred from an older file: a missing row means "no selection" and "no mask", which
# are states, not omissions.
FORMAT = 4
FILL = (7, 14, 21, 255)
WIDTH = 40
HEIGHT = 24
MEMBERS = ["mimetype", "manifest.json", "layers/0/layer.png", "mergedimage.png"]
BIG = 2048
LAYERS = 3
# A regression guard, not a benchmark: a save re-encodes two PNGs and a rename.
MAX_SAVE_SECONDS = 20.0


def check_archive(path: Path, fails: list[str], expect_visible: bool = True,
                  expect_opacity: float = 1.0) -> None:
    try:
        archive = zipfile.ZipFile(path)
    except zipfile.BadZipFile as exc:
        # The whole point of the test: another reader refuses the file we wrote.
        fails.append(f"zipfile cannot open the project: {exc}")
        return
    with archive:
        bad = archive.testzip()
        if bad is not None:
            fails.append(f"CRC check failed for member '{bad}'")
        names = archive.namelist()
        if names != MEMBERS:
            fails.append(f"members are {names}, expected {MEMBERS}")
        info = archive.infolist()[0]
        # The format promises a sniffable first member: uncompressed, at offset 0.
        if info.filename != "mimetype":
            fails.append(f"first member is '{info.filename}', expected 'mimetype'")
        elif info.compress_type != zipfile.ZIP_STORED:
            fails.append("mimetype is compressed, so a sniffer cannot read it at a fixed offset")
        if archive.read("mimetype") != MIMETYPE:
            fails.append(f"mimetype is {archive.read('mimetype')!r}, expected {MIMETYPE!r}")

        manifest = archive.read("manifest.json")
        layer = archive.read("layers/0/layer.png")
        merged = archive.read("mergedimage.png")
        # One layer at full strength flattens to itself; faded or hidden, it must not, which is
        # the same identity the compositor promises.
        if expect_visible and expect_opacity == 1.0 and merged != layer:
            fails.append("mergedimage.png and layers/0/layer.png differ with one full-strength layer")
        if (not expect_visible or expect_opacity != 1.0) and merged == layer:
            fails.append(f"mergedimage.png equals the layer although the manifest says "
                         f"visible={expect_visible} opacity={expect_opacity}")

    try:
        doc = json.loads(manifest)
    except ValueError as exc:
        fails.append(f"manifest.json is not JSON: {exc}")
        return
    if doc.get("format") != FORMAT:
        fails.append(f"manifest format is {doc.get('format')!r}, expected {FORMAT}")
    if doc.get("size") != [WIDTH, HEIGHT]:
        fails.append(f"manifest size is {doc.get('size')!r}, expected [{WIDTH}, {HEIGHT}]")
    layers = doc.get("layers")
    if not isinstance(layers, list) or len(layers) != 1:
        fails.append(f"manifest layers is {layers!r}, expected a one-element list")
        return
    if layers[0].get("src") != "layers/0/layer.png":
        fails.append(f"layer src is {layers[0].get('src')!r}, expected 'layers/0/layer.png'")
    if not layers[0].get("name"):
        fails.append("layer has no name")
    # The format requires the layer's own state, so a reader that only looks at the PNGs still
    # learns what the file says the layer looks like.
    if layers[0].get("visible") is not expect_visible:
        fails.append(f"layer 'visible' is {layers[0].get('visible')!r}, "
                     f"expected {str(expect_visible).lower()}")
    if layers[0].get("opacity") != expect_opacity:
        fails.append(f"layer 'opacity' is {layers[0].get('opacity')!r}, expected {expect_opacity}")
    if layers[0].get("mode") != "normal":
        fails.append(f"layer 'mode' is {layers[0].get('mode')!r}, expected 'normal'")

    from PIL import Image

    with Image.open(io.BytesIO(layer)) as decoded:
        pixels = decoded.convert("RGBA").load()
        if decoded.size != (WIDTH, HEIGHT):
            fails.append(f"layer PNG is {decoded.size}, expected ({WIDTH}, {HEIGHT})")
        for x in (0, WIDTH - 1):
            for y in (0, HEIGHT - 1):
                if pixels[x, y] != FILL:
                    fails.append(f"layer pixel ({x},{y}) is {pixels[x, y]}, expected {FILL}")
                    return


def write_foreign(path: Path, compression: int, layer_name: str) -> None:
    """A project written by something that is not us, so the reader is tested on foreign bytes."""
    from PIL import Image

    buffer = io.BytesIO()
    Image.new("RGBA", (WIDTH, HEIGHT), FILL).save(buffer, format="PNG")
    layer = buffer.getvalue()
    manifest = json.dumps({
        "format": FORMAT,
        "size": [WIDTH, HEIGHT],
        # A non-default opacity on purpose: a file from elsewhere that disagrees with the app's
        # defaults has to open as it says, not as what a new layer would be.
        "layers": [{"name": layer_name, "src": "layers/0/layer.png",
                    "visible": True, "opacity": 0.5, "mode": "normal"}],
    }, indent=2).encode()
    with zipfile.ZipFile(path, "w") as archive:
        # Sniffable first member, then the rest, however the writer likes it.
        archive.writestr("mimetype", MIMETYPE, compress_type=zipfile.ZIP_STORED)
        archive.writestr("manifest.json", manifest, compress_type=compression)
        archive.writestr("layers/0/layer.png", layer, compress_type=compression)


def check_foreign_project(work: Path, wire: wd.Wire, fails: list[str]) -> None:
    stored = work / "foreign_stored.noise"
    deflated = work / "foreign_deflated.noise"
    write_foreign(stored, zipfile.ZIP_STORED, "Sky sketch")
    write_foreign(deflated, zipfile.ZIP_DEFLATED, "Sky sketch")
    checksums = []
    for project in (stored, deflated):
        try:
            got = wire.call("doc.open", {"path": str(project)})
        except wd.WireError as exc:
            fails.append(f"the reader refused {project.name}: {exc}")
            continue
        names = [row["name"] for row in got.get("layers", [])]
        if names != ["Sky sketch"]:
            fails.append(f"{project.name}: layers are {names}, expected ['Sky sketch']")
        if [got.get("width"), got.get("height")] != [WIDTH, HEIGHT]:
            fails.append(f"{project.name}: size is {got.get('width')}x{got.get('height')}")
        checksums.append(got["checksum"])
    if len(checksums) == 2 and checksums[0] != checksums[1]:
        fails.append("stored and deflated members of one payload opened differently")

    # An archive that came from elsewhere must also be writable again without our help: this
    # rewrites it with our writer, which `zipfile` then reads. Its one layer is at half strength,
    # which is the point of writing it by hand.
    wire.call("doc.save", {"path": str(work / "from_foreign.noise")})
    check_archive(work / "from_foreign.noise", fails, True, 0.5)


def check_selection_archive(work: Path, wire: wd.Wire, fails: list[str]) -> None:
    """The selection plane is a plain byte array named by the manifest, so a reader that knows
    nothing about this app has to find the same rectangle in it that the app reports."""
    path = work / "selected.noise"
    rect = [4, 4, 12, 8]
    wire.call("select.rect", {"rect": rect, "mode": "replace"})
    reported = wire.call("select.get", {})
    wire.call("doc.save", {"path": str(path)})
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        src = (manifest.get("selection") or {}).get("src")
        if src is None:
            fails.append("a document with a selection saved no 'selection' row")
            return
        if src not in archive.namelist():
            fails.append(f"manifest names selection member '{src}', which the archive lacks")
            return
        plane = archive.read(src)
        if len(plane) != WIDTH * HEIGHT:
            fails.append(f"selection plane is {len(plane)} bytes, expected {WIDTH * HEIGHT}")
            return
        wanted = {(y * WIDTH + x)
                  for y in range(rect[1], rect[1] + rect[3])
                  for x in range(rect[0], rect[0] + rect[2])}
        claimed = {index for index, byte in enumerate(plane) if byte}
        if claimed != wanted:
            fails.append(f"the plane claims {len(claimed)} pixels, expected the {len(wanted)} of "
                         f"the rectangle")

    # Through the app again: the same report after a save and an open, and no leftover selection in
    # the next document.
    wire.call("doc.new", {"width": 2, "height": 2})
    opened = wire.call("doc.open", {"path": str(path)})
    if opened.get("selection") != reported:
        fails.append(f"selection after reopen is {opened.get('selection')}, expected {reported}")
    if not reported.get("active"):
        fails.append("the rectangle selected nothing, so the round trip proved nothing")
    wire.call("doc.new", {"width": 2, "height": 2})
    if wire.call("select.get", {}).get("active"):
        fails.append("a fresh document opened with a selection")


def check_layer_mask_archive(work: Path, wire: wd.Wire, fails: list[str]) -> None:
    """A layer mask is a plane the layer's own row names, so an outside reader can find which layer
    is masked and which pixels the file means to show, without knowing anything about this app."""
    path = work / "masked.noise"
    # The checks run in sequence and the one before left a 2x2 document open: the plane's size is
    # this document's, so say which document is being masked rather than assume the last one.
    wire.call("doc.new", {"width": WIDTH, "height": HEIGHT, "fill": [12, 34, 56, 255]})
    wd.wait_for_fit(wire)
    rect = [2, 1, 9, 5]
    wire.call("select.rect", {"rect": rect, "mode": "replace"})
    report = wire.call("mask.from_selection", {})
    wire.call("doc.save", {"path": str(path)})
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        rows = manifest.get("layers") or []
        with_mask = [i for i, row in enumerate(rows) if row.get("mask")]
        if with_mask != [0]:
            fails.append(f"mask rows are on layers {with_mask}, expected the active layer 0")
            return
        src = rows[0]["mask"]["src"]
        if src not in archive.namelist():
            fails.append(f"layer 0 names mask member '{src}', which the archive lacks")
            return
        plane = archive.read(src)
        if len(plane) != WIDTH * HEIGHT:
            fails.append(f"layer mask plane is {len(plane)} bytes, expected {WIDTH * HEIGHT}")
            return
        wanted = {(y * WIDTH + x)
                  for y in range(rect[1], rect[1] + rect[3])
                  for x in range(rect[0], rect[0] + rect[2])}
        claimed = {index for index, byte in enumerate(plane) if byte}
        if claimed != wanted:
            fails.append(f"the layer mask claims {len(claimed)} pixels, expected the {len(wanted)} "
                         f"of the rectangle")

    # Through the app again: the mask is on the same layer and the canvas is the same picture.
    wire.call("doc.new", {"width": 2, "height": 2})
    opened = wire.call("doc.open", {"path": str(path)})
    listed = wire.call("layer.list", {})["layers"]
    shown = (listed[0].get("mask") or {})
    if shown.get("covered") != report.get("covered"):
        fails.append(f"mask after reopen covers {shown.get('covered')}, expected "
                     f"{report.get('covered')}")
    if opened.get("dirty"):
        # Reading a mask is not an edit; a loader that set it through a path that marked the
        # document dirty would have every open look like an unsaved change.
        fails.append("opening a file with a layer mask marked the document dirty")
    wire.call("doc.new", {"width": 2, "height": 2})
    if wire.call("layer.list", {})["layers"][0].get("mask"):
        fails.append("a fresh document opened with a layer mask")


def check_hidden_layer(work: Path, wire: wd.Wire, fails: list[str]) -> None:
    """What the manifest says and what the flattened image shows must agree.

    `mergedimage.png` is read by things that never read the stack, so a `visible: false` that
    still contributes pixels is invisible here and obvious there. The opacity bounds are exact
    identities rather than a blend recomputed in Python: 0 is the same image as hiding the
    layer, and 1 is exactly the image it was before anything was hidden."""
    project = work / "hidden.noise"
    wire.call("doc.new", {"width": WIDTH, "height": HEIGHT, "fill": list(FILL)})
    wd.wait_for_fit(wire)
    wire.call("layer.add", {"name": "Top"})
    wire.call("tool.stroke", {"points": [[8.0, 8.0], [WIDTH - 8.0, HEIGHT - 8.0]], "size": 10.0,
                             "hardness": 0.5, "opacity": 1.0, "flow": 1.0, "spacing": 0.3,
                             "color": [240, 30, 30, 255]})
    if len(wire.call("layer.list")["layers"]) != 2:
        fails.append("expected two layers to hide one of")
        return
    shown = wire.call("layer.list", {})["composite_checksum"]
    wire.call("layer.set", {"index": 1, "visible": False})
    hidden = wire.call("layer.list", {})["composite_checksum"]
    if hidden == shown:
        fails.append("hiding the top layer left the composite byte-identical")
    wire.call("doc.save", {"path": str(project)})

    with zipfile.ZipFile(project) as archive:
        doc = json.loads(archive.read("manifest.json"))
        rows = doc.get("layers", [])
        marks = [row.get("visible") for row in rows]
        if marks != [True, False]:
            fails.append(f"manifest visibility is {marks}, expected [True, False]")
        merged = archive.read("mergedimage.png")
        if merged != archive.read("layers/0/layer.png"):
            fails.append("the flattened image still holds the layer the manifest marks hidden")
        if merged == archive.read("layers/1/layer.png"):
            fails.append("the base layer is the one that went missing instead")

    # Back through the same path the panel uses: the two ends of the slider are the two images
    # we can name exactly.
    wire.call("layer.set", {"index": 1, "visible": True, "opacity": 0.0})
    if wire.call("layer.list", {})["composite_checksum"] != hidden:
        fails.append("a layer at opacity 0 is not the image of a hidden layer")
    wire.call("layer.set", {"index": 1, "opacity": 1.0})
    if wire.call("layer.list", {})["composite_checksum"] != shown:
        fails.append("a layer at opacity 1 is not the image it was before it was faded")

    # And the state survives a save and an open, since a panel that lies after a reload is the
    # same bug with more steps.
    wire.call("layer.set", {"index": 1, "visible": False, "opacity": 0.25})
    saved_state = wire.call("doc.get", {})["checksum"]
    wire.call("doc.save", {})
    wire.call("doc.new", {"width": 2, "height": 2})
    reopened = wire.call("doc.open", {"path": str(project)})
    if reopened["checksum"] != saved_state:
        fails.append("visibility and opacity did not survive a save and an open")
    rows = [(row["visible"], row["opacity"]) for row in reopened["layers"]]
    if rows != [(True, 1.0), (False, 0.25)]:
        fails.append(f"reopened layer state is {rows}, expected [(True, 1.0), (False, 0.25)]")


def run(args: argparse.Namespace) -> int:
    runtime = Path(tempfile.mkdtemp(prefix="noise-container-"))
    work = Path(tempfile.mkdtemp(prefix="noise-container-files-"))

    env = dict(os.environ)
    env["NOISE_RUNTIME_DIR"] = str(runtime)
    env.pop("NOISE_CONFIG_DIR", None)
    project = work / "shot.noise"
    flat = work / "flat.png"
    fails: list[str] = []
    try:
        with (runtime / "app-stderr.log").open("w") as app_stderr:
            proc = subprocess.Popen(
                [str(args.bin), "--width=700", "--height=500", "--debug-wire=0"],
                env=env, stderr=app_stderr)
            wire = None
            try:
                port = wd.wire_port(runtime, proc, args.startup_timeout)
                wire = wd.Wire(port)
                wire.call("doc.new", {"width": WIDTH, "height": HEIGHT, "fill": list(FILL)})
                wd.wait_for_fit(wire)
                started = time.perf_counter()
                wire.call("doc.save", {"path": str(project)})
                small_save = time.perf_counter() - started
                wire.call("doc.export", {"path": str(flat)})
                check_archive(project, fails)

                # An edit has to survive a save through the same path, or the container is only
                # good for files that are never touched.
                line = [[4.0 + i, 6.0 + i * 0.5] for i in range(20)]
                wire.call("tool.stroke", {"points": line, "size": 6.0, "hardness": 0.5,
                                         "opacity": 0.8, "flow": 1.0, "spacing": 0.3,
                                         "color": [240, 30, 30, 255]})
                painted = wire.call("doc.get", {})["checksum"]
                wire.call("doc.save", {})
                wire.call("doc.new", {"width": 2, "height": 2})
                started = time.perf_counter()
                reopened = wire.call("doc.open", {"path": str(project)})
                small_open = time.perf_counter() - started
                print(f"container {WIDTH}x{HEIGHT}: save {small_save * 1000:.1f} ms, "
                      f"open {small_open * 1000:.1f} ms, "
                      f"{project.stat().st_size / 1024:.1f} KiB on disk")
                if reopened["checksum"] != painted:
                    fails.append("the saved project did not come back byte-identical")
                if reopened.get("dirty"):
                    fails.append("a project straight off disk reports itself dirty")
                if flat.stat().st_size == 0:
                    fails.append("doc.export wrote an empty file")
                check_foreign_project(work, wire, fails)
                check_hidden_layer(work, wire, fails)
                check_selection_archive(work, wire, fails)
                check_layer_mask_archive(work, wire, fails)

                # Recorded, not asserted narrowly: what a save of a full-size document costs.
                big = work / "big.noise"
                wire.call("doc.new", {"width": BIG, "height": BIG, "fill": [0, 0, 0, 0]})
                wd.wait_for_fit(wire)
                # A solid fill compresses to nothing, so paint first: the cost of a save is the
                # cost of the PNG encoder over what is actually on the layer.
                diagonal = [[float(i), float(i)] for i in range(4, BIG - 4, 6)]
                wire.call("tool.stroke", {"points": diagonal, "size": 90.0, "hardness": 0.2,
                                         "opacity": 0.9, "flow": 1.0, "spacing": 0.4,
                                         "color": [220, 60, 200, 255]})
                started = time.perf_counter()
                wire.call("doc.save", {"path": str(big)})
                save_seconds = time.perf_counter() - started
                started = time.perf_counter()
                wire.call("doc.open", {"path": str(big)})
                open_seconds = time.perf_counter() - started
                if save_seconds > MAX_SAVE_SECONDS:
                    fails.append(f"a {BIG}^2 save took {save_seconds:.2f} s, bound is "
                                 f"{MAX_SAVE_SECONDS} s")
                print(f"container {BIG}^2: save {save_seconds * 1000:.0f} ms, "
                      f"open {open_seconds * 1000:.0f} ms, "
                      f"{big.stat().st_size / 1024:.0f} KiB on disk")

                # A stack costs one PNG encode per layer plus the composite, so this is the
                # scaling that a save actually pays for.
                for index in range(1, LAYERS):
                    wire.call("layer.add", {"name": f"L{index}"})
                    wire.call("tool.stroke", {"points": diagonal, "size": 70.0, "hardness": 0.3,
                                             "opacity": 0.85, "flow": 1.0, "spacing": 0.4,
                                             "color": [60, 200, 90, 255]})
                stacked = work / "stacked.noise"
                started = time.perf_counter()
                wire.call("doc.save", {"path": str(stacked)})
                stacked_save = time.perf_counter() - started
                started = time.perf_counter()
                wire.call("doc.open", {"path": str(stacked)})
                stacked_open = time.perf_counter() - started
                if stacked_save > MAX_SAVE_SECONDS:
                    fails.append(f"a {LAYERS}-layer {BIG}^2 save took {stacked_save:.2f} s")
                print(f"container {BIG}^2 x {LAYERS} layers: save {stacked_save * 1000:.0f} ms, "
                      f"open {stacked_open * 1000:.0f} ms, "
                      f"{stacked.stat().st_size / 1024:.0f} KiB on disk")
            except wd.WireError as exc:
                fails.append(str(exc))
            finally:
                wd.shutdown(proc, wire)
        if not project.exists():
            fails.append("doc.save wrote no file")
    finally:
        shutil.rmtree(runtime, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)

    for failure in fails:
        print("FAIL", failure)
    if fails:
        return 1
    print(f"container: {len(MEMBERS)} members read back by zipfile, {WIDTH}x{HEIGHT} pixels "
          f"verified, one edit round-tripped")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=Path("build/noise-edit"))
    parser.add_argument("--startup-timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not Path(args.bin).exists():
        print(f"SKIP: {args.bin} does not exist")
        return 0
    if not (os.environ.get("DISPLAY") or shutil.which("xvfb-run")):
        print("SKIP: no DISPLAY and no xvfb-run, so nothing can render")
        return 0
    if not os.environ.get("DISPLAY"):
        print("SKIP: needs a display; run under xvfb-run")
        return 0
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: Pillow is not installed, so the layer PNG cannot be decoded")
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
