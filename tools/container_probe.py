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
FILL = (7, 14, 21, 255)
WIDTH = 40
HEIGHT = 24
MEMBERS = ["mimetype", "manifest.json", "layers/0/layer.png", "mergedimage.png"]
BIG = 2048
# A regression guard, not a benchmark: a save re-encodes two PNGs and a rename.
MAX_SAVE_SECONDS = 20.0


def check_archive(path: Path, fails: list[str]) -> None:
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
        if merged != layer:
            fails.append("mergedimage.png and layers/0/layer.png differ with one layer present")

    try:
        doc = json.loads(manifest)
    except ValueError as exc:
        fails.append(f"manifest.json is not JSON: {exc}")
        return
    if doc.get("format") != 1:
        fails.append(f"manifest format is {doc.get('format')!r}, expected 1")
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
        "format": 1,
        "size": [WIDTH, HEIGHT],
        "layers": [{"name": layer_name, "src": "layers/0/layer.png"}],
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
        if got.get("layer") != "Sky sketch":
            fails.append(f"{project.name}: layer is {got.get('layer')!r}, expected 'Sky sketch'")
        if [got.get("width"), got.get("height")] != [WIDTH, HEIGHT]:
            fails.append(f"{project.name}: size is {got.get('width')}x{got.get('height')}")
        checksums.append(got["checksum"])
    if len(checksums) == 2 and checksums[0] != checksums[1]:
        fails.append("stored and deflated members of one payload opened differently")

    # An archive that came from elsewhere must also be writable again without our help: this
    # rewrites it with our writer, which `zipfile` then reads.
    wire.call("doc.save", {"path": str(work / "from_foreign.noise")})
    check_archive(work / "from_foreign.noise", fails)


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
