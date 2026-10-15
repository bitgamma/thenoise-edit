#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Choose an aspect and a style on the real panel, then run the answer the panel committed.

The commands are tested hermetically; this is the only claim about the **panel**. It exists because the
two dropdowns are where the recipe table meets a person: a row that draws a ratio but never commits it,
or a style that changes the label without changing what the shortcut runs, would leave every
command-level test green while the button did something else.

The controls are found, not remembered. Each sweep step clicks, then clicks once more where an opened
list's second entry would be, and asks the app what its intent now holds. A pair that makes `aspect`
read back as a ratio *is* the aspect row; a pair that makes `recipe` read back as another member of the
family *is* the style row. Rows moving up or down the panel cost a few more clicks rather than a false
failure, and a control that never commits fails the claim.

The document is blank and unmarked, and no words are typed until the row has been found, so a stray
click lands on a verb that refuses rather than on one that edits.

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

# A window tall enough that the whole verb block is above the fold: the panel scrolls, and a sweep that
# had to scroll to reach a row would be claiming the scroll worked rather than that the row does.
WIDTH, HEIGHT = 1100, 900
SWEEP_STEP = 14
SWEEP_EDGE = 26  # px in from the right edge: inside a dropdown, outside a left-aligned button
# Entries of an opened list, counted from the row that opened it. The first entry is skipped: it is
# "canvas" for aspects and the row's own recipe for styles, and picking what is already held commits
# nothing. `ai.verbs` reports both lists head-first, so entry 2 is the first real choice.
LIST_ENTRIES = (2, 3, 4)
LIST_ROW = 22  # the height ImGui draws a list entry at, with this style's frame padding
ENTRY_DROP = 12  # the list starts under the row that opened it, not at it


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


def find_row(wire: wd.Wire, inj: wi.Injector, reads, p: Probe, what: str,
             start: int = 80, stop: int = HEIGHT - 160) -> int | None:
    """Click down the right edge until opening a dropdown and picking an entry changes `reads()`.

    The list is reopened for every entry tried: picking an entry closes it, so a second attempt after a
    no-op pick (the entry already held) would otherwise click on the panel behind. Returns the y of the
    row that worked, so a later claim points at the same widget.
    """
    x = WIDTH - SWEEP_EDGE
    for y in range(start, stop, SWEEP_STEP):
        for entry in LIST_ENTRIES:
            before = reads()
            click(inj, x, y)
            wd.settle(wire, 1)
            click(inj, x, y + entry * LIST_ROW + ENTRY_DROP)
            wd.settle(wire, 2)
            if reads() != before:
                p.check(True, f"the {what} row commits from a click", f"y={y} entry {entry}")
                return y
        # No Escape here to close a leftover list: `Escape` is `app.quit`. The next position's first
        # click is what closes it, which costs that position and nothing else.
    p.check(False, f"the {what} row commits from a click")
    return None


def run(wire: wd.Wire, inj_display: str, p: Probe) -> None:
    inj = wi.Injector(inj_display)
    try:
        wire.call("doc.new", {"width": 512, "height": 512})
        wire.call("server.connect")
        wire.call("ai.intent", {"recipe": "new-image"})
        row = {v["id"]: v for v in wire.call("ai.verbs")["verbs"]}["generate"]
        p.check(list(row["aspects"]) == ["1:1", "3:4", "4:3", "16:9"],
                "the generate row offers the recipe's aspects", str(row.get("aspects")))
        p.check(len(row.get("variants", [])) == 2, "and the recipe's styles",
                str([m["id"] for m in row.get("variants", [])]))
        p.check(row["active"] == "new-image", "and names what a press would run")

        y_aspect = find_row(wire, inj, lambda: wire.call("ai.intent", {})["aspect"], p, "aspect")
        if y_aspect is None:
            return
        chosen = wire.call("ai.intent", {})["aspect"]
        p.check(chosen is not None, "the dropdown chose a ratio", str(chosen))

        # The frame chosen on the panel has to be the frame the request asks for.
        wide, high = wire.call("ai.intent", {})["quote"]["size"]
        want = {"1:1": 1.0, "3:4": 0.75, "4:3": 1.3333, "16:9": 1.7778}[chosen]
        p.check(abs(wide / high - want) < 0.03, "the quoted size carries the ratio",
                f"{wide}x{high} for {chosen}")
        p.check(wide == 512 or high == 512, "and one axis still fills the canvas",
                f"{wide}x{high}")

        # The style row sits below the aspect row, so start the sweep where the aspect sweep stopped and
        # look for a click pair that changes which recipe the intent holds.
        held = wire.call("ai.intent", {})["recipe"]
        # The style row is the next one down from the aspect row, so the sweep is bounded rather than
        # running to the bottom of the dock: what it would find there is a slider.
        style_y = find_row(wire, inj, lambda: wire.call("ai.intent", {})["recipe"], p, "style",
                           start=y_aspect + SWEEP_STEP, stop=y_aspect + 200)
        if style_y is None:
            return
        style = wire.call("ai.intent", {})["recipe"]
        p.check(style != held, "the style row changed the recipe the intent holds",
                f"{held} -> {style}")
        active = {v["id"]: v for v in wire.call("ai.verbs")["verbs"]}["generate"]["active"]
        p.check(active == style, "and the row reports that recipe as the one a press would run")

        # A style is a recipe, so running it is an ordinary job: the recording answers and it lands.
        wire.call("ai.intent", {"vars": {"prompt": "a red cube on a shelf"}})
        ran = wire.call("ai.generate", {})
        if p.check(ran.get("job") is not None, "the key ran the style"):
            done = wire.call("job.wait", {"id": ran["job"]["id"], "timeout": 60})
            p.check(done["state"] == "succeeded", "and it finished",
                    str(done.get("job", {}).get("error", "")))
            p.check(len(wire.call("layer.list")["layers"]) == 2,
                    "and it landed on a layer of its own")
    finally:
        inj.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="build/noise-edit")
    args = parser.parse_args()
    args.bin = pathlib.Path(args.bin)
    reason = skip_reason(args)
    if reason:
        print(f"SKIP: {reason}")
        return 0
    try:
        display = wd.ensure_display(WIDTH, HEIGHT)
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
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, open(stderr_log, "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None})
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
        wd.wait_for_fit(wire)
        try:
            run(wire, inj_display=display, p=p)
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
