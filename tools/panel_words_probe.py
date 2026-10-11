#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Type into the panel's own words boxes with injected keys, then press the verb's key.

The verbs are commands and the commands are tested; this is the only check that the **panel** reaches
them. It exists because the box used to be one widget shared by the whole section, keyed by the recipe
at one end and by the verb at the other: words typed for Fill were filed under one name and read back
under another, so the panel could not run Fill or Change at all, and every command-level test stayed
green. Nothing but a click on the real widget and a key on the real keyboard can catch that shape.

The boxes are found, not remembered: each sweep step clicks, types a marker of letters no shortcut
claims, and asks the app what the intent now holds. The step that makes a recipe's `prompt` or
`instruction` read back as what was typed *is* that verb's box, so a row moving up or down the panel
costs a few more clicks rather than a false failure. Clicks land near the right edge of the dock
because a words box spans it and the buttons beside it do not.

    python3 tools/panel_words_probe.py --bin build/noise-edit

Runs against the committed recordings: no request leaves the machine.
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402
import wire_input as wi  # noqa: E402

WIDTH, HEIGHT = 1100, 760
MARK = [40, 40, 120, 90]
# Only letters no command claims a plain key for (`command.list`: B E V M F Escape [ ] . ,) and no
# digit: a key that falls on nothing has to stay harmless while the sweep looks for the box.
MARKER_ALPHABET = "zq"
SWEEP_STEP = 14
SWEEP_EDGE = 26  # px in from the right edge: inside a box, outside a left-aligned button


