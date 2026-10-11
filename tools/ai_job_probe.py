#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Watch the whole AI path on the running app: recipe in, request out, answer on the pixels.

The same script at two tiers. By default it points the app at the committed recordings, so it proves the
app's own wiring - discovery, role resolution, the compiled request, the queue, the landing - over the
wire and on screen, without a socket. With `--live` it talks to the dev box instead and asserts only what
holds whatever model answers: the request obeyed the policy, the answer landed inside its coverage, undo
gives the document back, and nothing was sent that the preview did not show.

    # hermetic
    python3 tools/ai_job_probe.py --bin build/noise-edit

    # live, small, and honest about the spend
    THENOISE_URL=... THENOISE_API_KEY=*** python3 tools/ai_job_probe.py --live --max-images 2

Nothing here asserts anything about image *content*: a diffusion model's output is not a golden file. The
timings it prints are for `docs/probes/` and are never asserted - the box is shared.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import sys
import tempfile
import time

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

W, H = 420, 300
RECIPE = "inpaint-remove"
VARS = {"target": "red marker", "context_desc": "graded backdrop"}


def scene() -> np.ndarray:
    """A graded backdrop with a hard-edged red marker in it: something for a mask to mean."""
    y, x = np.mgrid[0:H, 0:W].astype(float)
    u, v = 1 - x / W, 1 - y / H
    back = np.stack([40 + 70 * u + 30 * v, 60 + 40 * u + 60 * v, 90 + 25 * u + 70 * v], -1)
    grain = np.random.default_rng(11).normal(0.0, 5.0, back.shape[:2])[..., None]
    back = np.clip(back + grain, 0, 255)
    marker = (np.abs(x - 210) < 42) & (np.abs(y - 150) < 30)
    back[marker] = [236, 26, 34]
    return back.astype(np.uint8)


def digest(path: pathlib.Path) -> str:
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()


def export(wire: wd.Wire, path: pathlib.Path) -> np.ndarray:
    wire.call("doc.export", {"path": str(path)})
    return np.asarray(Image.open(path).convert("RGBA"))


def counts(before: np.ndarray, after: np.ndarray, bounds: list[int]) -> tuple[int, int]:
    """Pixels that differ, and how many of them lie outside the box the composite claimed."""
    diff = np.abs(before.astype(int) - after.astype(int)).any(-1)
    x, y, w, h = bounds
    inside = np.zeros_like(diff)
    inside[y : y + h, x : x + w] = True
    return int(diff.sum()), int((diff & ~inside).sum())


