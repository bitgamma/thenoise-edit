#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Click each selection AI action while no server is configured; failure must be observable."""
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

WIDTH, HEIGHT = 1100, 800
VERBS = ("ai.heal", "ai.remove", "ai.fill", "ai.change")


def click(injector: wi.Injector, x: float, y: float) -> None:
    injector.move(round(x), round(y))
    injector.button(1, True)
    injector.button(1, False)
    injector.move(round(x), round(y))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    args = parser.parse_args()
    skip = wd.display_skip()
    if skip:
        print(f"SKIP: {skip}")
        return 0
    temp = pathlib.Path(tempfile.mkdtemp(prefix="ai-offline-ui-"))
    runtime = temp / "runtime"
    runtime.mkdir()
    display = wd.ensure_display(WIDTH, HEIGHT)
    proc = wire = injector = None
    stderr = (temp / "app.stderr").open("wb")
    failed = []
    try:
        proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, stderr,
                            {"THENOISE_URL": None, "THENOISE_API_KEY": None,
                             "NOISE_AI_FIXTURES": None}, start_dialog=False)
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0), timeout=60)
        injector = wi.Injector(display)
        injector.focus_app_window(WIDTH, HEIGHT)
        wire.call("doc.new", {"width": 512, "height": 512})
        wire.call("select.rect", {"rect": [120, 100, 200, 140]})
        wd.settle(wire)
        baseline_job_count = len(wire.call("job.list")["jobs"])
        initial = wire.call("ai.intent")
        if initial.get("configured", False) or initial.get("connected", False):
            failed.append("no-server test unexpectedly has a configured/connected AI backend")
        for command in VERBS:
            # The notice for a previous press should be dismissed through the existing intent seam.
            wire.call("ai.intent", {"connect": False})
            wd.settle(wire)
            bar = wire.call("verb.bar")
            buttons = bar["buttons"]
            matches = [i for i, b in enumerate(buttons) if b["command"] == command]
            if not bar["shown"] or len(matches) != 1:
                failed.append(f"missing button for {command}: {bar}")
                continue
            index = matches[0]
            if buttons[index]["ready"]:
                failed.append(f"{command} unexpectedly ready without a server")
            x, y, width, height = bar["screen"]
            if command == VERBS[0]:
                canvas = [round(c) for c in wire.call("app.state")["canvas"]]
                before_canvas = wire.call(
                    "screenshot", {"region": canvas, "name": "offline-before"})
            click(injector, x + width * (index + 0.5) / len(buttons), y + height / 2)
            wd.settle(wire)
            if command == VERBS[0]:
                after = wire.call("screenshot", {"region": canvas, "name": "offline-notice"})
                with Image.open(before_canvas["path"]) as first, Image.open(after["path"]) as second:
                    pixels = ImageChops.difference(first.convert("RGB"), second.convert("RGB"))
                    changed = sum(max(rgb) > 50 for rgb in pixels.getdata())
                if changed < 1000:
                    failed.append(f"AI failure did not draw a visible canvas notice: {changed} changed pixels")
            intent = wire.call("ai.intent")
            if not intent.get("error"):
                failed.append(f"{command} click yielded no observable AI error")
            if wire.call("verb.bar").get("ask") is not None:
                failed.append(f"{command} opened a prompt even though the AI backend is absent")
            if len(wire.call("job.list")["jobs"]) != baseline_job_count:
                failed.append(f"{command} queued an AI job without a server")
        if failed:
            for item in failed:
                print("FAIL:", item)
            return 1
        print("Offline Heal/Remove/Fill/Change clicks report actionable failure and queue no jobs.")
        return 0
    finally:
        if injector is not None:
            injector.close()
        if proc is not None:
            wd.shutdown(proc, wire)
        stderr.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
