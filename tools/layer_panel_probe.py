#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does every layer own a surface in the panel, and does the picture on it belong to that layer?

Controls that float in a list belong to whichever name they sit near, so each layer now gets a bordered
card holding its visibility, its name, its own pictures, its opacity and its blend mode. The claims worth
a capture are the ones a checkbox cannot make: that the thumbnail is the layer's own content, that
changing opacity leaves that picture exactly as it was (opacity changes what the composite is made of and
no layer's bytes, so a card that re-sampled on it is spending a canvas-sized pass on every frame of a
drag), and that painting into the layer does change it.

Everything is pressed and measured at the coordinates the app reports in `app.state.layer_cards`, so the
probe follows the layout instead of remembering one.

    timeout 600 xvfb-run -a python3 tools/layer_panel_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

WIDTH, HEIGHT = 1200, 800


class Probe:
    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> bool:
        print(("  ok   " if ok else "  FAIL ") + claim + (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def click(inj: wi.Injector, x: float, y: float) -> None:
    inj.move(int(x), int(y))
    inj.button(1, True)
    inj.button(1, False)


def centre(rect: list) -> tuple[float, float]:
    return rect[0] + rect[2] / 2.0, rect[1] + rect[3] / 2.0


def grab(wire, rect: list, name: str) -> Image.Image:
    shot = wire.call("screenshot", {"region": [round(rect[0]), round(rect[1]),
                                               round(rect[2]), round(rect[3])],
                                    "name": name})
    keep = pathlib.Path("/tmp") / f"card_{name}.png"
    shutil.copy(shot["path"], keep)
    return Image.open(shot["path"]).convert('RGB')


def cards(wire) -> list[dict]:
    return wire.call("app.state")["layer_cards"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="layer-panel-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    fixture = tmp / "fixture.png"
    wd.write_fixture_png(fixture, 240, 160)
    p = Probe()
    print("one surface per layer, and its own picture on it")
    skip = wd.display_skip()
    if skip:
        print(f"SKIP: {skip}")
        return 0
    display = wd.ensure_display(WIDTH, HEIGHT)
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, fixture, open(tmp / "app.stderr", "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None},
                        start_dialog=False)
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0), timeout=120)
        inj = wi.Injector(display)
        # The window manager gives the first click to focusing, so a probe that clicks without focusing
        # first loses its first press and reads it as a dead control.
        inj.focus_app_window(WIDTH, HEIGHT)
        wd.wait_for_fit(wire)
        wire.call("layer.add", {})
        wd.settle(wire)

        listed = wire.call("layer.list")["layers"]
        got = cards(wire)
        p.check(len(got) == len(listed), "one card per layer", f"{len(got)} for {len(listed)} layers")

        # Top card first, the way the stack reads: card i names the layer at index count-1-i.
        order = [card["index"] for card in got]
        p.check(order == sorted(order, reverse=True), "the top card is the top layer", str(order))

        window = wire.call("app.state")["layers_window"]
        inside = all(
            card["box"][0] >= window[0] - 1 and card["box"][1] >= window[1] - 1 and
            card["box"][0] + card["box"][2] <= window[0] + window[2] + 1
            for card in got)
        p.check(inside, "every card sits inside the panel", str([round(v) for v in window]))
        disjoint = all(got[i]["box"][1] >= got[i - 1]["box"][1] + got[i - 1]["box"][3] - 1
                       for i in range(1, len(got)))
        p.check(disjoint, "and no two share a pixel",
                str([[round(v) for v in c["box"]] for c in got]))

        blank = grab(wire, got[0]["picture"], "thumb-blank")
        picture = grab(wire, got[1]["picture"], "thumb-picture")
        p.check(got[0]["picture"][2] > 4 and got[1]["picture"][2] > 4,
                "each card carries a picture of its own", str(got[1]["picture"]))
        p.check(ImageChops.difference(blank, picture).getbbox() is not None,
                "and the picture is the layer's content, not a placeholder",
                str(ImageChops.difference(blank, picture).getbbox()))

        # Opacity is the composite's business: the layer's own bytes have not moved, so its thumbnail
        # must not have been re-sampled - the caching rule, checked with pixels.
        wire.call("layer.set", {"index": got[1]["index"], "opacity": 0.35})
        wd.settle(wire)
        after_opacity = grab(wire, cards(wire)[1]["picture"], "thumb-after-opacity")
        p.check(ImageChops.difference(picture, after_opacity).getbbox() is None,
                "an opacity change leaves the layer's picture byte-identical",
                str(ImageChops.difference(picture, after_opacity).getbbox()))

        # Painting does move the layer, so the card has to follow.
        wire.call("layer.select", {"index": got[1]["index"]})
        wire.call("tool.stroke", {"points": [[20.0, 20.0], [200.0, 140.0]], "size": 40.0,
                                  "hardness": 1.0})
        wd.settle(wire)
        after_paint = grab(wire, cards(wire)[1]["picture"], "thumb-after-paint")
        p.check(ImageChops.difference(picture, after_paint).getbbox() is not None,
                "painting into the layer changes its picture",
                str(ImageChops.difference(picture, after_paint).getbbox()))

        # The card's checkbox is the layer's visibility, at the box the app reported for it.
        top = cards(wire)[0]
        before = wire.call("layer.list")["layers"][top["index"]]["visible"]
        click(inj, *centre(top["visibility"]))
        wd.settle(wire)
        flipped = wire.call("layer.list")["layers"][top["index"]]["visible"]
        p.check(flipped != before, "the checkbox on a card is that layer's visibility",
                f"{before} to {flipped}")
        click(inj, *centre(top["visibility"]))
        wd.settle(wire)
        p.check(wire.call("layer.list")["layers"][top["index"]]["visible"] == before,
                "and pressing it again puts it back")

        # A card's title row selects its layer, wherever the card is. The active layer has to be the
        # other one first, or the claim passes without the click doing anything.
        wire.call("layer.select", {"index": top["index"]})
        wd.settle(wire)
        p.check([card["index"] for card in cards(wire) if card["active"]] == [top["index"]],
                "the top layer is active before the click", str(top["index"]))
        bottom = cards(wire)[-1]
        title = (bottom["box"][0] + 60.0, bottom["box"][1] + 10.0)
        click(inj, *title)
        wd.settle(wire)
        active = [card["index"] for card in cards(wire) if card["active"]]
        p.check(active == [bottom["index"]], "and its title row makes that layer the active one",
                str(active))
    finally:
        wd.shutdown(proc, wire)
        shutil.rmtree(runtime, ignore_errors=True)
    print("FAIL" if p.failed else "ok")
    return 1 if p.failed else 0


sys.exit(main())