class Probe:
    def __init__(self) -> None:
        self.failed = 0

    def check(self, ok: bool, claim: str, detail: str = "") -> bool:
        print(("  ok   " if ok else "  FAIL ") + claim + (f"  {detail}" if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def request_fields(reply: dict) -> dict:
    return {pair[0]: pair[1] for pair in reply["request"]["fields"]}


def run_case(live: bool, args: argparse.Namespace, tmp: pathlib.Path, p: Probe) -> None:
    doc_png = tmp / "scene.png"
    Image.fromarray(scene(), "RGB").save(doc_png)
    runtime = tmp / ("live" if live else "recorded")
    runtime.mkdir()

    # Exactly one of the two, never both: a probe that means to replay must not reach the network by
    # accident, and one that means to ask the box must not be answered by a recording.
    extra = {"NOISE_AI_FIXTURES": None if live else str(args.fixtures)}
    stderr = open(runtime / "app.stderr", "wb")
    proc = wd.start_app(args, runtime, 1000, 700, doc_png, stderr, extra)
    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0 if live else 40.0))
        wd.wait_for_fit(wire)
        x0, y0, side = 150, 96, 120
        wire.call("select.rect", {"rect": [x0, y0, side, side]})

        state = wire.call("server.state")
        p.check(state["tables"], "the role table and the recipes loaded", str(state.get("recipes_error", "")))
        connected = wire.call("server.connect")
        p.check(bool(connected.get("connected")), "server.connect reached a server",
                str(connected.get("server", "")))
        edit = next(r for r in connected["roles"]["roles"] if r["role"] == "edit")
        p.check(bool(edit["resolved"]), "the edit role resolved",
                f"model={edit['model']} ({edit['reason']})")
        model = edit["model"]

        preview = wire.call("ai.preview", {"recipe": RECIPE, "vars": VARS, "seed": args.seed})
        fields = request_fields(preview)
        size = preview["request"]["size"]
        w_px, h_px = (int(v) for v in size.split("x"))
        p.check(preview["request"]["path"] == "/images/edits", "an edit goes to /images/edits")
        p.check(preview["request"]["model"] == model, "the request names the resolved model")
        p.check(w_px % 16 == 0 and h_px % 16 == 0, "the size obeys the 16px policy", size)
        p.check("mask" not in fields, "no mask field is sent",
                "the engine has none: region and prep are the spatial vocabulary")
        p.check(set(fields) == {"model", "image", "prompt", "size", "steps", "cfg_scale", "seed",
                                "response_format"} - {"image"},
                "the field list is exactly the tier-1 set", str(sorted(fields)))
        prompt = str(fields["prompt"])
        p.check("{{" not in prompt and VARS["target"] in prompt,
                "the recipe's placeholders were substituted before sending", prompt[:60] + "...")
        prepped = pathlib.Path(preview["prepped"])
        p.check(prepped.exists(), "the prepped region is on disk", prepped.name)
        with Image.open(prepped) as handle:
            p.check(handle.size == (w_px, h_px), "the uploaded region is the size requested",
                    f"{handle.size} vs {size}")

        # The box is shared and holds one image model, so whatever was resident when the probe
        # started goes back there whether or not the checks below pass.
        resident_before = str(connected.get("model_loaded") or "")

        before_path = tmp / ("live-before.png" if live else "before.png")
        before = export(wire, before_path)

        started = time.time()
        run = wire.call("ai.run", {"recipe": RECIPE, "vars": VARS, "seed": args.seed})
        job_id = run["job"]["id"]
        # A resolved model that is not the resident one costs a swap first: tens of seconds,
        # sometimes minutes. This is the wait the UI will show an ETA for.
        waited = wire.call("job.wait", {"id": job_id, "timeout": 1800 if live else 30})
        took = time.time() - started
        p.check(waited["state"] == "succeeded", f"job {job_id} succeeded",
                f"{took:.1f}s failure={waited.get('failure')} error={waited.get('error', '')}")
        if waited["state"] != "succeeded":
            return
        dims = waited["result_dims"]
        p.check(dims[0] > 0 and dims[1] > 0, "an answer came back", str(dims))
        p.check(dims[0] % 16 == 0 and dims[1] % 16 == 0, "the answer obeys the size policy", str(dims))

        applied = wire.call("job.apply", {"id": job_id, "apply": True})
        report = applied["composite"]
        p.check(bool(report["composed"]) and report["pixels_changed"] > 0,
                "the answer composited onto the document",
                f"changed={report['pixels_changed']} feather={report['feather_px']} "
                f"coverage={report['coverage_pixels']}")
        after = export(wire, tmp / ("live-after.png" if live else "after.png"))
        changed, outside = counts(before, after, report["coverage_bounds"])
        p.check(changed == report["pixels_changed"],
                "the report describes the document", f"diff={changed} report={report['pixels_changed']}")
        p.check(outside == 0, "nothing changed outside the coverage the job claimed",
                f"{outside} pixels beyond {report['coverage_bounds']}")

        wire.call("edit.undo")
        undone = export(wire, tmp / ("live-undone.png" if live else "undone.png"))
        p.check((after != undone).any() and (before == undone).all(),
                "undo gives the document back byte for byte")

        if not live:
            again = wire.call("ai.run", {"recipe": RECIPE, "vars": VARS, "seed": args.seed})
            second = wire.call("job.wait", {"id": again["job"]["id"], "timeout": 30})
            p.check(bool(second["from_cache"]), "the same request is answered from the cache",
                    f"job {again['job']['id']}")
            stats = wire.call("cache.stats")
            p.check(stats["hits"] >= 1 and stats["entries"] >= 1, "the cache counts what it served",
                    json.dumps({k: stats[k] for k in ("entries", "hits", "misses")}))
            # The request that went out is the one the cache keys on, and the recording says which model
            # answered: a cache hit that was really a second request would be a wait nobody asked for.
            p.check(second["cache_key"] == waited["cache_key"],
                    "the repeat request keyed on the same bytes")
        print(f"  shots: {before_path.parent}")
    finally:
        settled = ""
        if live and wire is not None:
            settled = str(wire.call("server.state").get("model_loaded") or "")
        if live and wire is not None and resident_before and settled != resident_before:
            try:
                loaded = wire.call("server.load", {"model": resident_before, "pin": True})
                print(f"  ok   the model the box was running is back: {resident_before} "
                      f"({loaded.get('elapsed_ms', 0)} ms)")
            except Exception as exc:  # a failed restore is said out loud, never hidden
                p.failed += 1
                print(f"  FAIL could not put {resident_before} back: {exc}")
        wd.shutdown(proc, wire)
        stderr.close()


def skip_reason(args: argparse.Namespace, live: bool) -> str | None:
    if not pathlib.Path(args.bin).exists():
        return f"{args.bin} does not exist"
    if not (os.environ.get("DISPLAY") or shutil.which("xvfb-run")):
        return "no DISPLAY and no xvfb-run, so nothing can render"
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        return "numpy/Pillow are not installed, so the fixture cannot be built"
    if live and not (os.environ.get("THENOISE_URL") and os.environ.get("THENOISE_API_KEY")):
        return "no THENOISE_URL / $THENOISE_API_KEY, so there is no dev box to ask"
    if not live and not pathlib.Path(args.fixtures).is_dir():
        return f"no recordings at {args.fixtures}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=pathlib.Path("build/noise-edit"))
    parser.add_argument("--fixtures", default=pathlib.Path(__file__).resolve().parent.parent
                        / "tests" / "fixtures" / "http")
    parser.add_argument("--live", action="store_true",
                        help="ask the dev box instead of the recordings")
    parser.add_argument("--max-images", type=int, default=2,
                        help="how many requests --live may make (each recipe run is one)")
    parser.add_argument("--seed", type=int, default=1234)
    args = parser.parse_args()

    reason = skip_reason(args, args.live)
    if reason:
        print(f"SKIP: {reason}")
        return 0
    if args.live and args.max_images < 1:
        print("SKIP: --max-images 0 means nothing may be asked")
        return 0

    args.bin = pathlib.Path(args.bin)
    args.fixtures = pathlib.Path(args.fixtures)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="ai-job-"))
    p = Probe()
    print(f"{'live: the dev box' if args.live else 'recorded: tests/fixtures/http'}")
    run_case(args.live, args, tmp, p)
    if p.failed:
        print(f"{p.failed} claim(s) failed ({tmp})")
        return 1
    print(f"all claims held ({tmp})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
