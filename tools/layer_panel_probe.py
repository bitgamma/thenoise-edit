#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does every layer own a surface in the panel, and does the picture on it belong to that layer?

Controls that float in a list belong to whichever name they sit near, so each layer now gets a bordered
card holding its visibility, its name, its own pictures, its opacity and its blend mode. The claims worth
a capture are the ones a checkbox cannot make: that the thumbnail is the layer's own content, that
changing opacity leaves that picture exactly as it was (opacity changes what the composite is made of and
no layer's bytes, so a card that re-sampled on it is spending a canvas-sized pass on every frame of a
drag), and that painting into the layer does change it.

The stack controls are claimed the same way: an arrow moves the layer of *its* card, a dim arrow answers a
press with nothing, a merge leaves the canvas exactly as it was, and undo gives the merged layer back as a
layer rather than as an empty one with the right name. Flatten is claimed the same way too, and walked
twice, because a stack put back in the wrong order still looks like a stack.

Everything is pressed and measured at the coordinates the app reports in `app.state.layer_cards`, so the
probe follows the layout instead of remembering one.

    timeout 600 env -u DISPLAY python3 tools/layer_panel_probe.py --bin build/noise-edit
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


def inside_box(rect: list, box: list) -> bool:
    return (rect[2] > 4 and rect[3] > 4 and rect[0] >= box[0] and rect[1] >= box[1] and
            rect[0] + rect[2] <= box[0] + box[2] and rect[1] + rect[3] <= box[1] + box[3])


def overlaps(a: list, b: list) -> bool:
    return (a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and
            a[1] < b[1] + b[3] and b[1] < a[1] + a[3])


def rebuilds(wire) -> int:
    return wire.call("app.state")["layer_thumbs"]["rebuilds"]


def card_at(got: list, index: int) -> dict:
    return next(card for card in got if card["index"] == index)


def canvas_shot(wire, name: str) -> Image.Image:
    return Image.open(wd.capture_canvas(wire, name)["path"]).convert("RGB")


def differs(a: Image.Image, b: Image.Image) -> tuple[int, int]:
    """How far apart two captures are: how many pixels moved, and by how much at worst."""
    steps = ImageChops.difference(a, b).convert("L").histogram()
    moved = sum(steps[1:])
    worst = max((step for step, count in enumerate(steps) if count), default=0)
    return moved, worst


