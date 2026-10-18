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
twice, because a stack put back in the wrong order still looks like a stack. Naming a layer is claimed
with real keys on the card itself: `Enter` commits, and `Escape` cancels the edit instead of leaving the
program - the one claim here that only a keyboard can make. And the row those stack-wide
verbs live in is claimed to be *pinned* - below the scrolling list rather than at the end of it, so three
cards cannot push the way to flatten them off the panel.

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
import time

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


# XK_Return, for the field that commits on it, and XK_F2 for the one key that opens it.
KEY_RETURN = 0xFF0D
KEY_F2 = 0xFFBF


def type_text(inj: wi.Injector, words: str) -> None:
    """Keys, one character at a time: lowercase ASCII and space, which are their own keysyms."""
    for ch in words:
        inj.key(ord(ch))


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


def interior(image: Image.Image, edge: int = 1) -> Image.Image:
    """A reported rect is fractional and a capture is not, so the outermost pixel of a grab can belong to
    the frame around the widget rather than to the widget - which is how a thumbnail that had not changed
    differed from itself by one column of card border. Compare what is inside."""
    return image.crop((edge, edge, image.size[0] - edge, image.size[1] - edge))


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

        panel = wire.call("app.state")["layer_panel"]
        window = wire.call("app.state")["layers_window"]
        fold = panel["list"]
        inside = all(
            card["box"][0] >= fold[0] - 1 and card["box"][1] >= fold[1] - 1 and
            card["box"][0] + card["box"][2] <= fold[0] + fold[2] + 1
            for card in got)
        p.check(inside, "every card sits inside the panel's scrolling list",
                str([round(v) for v in fold]))
        # The row that answers for the whole stack is pinned below the list, not filed at the end of it.
        # It used to be the last thing in a scrollable panel, which at three layers put it at y=926 in a
        # window ending at 800: reported, drawn nowhere, reachable only by scrolling to it.
        p.check(inside_box(fold, window) and
                all(button[1] >= fold[1] + fold[3] - 1 for button in
                    (panel["add"], panel["remove"], panel["flatten"])),
                "and the stack's own row is below the list and inside the panel",
                f"list={[round(v) for v in fold]} row={[round(v) for v in panel['flatten']]} "
                f"window={[round(v) for v in window]}")
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
        p.check(ImageChops.difference(interior(thumb_moved), interior(thumb_moved_on)).getbbox() is None,
                "and its picture went with it, inside the box the card reported",
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
        # Three cards do not fit the list, which is the case that used to push this row off the panel. The
        # press has to land with no scrolling involved: a person reaching for Flatten reaches for the
        # button, not for the wheel.
        panel = wire.call("app.state")["layer_panel"]
        p.check(all(inside_box(button, window) for button in
                    (panel["add"], panel["remove"], panel["flatten"])),
                "three cards deep, the panel's own row is still on screen with nothing scrolled",
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

        # Naming a layer is how a person says which one they mean, so it happens on the card: `F2` turns
        # that card's title row into the field, `Enter` commits it through `layer.rename`, and `Escape` is
        # the field's to cancel with rather than the program's to leave by. That last one needs a real key:
        # the same key ends the app the moment nothing owns the keyboard.
        #
        # The double-click a hand uses opens the same field and is not driven here. Injected button presses
        # reach this app ~0.5 s apart - measured, presses 200 frames apart at ~340 fps however the driver
        # spaces them - which is outside ImGui's 0.3 s double-click window, so a probe claiming to
        # "double-click" would assert something it never did. `layer.rename.begin` is the door the key and
        # the double-click share, and the one a driver can trust.
        naming = card_at(cards(wire), 1)
        named = naming["index"]
        named_before = wire.call("layer.list")["layers"][named]["name"]
        canvas_named = canvas_shot(wire, "before-rename")
        sampled_rename = rebuilds(wire)
        wire.call("layer.select", {"index": named})
        wd.settle(wire)
        inj.key(KEY_F2)
        wd.settle(wire)
        editing = [card["index"] for card in cards(wire) if card["renaming"]]
        opened = p.check(editing == [named],
                         "F2 puts the active layer's card into the field", str(editing))
        if not opened:
            # Everything below types, and a key typed while nothing owns the keyboard is a shortcut: the
            # letters would press the app's own verbs and `Escape` would ask it to quit. Stop here rather
            # than spend the rest of the run on a cascade.
            p.check(False, "the claims after it need the field open, so they were not attempted")
        else:
            p.check(wire.call("layer.list")["layers"][named]["name"] == named_before,
                    "and nothing is renamed before it is confirmed")
            clashing = [f"layer {card['index']}: {a} over {b}"
                        for card in cards(wire)
                        for i, a in enumerate(CONTROLS) for b in CONTROLS[i + 1:]
                        if overlaps(card[a], card[b])]
            p.check(not clashing, "the field takes the name's place rather than lying over it",
                    "; ".join(clashing))
            try:
                wire.call("layer.rename", {"index": named, "name": "   "})
                refused = "accepted"
            except wd.WireError as exc:
                refused = str(exc)
            p.check("not a name a layer can carry" in refused and "'   '" in refused,
                    "a name of nothing is refused by the command the field uses, naming it", refused)
            inj.modified_key(ord("a"), wi.KEY_CTRL_L)  # select what the card called it
            type_text(inj, "sky")
            inj.key(KEY_RETURN)
            wd.settle(wire)
            p.check(wire.call("layer.list")["layers"][named]["name"] == "sky",
                    "and Enter commits it through `layer.rename`",
                    wire.call("layer.list")["layers"][named]["name"])
            p.check(not [card for card in cards(wire) if card["renaming"]],
                    "the field closes once the name is in")
            p.check(ImageChops.difference(canvas_named,
                                          canvas_shot(wire, "after-rename")).getbbox() is None,
                    "a rename moves no pixel - the canvas is not where a name is")
            p.check(rebuilds(wire) == sampled_rename, "and costs no card a resample",
                    f"{sampled_rename} to {rebuilds(wire)}")
            wire.call("edit.undo", {})
            wd.settle(wire)
            p.check(wire.call("layer.list")["layers"][named]["name"] == named_before,
                    "one undo gives the name back, so a mistyped one does not cost the stroke under it",
                    wire.call("layer.list")["layers"][named]["name"])

            wire.call("layer.rename.begin", {"index": named})
            wd.settle(wire)
            inj.modified_key(ord("a"), wi.KEY_CTRL_L)
            type_text(inj, "not this")
            inj.key(wi.KEY_ESCAPE)
            wd.settle(wire)
            p.check(wire.call("layer.list")["layers"][named]["name"] == named_before,
                    "Escape drops a half-typed name instead of saving it",
                    wire.call("layer.list")["layers"][named]["name"])
            p.check(len(cards(wire)) == 3, "and the same key left the app running, three cards deep")

        # A card's title row selects its layer - and about the awkward one, which is the last card down:
        # three of them do not fit a panel this tall, so the wheel is how a person reaches it, and the
        # press has to land where the app now says the control is rather than where it used to be.
        # Back to the top of the list first: the claims below are about a wheel-down reaching the last
        # card, which is nothing if the panel is already at the bottom.
        fold = wire.call("app.state")["layer_panel"]["list"]
        inj.move(fold[0] + fold[2] / 2.0, fold[1] + fold[3] / 2.0)
        for _ in range(10):
            inj.wheel(3)
            wd.settle(wire)
        wire.call("layer.select", {"index": cards(wire)[0]["index"]})
        wd.settle(wire)
        above_fold = card_at(cards(wire), 0)
        inj.wheel(-6)
        wd.settle(wire)
        got = cards(wire)
        bottom = card_at(got, 0)
        p.check(bottom["title"][1] < above_fold["title"][1],
                "the wheel brings the last card up through the list",
                f"{round(above_fold['title'][1])} to {round(bottom['title'][1])}")
        p.check(inside_box(bottom["title"], wire.call("app.state")["layer_panel"]["list"]),
                "until its title row is on screen", str([round(v) for v in bottom["title"]]))
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
