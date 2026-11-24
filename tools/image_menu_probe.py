#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive image geometry operations from the Image menu in the running editor."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import sys
import tempfile

from PIL import Image, ImageChops

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402

WIDTH, HEIGHT = 1280, 800
IMAGE_MENU_X = 215
IMAGE_SIZE_Y = 32
CANVAS_SIZE_Y = 51
CROP_Y = 70
ROTATE_Y = 109
ZOOM_SELECTOR = (1230, 10)
ZOOM_FIT_Y = 31
ZOOM_100_Y = 63
ZOOM_200_Y = 96
KEY_CTRL_L = 0xFFE3
KEY_ALT_L = 0xFFE9
KEY_RETURN = 0xFF0D


class Probe:
    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> None:
        print(("  ok   " if ok else "  FAIL ") + claim +
              (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1


def click(inj: wi.Injector, x: int, y: int) -> None:
    inj.move(x, y)
    inj.button(1, True)
    inj.button(1, False)


def replace_integer(inj: wi.Injector, x: int, y: int, value: int) -> None:
    click(inj, x, y)
    inj.modified_key(ord("a"), KEY_CTRL_L)
    for digit in str(value):
        inj.key(ord(digit))
    inj.key(KEY_RETURN)


def open_image_item(inj: wi.Injector, item_y: int) -> None:
    click(inj, IMAGE_MENU_X, 10)
    click(inj, 245, item_y)


def dimensions(wire: wd.Wire) -> tuple[int, int]:
    state = wire.call("app.state")
    return state["document"]["width"], state["document"]["height"]


def view_scale(wire: wd.Wire) -> float:
    return float(wire.call("app.state")["view"]["scale"])


def choose_zoom(inj: wi.Injector, wire: wd.Wire, item_y: int) -> None:
    click(inj, *ZOOM_SELECTOR)
    wd.settle(wire)
    click(inj, ZOOM_SELECTOR[0], item_y)
    wd.settle(wire, 8)


def capture_window(wire: wd.Wire, name: str) -> Image.Image:
    result = wire.call("screenshot", {"name": name})
    return Image.open(result["path"]).convert("RGB").copy()


def exercise_shortcut(wire: wd.Wire, inj: wi.Injector, probe: Probe,
                      key: int, title: str, cancel: tuple[int, int],
                      edit: tuple[int, int] | None = None) -> None:
    inj.move(640, HEIGHT - 20)
    wd.settle(wire)
    baseline = capture_window(wire, f"baseline-{title.lower().replace(' ', '-')}")
    before = dimensions(wire)
    inj.modified_key(key, KEY_CTRL_L, KEY_ALT_L)
    wd.settle(wire, 4)
    first = capture_window(wire, f"shortcut-{title.lower().replace(' ', '-')}")
    probe.check(ImageChops.difference(baseline, first).getbbox() is not None,
                f"Ctrl+Alt opens {title} through the command table")
    if edit is not None:
        replace_integer(inj, *edit, 333)
        inj.move(640, HEIGHT - 20)
        wd.settle(wire)
        edited = capture_window(wire, f"edited-{title.lower().replace(' ', '-')}")
        field = (edit[0] - 24, edit[1] - 12, edit[0] + 24, edit[1] + 12)
        probe.check(ImageChops.difference(first.crop(field), edited.crop(field)).getbbox()
                    is not None, f"{title} accepts a dimension edit")
    click(inj, *cancel)
    inj.move(640, HEIGHT - 20)
    wd.settle(wire, 4)
    closed = capture_window(wire, f"cancelled-{title.lower().replace(' ', '-')}")
    probe.check(dimensions(wire) == before,
                f"Cancel leaves the document unchanged in {title}", str(dimensions(wire)))
    probe.check(ImageChops.difference(baseline, closed).getbbox() is None,
                f"Cancel closes {title}")
    if edit is None:
        return
    inj.modified_key(key, KEY_CTRL_L, KEY_ALT_L)
    wd.settle(wire, 4)
    reopened = capture_window(wire, f"reopened-{title.lower().replace(' ', '-')}")
    probe.check(ImageChops.difference(first, reopened).getbbox() is None,
                f"reopening {title} resets values to the current document")
    click(inj, *cancel)
    inj.move(640, HEIGHT - 20)
    wd.settle(wire, 3)
    closed = capture_window(wire, f"reclosed-{title.lower().replace(' ', '-')}")
    probe.check(ImageChops.difference(baseline, closed).getbbox() is None,
                f"Cancel closes reopened {title}")


def run(wire: wd.Wire, inj: wi.Injector, probe: Probe) -> None:
    wire.call("doc.new", {"width": 200, "height": 120})
    wd.settle(wire, 8)

    shortcut_entries = {entry["id"]: entry["shortcuts"]
                        for entry in wire.call("command.list")["commands"]}
    probe.check("Ctrl+Alt+I" in shortcut_entries.get("ui.image_size", []),
                "Image Size shortcut is registered in the command table")
    probe.check("Ctrl+Alt+C" in shortcut_entries.get("ui.canvas_size", []),
                "Canvas Size shortcut is registered in the command table")
    probe.check("Ctrl+Alt+U" in shortcut_entries.get("ui.upscale", []),
                "Upscale shortcut is registered in the command table")
    exercise_shortcut(wire, inj, probe, ord("i"), "Image Size", (548, 495), (620, 328))
    exercise_shortcut(wire, inj, probe, ord("c"), "Canvas Size", (545, 531), (630, 312))
    exercise_shortcut(wire, inj, probe, ord("u"), "Upscale", (510, 550))

    fit_scale = view_scale(wire)
    choose_zoom(inj, wire, ZOOM_100_Y)
    probe.check(abs(view_scale(wire) - 1.0) < 0.005,
                "the top-bar 100% selector updates viewport zoom", str(view_scale(wire)))
    choose_zoom(inj, wire, ZOOM_200_Y)
    probe.check(abs(view_scale(wire) - 2.0) < 0.005,
                "the top-bar 200% selector updates viewport zoom", str(view_scale(wire)))
    inj.move(500, 400)
    inj.wheel(1)
    wd.settle(wire, 6)
    wheel_scale = view_scale(wire)
    probe.check(wheel_scale > 2.0, "mouse-wheel zoom still updates viewport state",
                str(wheel_scale))
    choose_zoom(inj, wire, ZOOM_FIT_Y)
    probe.check(abs(view_scale(wire) - fit_scale) < 0.005,
                "the top-bar Fit selector restores the document fit", str(view_scale(wire)))

    open_image_item(inj, IMAGE_SIZE_Y)
    wd.settle(wire)
    replace_integer(inj, 620, 328, 320)
    wd.settle(wire)
    click(inj, 620, 460)
    wd.settle(wire, 6)
    resized = dimensions(wire)
    probe.check(resized == (320, 192),
                "Image Size applies Lanczos3 from the menu and keeps the locked ratio",
                str(resized))
    wire.call("edit.undo")
    wd.settle(wire, 4)
    probe.check(dimensions(wire) == (200, 120),
                "one Undo restores the pre-resize dimensions", str(dimensions(wire)))

    open_image_item(inj, CANVAS_SIZE_Y)
    wd.settle(wire)
    replace_integer(inj, 630, 312, 260)
    click(inj, 612, 393)
    click(inj, 565, 478)
    wd.settle(wire)
    inj.move(500, 600)
    wd.settle(wire, 6)
    canvas_state = wire.call("app.state")["document"]
    probe.check((canvas_state["width"], canvas_state["height"]) == (260, 120),
                "Canvas Size applies its dimensions from the menu",
                f"{canvas_state['width']} × {canvas_state['height']}")
    selection = canvas_state["selection"]
    probe.check(selection["active"] and selection["bounds"] == [200, 0, 60, 120],
                "the left anchor selects only the newly added area for AI Fill",
                str(selection))
    wire.call("edit.undo")
    wd.settle(wire, 4)

    wire.call("select.rect", {"rect": [20, 10, 80, 60]})
    wd.settle(wire)
    open_image_item(inj, CROP_Y)
    wd.settle(wire)
    click(inj, 696, 451)
    wd.settle(wire, 6)
    cropped = dimensions(wire)
    probe.check(cropped == (80, 60), "Crop to Selection applies from the Crop dialog",
                str(cropped))
    wire.call("edit.undo")
    wd.settle(wire, 4)
    probe.check(dimensions(wire) == (200, 120),
                "Undo restores the canvas after cropping", str(dimensions(wire)))

    open_image_item(inj, ROTATE_Y)
    wd.settle(wire)
    click(inj, 400, 130)
    wd.settle(wire, 6)
    rotated = dimensions(wire)
    probe.check(rotated == (120, 200), "Rotate applies clockwise from its Image submenu",
                str(rotated))
    wire.call("edit.undo")
    wd.settle(wire, 4)
    probe.check(dimensions(wire) == (200, 120),
                "Undo restores the canvas after rotation", str(dimensions(wire)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not args.bin.is_file():
        print(f"SKIP: no binary at {args.bin}")
        return 0
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0

    temp = pathlib.Path(tempfile.mkdtemp(prefix="image-menu-"))
    runtime = temp / "runtime"
    stderr_path = temp / "app.stderr"
    display = wd.ensure_display(WIDTH, HEIGHT)
    probe = Probe()
    proc = wire = inj = None
    try:
        with stderr_path.open("wb") as stderr:
            app_args = argparse.Namespace(bin=args.bin, theme="dark")
            proc = wd.start_app(app_args, runtime, WIDTH, HEIGHT, None, stderr,
                                {"THENOISE_URL": None, "THENOISE_API_KEY": None})
            wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
            inj = wi.Injector(display)
            inj.focus_app_window(WIDTH, HEIGHT)
            print("Image menu: resize, canvas extension, crop, rotate and Undo")
            run(wire, inj, probe)
    except (wd.WireError, OSError, TimeoutError) as exc:
        print(f"FAIL: {exc}")
        probe.failed += 1
    finally:
        if inj is not None:
            inj.close()
        wd.shutdown(proc, wire)
        shutil.rmtree(temp, ignore_errors=True)
    print("FAIL" if probe.failed else "ok")
    return 1 if probe.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