def apart(a: Image.Image, b: Image.Image) -> str:
    moved, worst = differs(a, b)
    return f"{moved} of {a.size[0] * a.size[1]} px differ, worst step {worst}"


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
        # must not have been re-sampled - the caching rule, checked with pixels and with the count the
        # panel publishes, because pixels alone could be identical by luck.
        sampled = rebuilds(wire)
        wire.call("layer.set", {"index": got[1]["index"], "opacity": 0.35})
        wd.settle(wire)
        after_opacity = grab(wire, cards(wire)[1]["picture"], "thumb-after-opacity")
        p.check(ImageChops.difference(picture, after_opacity).getbbox() is None,
                "an opacity change leaves the layer's picture byte-identical",
                str(ImageChops.difference(picture, after_opacity).getbbox()))
        p.check(rebuilds(wire) == sampled, "and costs no resample at all",
                f"{sampled} to {rebuilds(wire)}")

        # Painting does move the layer, so the card has to follow.
        wire.call("layer.select", {"index": got[1]["index"]})
        wire.call("tool.stroke", {"points": [[20.0, 20.0], [200.0, 140.0]], "size": 40.0,
                                  "hardness": 1.0})
        wd.settle(wire)
        after_paint = grab(wire, cards(wire)[1]["picture"], "thumb-after-paint")
        p.check(ImageChops.difference(picture, after_paint).getbbox() is not None,
                "painting into the layer changes its picture",
                str(ImageChops.difference(picture, after_paint).getbbox()))
        p.check(rebuilds(wire) > sampled, "and the panel's own count moves when a write does",
                f"{sampled} to {rebuilds(wire)}")

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

        # Where each control of a card is allowed to be. ImGui gives a point to the item submitted
        # *first*, so a control that overlaps a wider one draws, lights up under the cursor, and never
        # presses - which no screenshot shows, and which is why the reported rects are checked against
        # each other and not just against the card.
        got = cards(wire)
        CONTROLS = ("visibility", "title", "move_up", "move_down", "merge", "opacity", "mode")
        astray = [f"layer {card['index']}.{name}={[round(v) for v in card[name]]} in "
                  f"box={[round(v) for v in card['box']]}"
                  for card in got for name in CONTROLS if not inside_box(card[name], card["box"])]
        p.check(not astray, "every control of a card is drawn inside that card", "; ".join(astray))
        clashing = [f"layer {card['index']}: {a} over {b}"
                    for card in got for i, a in enumerate(CONTROLS) for b in CONTROLS[i + 1:]
                    if overlaps(card[a], card[b])]
        p.check(not clashing, "and no two controls of a card claim the same pixel",
                "; ".join(clashing))

        # The stack controls, on a stack of three so the middle card can go both ways. The two upper
        # layers each get a picture of their own, in their own colour, crossing the other's: an order is
        # then something the canvas shows and not only the list.
        wire.call("layer.add", {})
        for index, colour, points in ((1, [40, 200, 80, 255], [[20.0, 20.0], [200.0, 140.0]]),
                                      (2, [220, 40, 40, 255], [[20.0, 140.0], [200.0, 20.0]])):
            wire.call("layer.select", {"index": index})
            wire.call("tool.set", {"color": colour})
            wire.call("tool.stroke", {"points": points, "size": 40.0, "hardness": 1.0})
        wd.settle(wire)

        def stack() -> list:
            return [layer["name"] for layer in wire.call("layer.list")["layers"]]

        got = cards(wire)
        stack_before = stack()
        shown_before = wire.call("doc.get")["checksum"]
        canvas_before = canvas_shot(wire, "stack-before")
        p.check(len(got) == 3 and [c["index"] for c in got] == [2, 1, 0],
                "three layers, three cards, top first", str([c["index"] for c in got]))
        p.check(got[0]["can_move_up"] is False and got[0]["can_move_down"] is True,
                "the top card can go down but not up",
                f"up={got[0]['can_move_up']} down={got[0]['can_move_down']}")
        p.check(got[1]["can_move_up"] and got[1]["can_move_down"] and got[1]["can_merge"],
                "a card in the middle of the stack can do all three")
        p.check(got[-1]["can_move_up"] is True and got[-1]["can_move_down"] is False and
                got[-1]["can_merge"] is False,
                "the bottom card can go up, and has nothing to merge onto",
                f"up={got[-1]['can_move_up']} down={got[-1]['can_move_down']} "
                f"merge={got[-1]['can_merge']}")
        thumb_moved = grab(wire, card_at(got, 1)["picture"], "thumb-before-up")
        sampled_stack = rebuilds(wire)

        click(inj, *centre(got[0]["move_up"]))
        wd.settle(wire)
        p.check(stack() == stack_before, "the dim arrow of the top card answers a press with nothing",
                str(stack()))

        click(inj, *centre(card_at(cards(wire), 1)["move_up"]))
        wd.settle(wire)
        p.check(stack() == [stack_before[0], stack_before[2], stack_before[1]],
                "a card's Up moves that layer and no other", f"{stack_before}")
        moved_canvas = canvas_shot(wire, "stack-moved")
        p.check(differs(canvas_before, moved_canvas)[0] > 0,
                "and the canvas shows the new order", apart(canvas_before, moved_canvas))
        # The picture belongs to the layer, so it travels with it: the moved layer's thumbnail is now
        # under the card above it, and the card it left behind shows the other one.
        thumb_moved_on = grab(wire, card_at(cards(wire), 2)["picture"], "thumb-after-up")
        p.check(differs(thumb_moved, thumb_moved_on)[0] == 0, "and its picture went with it",
                apart(thumb_moved, thumb_moved_on))
        p.check(rebuilds(wire) == sampled_stack, "and the reorder resampled nothing either",
                f"{sampled_stack} to {rebuilds(wire)}")
        wire.call("edit.undo", {})
        wd.settle(wire)
        p.check(stack() == stack_before, "and one undo puts the stack back exactly", str(stack()))
        p.check(ImageChops.difference(canvas_before,
                                      canvas_shot(wire, "stack-undone")).getbbox() is None,
                "and the canvas with it - a reorder repaints, it does not paint")
        p.check(wire.call("doc.get")["checksum"] == shown_before,
                "and the document back byte for byte, the order included")

        click(inj, *centre(card_at(cards(wire), 1)["move_down"]))
        wd.settle(wire)
        p.check(stack() == [stack_before[1], stack_before[0], stack_before[2]],
                "and its Down is the other way round", str(stack()))
        wire.call("edit.undo", {})
        wd.settle(wire)
        p.check(stack() == stack_before, "again undone by one step", str(stack()))

        # Merge: the two layers the canvas already showed become one, and undo separates them again - the
        # survivor holding both pictures in between, and only its own afterwards.
        lower_thumb = grab(wire, card_at(cards(wire), 1)["picture"], "thumb-lower-before")
        click(inj, *centre(card_at(cards(wire), 2)["merge"]))
        wd.settle(wire)
        merged = wire.call("layer.list")["layers"]
        p.check(len(merged) == len(stack_before) - 1, "and Merge down takes one layer away",
                str([layer["name"] for layer in merged]))
        # The merge is what the blend already did, so the picture the person sees cannot have moved. The
        # document's own checksum *has* moved - the survivor really did take the other layer's pixels -
        # which is why this claim is a capture and not that number. It is not exact and cannot be: the
        # blend rounds each channel to 8 bits, so compositing the pair once and the merged layer once is
        # not the same arithmetic as each layer once. Measured on this stack: 12 px of 447 700, each by
        # one step, at the edges where the two strokes' soft pixels meet. A merge that dropped the mask,
        # the opacity or the mode moves thousands of pixels by tens.
        merged_canvas = canvas_shot(wire, "stack-merged")
        moved_px, worst_step = differs(canvas_before, merged_canvas)
        p.check(worst_step <= 1 and moved_px * 1000 < canvas_before.size[0] * canvas_before.size[1],
                "while the canvas shows what it showed, to within the rounding of a second blend",
                apart(canvas_before, merged_canvas))
        p.check(ImageChops.difference(lower_thumb,
                                      grab(wire, card_at(cards(wire), 1)["picture"],
                                           "thumb-lower-merged")).getbbox() is not None,
                "and the layer it merged into now holds both pictures")
        wire.call("edit.undo", {})
        wd.settle(wire)
        p.check(stack() == stack_before, "and undo gives the merged layer back by name", str(stack()))
        p.check(wire.call("doc.get")["checksum"] == shown_before,
                "and every byte of both layers back, the survivor having given its borrow")
        p.check(ImageChops.difference(lower_thumb,
                                      grab(wire, card_at(cards(wire), 1)["picture"],
                                           "thumb-lower-undone")).getbbox() is None,
                "and the other layer back to only its own pixels")

        # Flatten: the whole stack becomes the one layer the canvas was showing. The picture is the claim
        # that decides whether the beat works - "I am lost in layers" is no win if the canvas changes -
        # and the order the layers come back in is the thing a history of one-position-per-layer cannot
        # promise, so it is walked twice here on a stack whose layers are each a different colour.
        top_thumb = grab(wire, cards(wire)[0]["picture"], "thumb-top-before-flatten")
        # The stack's own buttons are below the cards in the same scrollable panel, so on three layers
        # they begin past the fold; the claim is that the wheel reaches them and that the press lands
        # where the app then says they are, which is the same rule the last card is held to.
        inj.move(window[0] + window[2] / 2.0, window[1] + window[3] / 2.0)
        for _ in range(8):
            if inside_box(wire.call("app.state")["layer_panel"]["flatten"], window):
                break
            inj.wheel(-3)
            wd.settle(wire)
        panel = wire.call("app.state")["layer_panel"]
        p.check(inside_box(panel["flatten"], window), "the wheel brings the panel's own buttons up",
                f"flatten={[round(v) for v in panel['flatten']]} in "
                f"window={[round(v) for v in window]}")
        p.check(not overlaps(panel["flatten"], panel["add"]) and
                not overlaps(panel["flatten"], panel["remove"]),
                "and the three of them claim no pixel twice",
                f"{[round(v) for v in panel['add']]} {[round(v) for v in panel['remove']]} "
                f"{[round(v) for v in panel['flatten']]}")
        p.check(panel["can_flatten"] is True, "and Flatten is live while the stack has more than one layer")
        click(inj, *centre(panel["flatten"]))
        wd.settle(wire)
        flat = wire.call("layer.list")["layers"]
        p.check(len(flat) == 1, "one press leaves one layer", str([l["name"] for l in flat]))
        p.check(flat[0]["name"] == "Background", "under the name that says what it is", flat[0]["name"])
        p.check(flat[0]["opacity"] == 1.0 and flat[0]["mode"] == "normal" and flat[0]["visible"],
                "neutral, so the composite cannot re-blend what was just baked into it",
                str({key: flat[0][key] for key in ("opacity", "mode", "visible")}))
        flattened_canvas = canvas_shot(wire, "stack-flattened")
        p.check(ImageChops.difference(canvas_before, flattened_canvas).getbbox() is None,
                "and the canvas has not moved by a pixel - flatten bakes, it does not repaint",
                apart(canvas_before, flattened_canvas))
        # The card holds a layer's own content, so the survivor's picture has to be more than any single
        # layer's was - the strokes and the photograph underneath them, where the top layer had its own
        # stroke over nothing.
        p.check(ImageChops.difference(top_thumb,
                                      grab(wire, cards(wire)[0]["picture"],
                                           "thumb-flattened")).getbbox() is not None,
                "and the one card left shows the picture the stack was making, not one layer's")
        p.check(wire.call("app.state")["layer_panel"]["can_flatten"] is False,
                "a stack of one has nothing to flatten, and says so")
        click(inj, *centre(wire.call("app.state")["layer_panel"]["flatten"]))
        wd.settle(wire)
        p.check(len(wire.call("layer.list")["layers"]) == 1,
                "and the dim button answers a press with nothing")
        wire.call("edit.undo", {})
        wd.settle(wire)
        p.check(stack() == stack_before, "one undo gives the whole stack back, in the order it was",
                f"{stack()} from {stack_before}")
        p.check(wire.call("doc.get")["checksum"] == shown_before,
                "every layer byte for byte, none of them holding the flattened picture")
        p.check(ImageChops.difference(canvas_before,
                                      canvas_shot(wire, "stack-flatten-undone")).getbbox() is None,
                "and the canvas as it was")
        wire.call("edit.redo", {})
        wd.settle(wire)
        p.check(len(wire.call("layer.list")["layers"]) == 1, "redo flattens it again")
        wire.call("edit.undo", {})
        wd.settle(wire)
        # The second walk is the one that catches a stack put back in the wrong order: an index recorded
        # against a shrinking stack re-inserts bottom-last, which shuffles and looks plausible.
        p.check(stack() == stack_before, "and undo a second time is as exact as the first", str(stack()))

        # A card's title row selects its layer - and about the awkward one, which is the last card down:
        # three of them do not fit a panel this tall, so the wheel is how a person reaches it, and the
        # press has to land where the app now says the control is rather than where it used to be.
        # Back to the top of the list first: the claims below are about a wheel-down reaching the last
        # card, which is nothing if the panel is already at the bottom.
        inj.move(window[0] + window[2] / 2.0, window[1] + window[3] / 2.0)
        for _ in range(10):
            inj.wheel(3)
            wd.settle(wire)
        wire.call("layer.select", {"index": cards(wire)[0]["index"]})
        wd.settle(wire)
        above_fold = card_at(cards(wire), 0)
        inj.move(window[0] + window[2] / 2.0, window[1] + window[3] / 2.0)
        inj.wheel(-6)
        wd.settle(wire)
        got = cards(wire)
        bottom = card_at(got, 0)
        p.check(bottom["title"][1] < above_fold["title"][1],
                "the wheel brings the last card up into the panel",
                f"{round(above_fold['title'][1])} to {round(bottom['title'][1])}")
        p.check(inside_box(bottom["title"], window), "until its title row is on screen",
                str([round(v) for v in bottom["title"]]))
        p.check([card["index"] for card in got if card["active"]] != [bottom["index"]],
                "and another layer is the active one before the click", str(got[0]["index"]))
        click(inj, *centre(bottom["title"]))
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
