#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Choose an aspect and a style on the real panel, then run the answer the panel committed.

The commands are tested hermetically; this is the only claim about the **panel**. It exists because the
two dropdowns are where the recipe table meets a person: a row that draws a ratio but never commits it,
or a style that changes the label without changing what the shortcut runs, would leave every
command-level test green while the button did something else.

The controls publish their on-screen rectangles through app.state; the probe clicks those widgets
and their actual popup entries, instead of sweeping across unrelated controls after a layout change.

    python3 tools/panel_aspect_probe.py --bin build/noise-edit

Runs against the committed recordings: no request leaves the machine.
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402


class Probe:
    """Claims about the window, printed as they are decided."""

    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> bool:
        print(("  ok   " if ok else "  FAIL ") + claim + (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def skip_reason(args: argparse.Namespace) -> str | None:
    if not args.bin.exists():
        return f"no binary at {args.bin}"
    return wd.display_skip()


def click(inj: wi.Injector, x: int, y: int) -> None:
    inj.move(x, y)
    inj.button(1, True)
    inj.button(1, False)
    inj.move(x, y)


def click_box(inj: wi.Injector, box: list) -> None:
    x, y, width, height = box
    click(inj, round(x + width / 2), round(y + height / 2))


def picker_rect(wire: wd.Wire, inj: wi.Injector, kind: str) -> list:
    for _ in range(12):
        state = wire.call("app.state")
        box = state["ai_pickers"][kind]
        framebuffer_width, framebuffer_height = state["framebuffer"]
        if (box[2] > 20 and box[0] >= 0 and box[1] >= 0
                and box[0] + box[2] <= framebuffer_width
                and box[1] + box[3] <= framebuffer_height):
            return box
        inj.move(round(framebuffer_width - 300), round(framebuffer_height * 0.5))
        inj.wheel(-3 if box[1] + box[3] > framebuffer_height else 3)
        wd.settle(wire)
    raise wd.WireError(f"{kind} picker remained outside the visible AI Workspace: {box}")


def open_workspace(wire: wd.Wire, inj: wi.Injector) -> None:
    click(inj, 396, 10)
    wd.settle(wire)
    click(inj, 410, 31)
    wd.settle(wire)
    state = wire.call("app.state")
    if state["ai_pickers"]["aspect"][2] <= 0:
        raise wd.WireError("AI Workspace menu did not open the aspect picker")


def choose_picker_option(wire: wd.Wire, inj: wi.Injector, kind: str, choice: str) -> None:
    click_box(inj, picker_rect(wire, inj, kind))
    wd.settle(wire)
    entries = {entry["id"]: entry["screen"]
               for entry in wire.call("app.state")["ai_pickers"][f"{kind}_options"]}
    if choice not in entries:
        raise wd.WireError(f"{kind} dropdown opened without option {choice}: {entries}")
    click_box(inj, entries[choice])
    wd.settle(wire)


def run(wire: wd.Wire, inj_display: str, p: Probe, tmp: pathlib.Path) -> None:
    inj = wi.Injector(inj_display)
    try:
        wire.call("doc.new", {"width": 512, "height": 512})
        open_workspace(wire, inj)
        wire.call("server.connect")
        wire.call("ai.intent", {"recipe": "new-image"})
        row = {v["id"]: v for v in wire.call("ai.verbs")["verbs"]}["generate"]
        p.check(list(row["aspects"]) == ["1:1", "3:4", "4:3", "16:9"],
                "the generate row offers the recipe's aspects", str(row.get("aspects")))
        p.check(len(row.get("variants", [])) == 2, "and the recipe's styles",
                str([m["id"] for m in row.get("variants", [])]))
        p.check(row["active"] == "new-image", "and names what a press would run")

        choose_picker_option(wire, inj, "aspect", "1:1")
        p.check(wire.call("ai.intent", {})["aspect"] == "1:1",
                "the aspect row commits from a click")
        chosen = wire.call("ai.intent", {})["aspect"]
        p.check(chosen is not None, "the dropdown chose a ratio", str(chosen))

        # The frame chosen on the panel has to be the frame the request asks for.
        wide, high = wire.call("ai.intent", {})["quote"]["size"]
        want = {"1:1": 1.0, "3:4": 0.75, "4:3": 1.3333, "16:9": 1.7778}[chosen]
        p.check(abs(wide / high - want) < 0.03, "the quoted size carries the ratio",
                f"{wide}x{high} for {chosen}")
        p.check(wide == 512 or high == 512, "and one axis still fills the canvas",
                f"{wide}x{high}")

        held = wire.call("ai.intent", {})["recipe"]
        choose_picker_option(wire, inj, "style", "new-image-photo")
        style = wire.call("ai.intent", {})["recipe"]
        p.check(style == "new-image-photo" and style != held,
                "the style dropdown selects photo", f"{held} -> {style}")
        active = {v["id"]: v for v in wire.call("ai.verbs")["verbs"]}["generate"]["active"]
        p.check(active == style, "and Generate uses the selected photo recipe")

        # Both controls must keep the chosen style, not silently switch to `new-image`.
        choose_picker_option(wire, inj, "aspect", "3:4")
        p.check(wire.call("ai.intent", {})["recipe"] == style,
                "changing aspect keeps the chosen style")
        click_box(inj, picker_rect(wire, inj, "prompt"))
        for letter in "oak":
            inj.key(ord(letter))
        wd.settle(wire, 3)
        prompted = wire.call("ai.intent", {})
        p.check(prompted["recipe"] == style and prompted["vars"].get("prompt") == "oak",
                "typing in the prompt keeps the chosen style", str(prompted["vars"]))

        choose_picker_option(wire, inj, "aspect", "1:1")
        choose_picker_option(wire, inj, "style", "new-image-anime")
        active = {v["id"]: v for v in wire.call("ai.verbs")["verbs"]}["generate"]["active"]
        p.check(active == "new-image-anime", "and Generate follows the next style")

        # A style is a recipe, so running it is an ordinary job: the recording answers and it lands.
        wire.call("ai.intent", {"vars": {"prompt": "a red cube on a shelf"}})
        ran = wire.call("ai.generate", {})
        if p.check(ran.get("job") is not None, "the key ran the style"):
            done = wire.call("job.wait", {"id": ran["job"]["id"], "timeout": 60})
            p.check(done["state"] == "succeeded", "and it finished",
                    str(done.get("job", {}).get("error", "")))
            # The canvas was blank, so the answer is the canvas: `New` made one empty layer, nobody
            # drew on it, and a layer above an invisible one is a stack nobody asked for.
            landed = wire.call("layer.list")["layers"]
            p.check(len(landed) == 1, "and the answer arrived as the canvas",
                    f"{len(landed)} layers")
            again = wire.call("ai.generate", {})
            done_again = wire.call("job.wait", {"id": again["job"]["id"], "timeout": 60})
            p.check(done_again["state"] == "succeeded", "a second press finished too")
            # Once the canvas holds something, every answer is its own layer again.
            stacked = wire.call("layer.list")["layers"]
            p.check(len(stacked) == 2, "and the next answer landed on a layer of its own",
                    f"{len(stacked)} layers")

        state = wire.call("app.state")
        # A long References group can extend beyond the scrollable tool window. The published drop
        # target must stay within the visible workspace so a drop over another panel or the canvas keeps
        # its ordinary routing.
        wire.call("doc.new", {"width": 512, "height": 512})
        reference_dir = tmp / "references"
        reference_dir.mkdir()
        paths = []
        for index in range(8):
            path = reference_dir / f"reference-{index}.png"
            wd.write_fixture_png(path, 12, 10)
            paths.append(str(path))
        wire.call("ai.references.add", {"paths": paths})
        framebuffer_width, framebuffer_height = state["framebuffer"]
        inj.move(framebuffer_width - 300, framebuffer_height // 2)
        inj.wheel(-4)
        wd.settle(wire, 3)
        state = wire.call("app.state")
        drop_x, drop_y, drop_w, drop_h = state["ai_references_rect"]
        framebuffer_width, framebuffer_height = state["framebuffer"]
        rect_inside_visible_tool = (
            drop_w > 0 and drop_h > 0
            and drop_x >= 0 and drop_y >= 0
            and drop_x + drop_w <= framebuffer_width
            and drop_y + drop_h <= framebuffer_height
        )
        p.check(rect_inside_visible_tool,
                "the reference drop target stays within its visible AI Workspace",
                f"rect={[drop_x, drop_y, drop_w, drop_h]}, framebuffer={state['framebuffer']}")
    finally:
        inj.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="build/noise-edit")
    parser.add_argument("--width", type=int, default=1100)
    parser.add_argument("--height", type=int, default=900)
    args = parser.parse_args()
    args.bin = pathlib.Path(args.bin)
    reason = skip_reason(args)
    if reason:
        print(f"SKIP: {reason}")
        return 0
    try:
        display = wd.ensure_display(args.width, args.height)
    except wd.WireError as exc:
        print(f"SKIP: {exc}")
        return 0

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="panel-aspect-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    root = pathlib.Path(__file__).resolve().parent.parent
    p = Probe()
    print("the panel's own door: clicks on the aspect and style dropdowns")
    stderr_log = tmp / "app.stderr"
    proc = wd.start_app(args, runtime, args.width, args.height, None, open(stderr_log, "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None})
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
        wd.wait_for_fit(wire)
        try:
            run(wire, inj_display=display, p=p, tmp=tmp)
        except wd.WireError as exc:
            # The app exiting mid-sweep looks like a wire bug; its own last words say otherwise.
            p.check(False, "the panel answered the wire",
                    f"{exc}; app said: {stderr_log.read_text(errors='replace')[-400:].strip()}")
    finally:
        wd.shutdown(proc, wire)
    print("FAILURES" if p.failed else "all claims held", f"({tmp})")
    return 1 if p.failed else 0


if __name__ == "__main__":
    sys.exit(main())
