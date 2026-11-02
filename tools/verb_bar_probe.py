#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Put a mark on the canvas and press the verbs that appear beside it.

`verb.bar` and the panel both press `ai.<verb>`, so the commands are covered elsewhere; what only this
can see is the bar itself — that it is drawn where the mark is rather than where a remembered constant
says it should be, that it stays out of the mark it is anchored to, and that a click on one of its
buttons is a real press of that verb and lands an answer.

The buttons are found by reading the app's own report of where it drew them, so a bar that moved,
resized or reordered is still pressed; a bar that was never drawn fails the claim instead of clicking
through empty canvas.

Runs against the committed recordings: no request leaves the machine.

    python3 tools/verb_bar_probe.py --bin build/noise-edit
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402

WIDTH, HEIGHT = 1100, 900
KEY_RETURN = 0xFF0D  # XK_Return: the popup takes Enter as confirmation
MARK = [120, 100, 200, 140]
EXPECTED = ["ai.heal", "ai.remove", "ai.fill", "ai.change"]


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
    inj.move(int(x), int(y))


def mark_screen(state: dict) -> tuple[float, float, float, float]:
    """`MARK` projected through the same view the app drew with."""
    left, top, right, bottom = wd.document_screen_rect(state)
    scale = state["view"]["scale"]
    return left + MARK[0] * scale, top + MARK[1] * scale, left + (MARK[0] + MARK[2]) * scale, top + (
        MARK[1] + MARK[3]) * scale


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="verb-bar-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    p = Probe()
    print("the verbs that come to the marked area")
    skip = wd.display_skip()
    if skip:
        print(f"SKIP: {skip}")
        return 0
    display = wd.ensure_display(WIDTH, HEIGHT)
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, open(tmp / "app.stderr", "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None},
                        start_dialog=False)
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0), timeout=120)
        inj = wi.Injector(display)
        # Xvfb has no window manager: mouse presses can hit the app while keyboard
        # events still go nowhere. Focus it before typing into the verb's prompt.
        inj.focus_app_window(WIDTH, HEIGHT)
        wire.call("doc.new", {"width": 512, "height": 512})
        wire.call("server.connect")
        wd.settle(wire)

        empty = wire.call("verb.bar")
        p.check(empty["shown"] is False and empty["buttons"] == [],
                "an unmarked canvas offers no bar", str(empty["mark"]))

        blank_checksum = wire.call("doc.get")["checksum"]
        wire.call("select.rect", {"rect": MARK})
        wd.settle(wire)
        bar = wire.call("verb.bar")
        p.check(bar["shown"] is True, "the mark brings the bar")
        p.check(bar["mark"] == MARK, "and it hangs off the mark the app holds", str(bar["mark"]))
        p.check([b["command"] for b in bar["buttons"]] == EXPECTED,
                "the buttons are the verbs whose recipe reads the marked area",
                str([b["label"] for b in bar["buttons"]]))
        p.check(all(b["ready"] for b in bar["buttons"]), "and a server means they all run",
                str([b["reason"] for b in bar["buttons"] if not b["ready"]]))

        bx, by, bw, bh = bar["screen"]
        ml, mt, mr, mb = mark_screen(wire.call("app.state"))
        p.check(bw > 0 and bh > 0 and bx >= 0 and by >= 0 and bx + bw <= WIDTH and by + bh <= HEIGHT,
                "the bar is inside the window", str(bar["screen"]))
        p.check(by + bh <= mt + 2 or by >= mb - 2, "and clear of the mark it is anchored to",
                f"bar y {by:.0f}..{by + bh:.0f}, mark y {mt:.0f}..{mb:.0f}")
        p.check(ml <= bx + bw / 2 <= mr, "centred over it",
                f"bar centre {bx + bw / 2:.0f}, mark x {ml:.0f}..{mr:.0f}")

        # The first button, pressed where the app says it drew it. `Remove` is the one whose recording
        # is committed, so the answer comes back without a network.
        remove = next(b for b in bar["buttons"] if b["command"] == "ai.remove")
        index = bar["buttons"].index(remove)
        step = bw / len(bar["buttons"])
        jobs_before = len(wire.call("job.list")["jobs"])
        click(inj, bx + step * (index + 0.5), by + bh / 2)
        wd.settle(wire, 2)
        started = wire.call("job.list")["jobs"]
        p.check(len(started) == jobs_before + 1, "the bar's button started a job",
                f"{jobs_before} -> {len(started)}")
        if started:
            done = wire.call("job.wait", {"id": started[-1]["id"], "timeout": 120})
            p.check(done["state"] == "succeeded", "and it finished",
                    str(done.get("job", {}).get("error", "")))
            # The canvas was blank, so the answer *is* the canvas: one empty layer and an answer above
            # it is a stack nobody asked for. The pixels are the claim, not the layer count.
            landed = wire.call("layer.list")["layers"]
            p.check(len(landed) == 1, "and it arrived on the blank canvas, not over it",
                    f"{len(landed)} layers")
            p.check(wire.call("doc.get")["checksum"] != blank_checksum,
                    "and the canvas changed")

        # Once a result lands, the old mark remains but its floating bar stays hidden.
        # Start the next edit with a fresh selection, as a user would, and read the newly
        # drawn button positions instead of clicking where the previous bar used to be.
        completed_bar = wire.call("verb.bar")
        p.check(not completed_bar["shown"], "the completed edit hides the previous bar")
        wire.call("select.clear")
        wire.call("select.rect", {"rect": MARK})
        wd.settle(wire)
        bar = wire.call("verb.bar")
        p.check(bar["shown"] and [b["command"] for b in bar["buttons"]] == EXPECTED,
                "a fresh selection brings the AI verbs back")
        bx, by, bw, bh = bar["screen"]
        step = bw / len(bar["buttons"])

        # A verb that wants words answers a click with a box over the mark, not with a run on whatever
        # the panel happens to hold - and not with a job either, until the words are in.
        fill = next(b for b in bar["buttons"] if b["verb"] == "fill")
        fill_index = bar["buttons"].index(fill)
        jobs_before_fill = len(wire.call("job.list")["jobs"])
        click(inj, bx + step * (fill_index + 0.5), by + bh / 2)
        wd.settle(wire)
        ask = wire.call("verb.bar")["ask"]
        p.check(ask is not None and ask["verb"] == "fill",
                "the button that wants words opened a box instead of running", str(ask))
        p.check(len(wire.call("job.list")["jobs"]) == jobs_before_fill,
                "and nothing started while the box was empty")
        ax, ay, aw, ah = ask["screen"]
        p.check(aw >= 200 and ah >= 3 * 22, "the box is big enough for its own label and buttons",
                f"{aw:.0f}x{ah:.0f}")

        # Typed with real keys into the box the click opened, then confirmed with Return: the whole
        # gesture a person makes, with the pointer only ever clicking what the app reported.
        marker = "quartz lampshade"
        for ch in marker:
            inj.key(0x20 if ch == " " else ord(ch))
        wd.settle(wire)
        p.check(marker in wire.call("ai.intent")["vars"].get("prompt", ""),
                "the box holds what was typed", str(wire.call("ai.intent")["vars"]))
        inj.key(KEY_RETURN)
        wd.settle(wire, 2)
        started_fill = wire.call("job.list")["jobs"]
        p.check(len(started_fill) == jobs_before_fill + 1, "and Return ran the verb",
                f"{jobs_before_fill} -> {len(started_fill)}")
        if len(started_fill) > jobs_before_fill:
            done_fill = wire.call("job.wait", {"id": started_fill[-1]["id"], "timeout": 120})
            p.check(done_fill["state"] == "succeeded", "and it finished",
                    str(done_fill.get("job", {}).get("error", "")))
        p.check(wire.call("verb.bar")["ask"] is None, "and the box closed when the press was taken")
        wd.settle(wire)
        editor = wire.call("verb.bar")
        p.check(editor["mode"] == "actions" and not editor["shown"] and editor["buttons"] == [],
                "the completed edit leaves no floating dialog", str(editor))
        p.check(wire.call("layer.plane")["plane"] == "pixels",
                "the result leaves image pixels selected rather than the mask")
        wire.call("layer.plane", {"plane": "mask"})
        p.check(wire.call("layer.plane")["plane"] == "mask",
                "the result mask remains available by its thumbnail")
        wire.call("layer.plane", {"plane": "pixels"})
        wire.call("select.clear")
        wd.settle(wire)
        p.check(not wire.call("verb.bar")["shown"],
                "clearing the old selection does not reveal a return dialog")
        wire.call("select.rect", {"rect": MARK})
        wd.settle(wire)
        returned = wire.call("verb.bar")
        p.check(returned["shown"] and len(returned["buttons"]) == len(EXPECTED),
                "a new selection brings back Heal, Remove, Fill and Change")

        # With the mark gone the bar is gone, and the pixels where it used to be are only canvas: a
        # click there starts nothing rather than pressing an invisible button.
        wire.call("select.clear")
        wd.settle(wire)
        p.check(wire.call("verb.bar")["shown"] is False, "clearing the new mark takes the bar away")
        jobs_now = len(wire.call("job.list")["jobs"])
        click(inj, bx + step * (index + 0.5), by + bh / 2)
        wd.settle(wire, 2)
        p.check(len(wire.call("job.list")["jobs"]) == jobs_now, "and nothing is left to press")
    except wd.WireError as why:
        print(f"  FAIL wire: {why}")
        p.failed += 1
    finally:
        wd.shutdown(proc, wire)
    print(("FAILURES " if p.failed else "all claims held ") + f"({tmp})")
    return 1 if p.failed else 0


if __name__ == "__main__":
    sys.exit(main())