class Probe:
    """Claims about the window, printed as they are decided."""

    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> bool:
        print(("  ok   " if ok else "  FAIL ") + claim + (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def type_text(inj: wi.Injector, text: str) -> None:
    for ch in text:
        inj.key(0x20 if ch == " " else ord(ch))


def jobs(wire: wd.Wire) -> list[dict]:
    return wire.call("job.list", {})["jobs"]


def click(inj: wi.Injector, x: int, y: int) -> None:
    inj.move(x, y)
    inj.button(1, True)
    inj.button(1, False)


def find_words_box(wire: wd.Wire, inj: wi.Injector, field: str, x: int) -> tuple[int, str]:
    """Sweep the dock column for the widget that files text under `field`. Returns (y, marker)."""
    last_error = ""
    for step, y in enumerate(range(150, HEIGHT - 120, SWEEP_STEP)):
        marker = "".join(MARKER_ALPHABET[(step + i) % len(MARKER_ALPHABET)] for i in range(4))
        before = wire.call("doc.get", {})["checksum"]
        click(inj, x, y)
        type_text(inj, marker)
        inj.key(0xff0d)  # Enter: end the edit, so the verb's key is not swallowed by the box
        intent = wire.call("ai.intent", {})
        # A later box may take the typing happily - a different verb's box, and its own field - which
        # clears the intent's error. Keep the first refusal the sweep met: it is the one that explains
        # why the field being hunted never filled in.
        last_error = str(intent.get("error") or last_error)
        if marker in str(intent.get("vars", {}).get(field, "")):
            return y, marker
        # A click can land on a wordless verb and put an answer on the document; the next click expects
        # to find the scene it started from, so a step that changed pixels is stepped back. The undo is
        # allowed to refuse - pixels can move without being an undoable step of their own.
        if wire.call("doc.get", {})["checksum"] != before:
            try:
                wire.call("edit.undo")
            except wd.WireError:
                pass
        wire.call("select.rect", {"rect": list(MARK)})
    raise wd.WireError(f"nothing in the dock filed text under '{field}'; the last word it took was "
                       f"refused with: {last_error or 'nothing - no widget accepted the typing'}")


def verb_claims(p: Probe, wire: wd.Wire, inj: wi.Injector, row: dict, box_y: int, marker: str,
                keysym: int) -> None:
    verb, field, label = row["id"], row["ask"][0], str(row["label"])
    intent = wire.call("ai.intent", {})
    p.check(intent.get("recipe") == row["recipe"],
            f"{verb}: typing into its box selected its recipe", str(intent.get("recipe")))
    p.check(marker in str(intent.get("vars", {}).get(field, "")),
            f"{verb}: the box's words reached the intent", str(intent.get("vars")))

    before = len(jobs(wire))
    inj.modified_key(keysym, 0xffe3, 0xffe1)
    for _ in range(60):
        if len(jobs(wire)) > before:
            break
        time.sleep(0.25)
    fresh = [job for job in jobs(wire)[before:] if job.get("recipe") == row["recipe"]]
    if not p.check(bool(fresh), f"{verb}: its key queued a job carrying what was typed",
                   f"{len(jobs(wire)) - before} job(s) after the key"):
        return
    waited = wire.call("job.wait", {"id": fresh[-1]["id"], "timeout": 30})
    prompt = next((value for name, value in
                   ((pair[0], pair[1]) for pair in waited["request"]["fields"])
                   if name == "prompt"), "")
    p.check(waited["state"] == "succeeded", f"{verb}: the job succeeded",
            str(waited.get("failure") or waited.get("error")))
    p.check(marker in prompt, f"{verb}: the request prompt carries the typed words",
            prompt[:70])
    landed = str(waited.get("landing", {}).get("landed_layer", ""))
    p.check(landed == f"{label} {fresh[-1]['id']}",
            f"{verb}: the answer landed on a layer named for the verb and the job",
            str([layer["name"] for layer in wire.call("layer.list")["layers"]]))
    wire.call("edit.undo")


def run(wire: wd.Wire, inj_display: str, p: Probe) -> None:
    inj = wi.Injector(inj_display)
    inj.focus_app_window(WIDTH, HEIGHT)
    try:
        wire.call("select.rect", {"rect": list(MARK)})
        rows = {row["id"]: row for row in wd.field(wire.call("ai.verbs"), "verbs")}
        # Rule E through a real key first: with nothing typed, the key must start nothing. Done before
        # the sweep, which can itself land clicks on wordless verbs.
        for verb, keysym in (("fill", 0x66), ("change", 0x63)):
            before = len(jobs(wire))
            inj.modified_key(keysym, 0xffe3, 0xffe1)
            time.sleep(0.6)
            p.check(len(jobs(wire)) == before,
                    f"{verb}: pressed with nothing typed, no job started",
                    str([job["id"] for job in jobs(wire)[before:]]))
        box_x = WIDTH - SWEEP_EDGE
        for verb, keysym in (("fill", 0x66), ("change", 0x63)):
            row = rows[verb]
            p.check(bool(row["ask"]), f"{verb}: its recipe asks for words", str(row["ask"]))
            box_y, marker = find_words_box(wire, inj, row["ask"][0], box_x)
            print(f"  ..   {verb}: its box is at y={box_y}, typed marker '{marker}'")
            verb_claims(p, wire, inj, row, box_y, marker, keysym)
    finally:
        inj.close()


def skip_reason(args: argparse.Namespace) -> str | None:
    if not pathlib.Path(args.bin).exists():
        return f"{args.bin} does not exist"
    return wd.display_skip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=pathlib.Path("build/noise-edit"))
    args = parser.parse_args()
    reason = skip_reason(args)
    if reason:
        print(f"SKIP: {reason}")
        return 0

    args.bin = pathlib.Path(args.bin)
    try:
        display = wd.ensure_display(WIDTH, HEIGHT)
    except wd.WireError as exc:
        print(f"SKIP: {exc}")
        return 0

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="panel-words-"))
    from PIL import Image  # noqa: PLC0415  (skipped above when it is missing)
    doc = tmp / "scene.png"
    Image.new("RGB", (320, 240), (90, 90, 90)).save(doc)
    runtime = tmp / "runtime"
    runtime.mkdir()
    root = pathlib.Path(__file__).resolve().parent.parent
    p = Probe()
    print("the panel's own door: injected keys into the words box, then the verb's key")
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, doc, open(tmp / "app.stderr", "wb"),
                        {"NOISE_AI_FIXTURES": str(root / "tests" / "fixtures" / "http"),
                         "THENOISE_URL": None, "THENOISE_API_KEY": None})
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
        wd.wait_for_fit(wire)
        try:
            run(wire, inj_display=display, p=p)
        except wd.WireError as exc:
            p.check(False, "the panel answered the wire", str(exc))
    finally:
        wd.shutdown(proc, wire)
    print("FAILURES" if p.failed else "all claims held", f"({tmp})")
    return 1 if p.failed else 0


if __name__ == "__main__":
    sys.exit(main())
