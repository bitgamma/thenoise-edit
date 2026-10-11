#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Watch the whole AI path on the running app: verb in, request out, answer on the pixels.

The same script at two tiers. By default it points the app at the committed recordings, so it proves the
app's own wiring - discovery, role resolution, the compiled request, the queue, the landing - over the
wire and on screen, without a socket. With `--live` it talks to the dev box instead and asserts only what
holds whatever model answers: the request obeyed the policy, the answer landed inside its coverage, undo
gives the document back, and nothing was sent that the preview did not show.

    # hermetic
    python3 tools/ai_job_probe.py --bin build/noise-edit

    # live, small, and honest about the spend
    THENOISE_URL=... THENOISE_API_KEY=*** python3 tools/ai_job_probe.py --live --max-images 3

Every verb that works on a marked area is driven through its own command - `ai.remove`, `ai.fill`,
`ai.change` - each with the sequence a person has: mark, type the words the verb asks for if it asks for
any, press. Nothing is invoked through `ai.run` behind the verb's back, because the verb *is* the product.

Nothing here asserts anything about image *content*: a diffusion model's output is not a golden file. The
timings it prints are for `docs/probes/` and are never asserted - the box is shared.
"""
from __future__ import annotations

import argparse
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
MARK = [150, 96, 120, 120]
# One line per verb, and one sentence per field the verb's recipe asks for - `prompt` (Fill) and
# `instruction` (Change). Remove is here too and asks for nothing, which is asserted rather than
# assumed: a recipe that grew an ask field since this table was written shows up as a refusal, not as a
# silently blank prompt. The label the answer's layer is named for comes from `ai.verbs`, never from
# here, so renaming a recipe cannot make this probe agree with itself.
SENTENCES = {
    "prompt": "a brass reading lamp with a green shade",
    "instruction": "the marker becomes a waxing moon, same lighting",
    "context_desc": "graded backdrop",
}


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


def verb_rows(wire: wd.Wire) -> dict:
    """The verb table as the window sees it, keyed by verb id. Read again whenever readiness matters."""
    return {row["id"]: row for row in wd.field(wire.call("ai.verbs"), "verbs")}


def request_fields(reply: dict) -> dict:
    return {pair[0]: pair[1] for pair in reply["request"]["fields"]}


def words_for(ask: list[str]) -> dict:
    """What a person types into the row: one value per field the recipe asked for."""
    return {name: SENTENCES.get(name, "the thing the recipe asked about") for name in ask}


def preview_claims(p: Probe, wire: wd.Wire, row: dict, words: dict, seed: int, model: str) -> None:
    """What a press would send, checked before it costs anything. Field policy and geometry, per verb."""
    preview = wire.call("ai.preview", {"recipe": row["recipe"], "vars": words, "seed": seed})
    fields = request_fields(preview)
    size = preview["request"]["size"]
    w_px, h_px = (int(v) for v in size.split("x"))
    verb = row["id"]
    p.check(preview["request"]["path"] == "/images/edits", f"{verb}: an edit goes to /images/edits",
            preview["request"]["path"])
    p.check(preview["request"]["model"] == model, f"{verb}: the request names the resolved model")
    p.check(w_px % 16 == 0 and h_px % 16 == 0, f"{verb}: the size obeys the 16px policy", size)
    p.check("mask" not in fields, f"{verb}: no mask field is sent",
            "the engine has none: region and prep are the spatial vocabulary")
    p.check(set(fields) == {"model", "prompt", "size", "steps", "cfg_scale", "seed", "response_format"},
            f"{verb}: the field list is exactly the tier-1 set", str(sorted(fields)))
    prompt = str(fields["prompt"])
    p.check("{{" not in prompt, f"{verb}: every placeholder was substituted", prompt[:60] + "...")
    for name, value in words.items():
        if name in row["ask"]:
            p.check(value in prompt, f"{verb}: the typed words reached the prompt",
                    "" if value in prompt else f"{name}={value!r} not in {prompt[:60]!r}")
    prepped = pathlib.Path(preview["prepped"])
    p.check(prepped.exists(), f"{verb}: the prepped region is on disk", prepped.name)
    if prepped.exists():
        with Image.open(prepped) as handle:
            p.check(handle.size == (w_px, h_px), f"{verb}: the upload is the size requested",
                    f"{handle.size} vs {size}")


def press_refuses_without_the_words(p: Probe, wire: wd.Wire, row: dict) -> bool:
    """Rule E on the running app: press the verb with nothing typed and it says so, naming both.

    The bare press is the window's own key, so a key that ran on words it was never given - or that
    silently reused whatever the previous verb had typed - is caught here rather than admired later.
    """
    if not row["ask"]:
        return True
    layers_before = len(wire.call("layer.list")["layers"])
    try:
        wire.call(row["command"], {})
    except wd.WireError as refused:
        said = str(refused)
        named = row["recipe"] in said and row["ask"][0] in said
        p.check(named, f"{row['id']}: pressed with no words, refused and named the field", said[:110])
        p.check(len(wire.call("layer.list")["layers"]) == layers_before,
                f"{row['id']}: the refusal cost no layer")
        return named
    p.check(False, f"{row['id']}: pressed with no words and it ran anyway",
            "an operation with no words is a coin toss, and it should have been refused")
    return False


def landing_claims(p: Probe, wire: wd.Wire, live: bool, tmp: pathlib.Path, row: dict, waited: dict,
                   scene_before: np.ndarray, took: float) -> None:
    verb, label = row["id"], str(row["label"])
    job_id = waited["id"]
    p.check(waited["state"] == "succeeded", f"{verb}: job {job_id} succeeded",
            f"{took:.1f}s failure={waited.get('failure')} error={waited.get('error', '')}")
    if waited["state"] != "succeeded":
        return
    dims = waited["result_dims"]
    p.check(dims[0] > 0 and dims[1] > 0, f"{verb}: an answer came back", str(dims))
    p.check(dims[0] % 16 == 0 and dims[1] % 16 == 0, f"{verb}: the answer obeys the size policy",
            str(dims))

    # Rule A: waiting was the last thing anyone did. The answer is already on the document, on its own
    # layer named for the recipe and the job, and the job carries the report of that landing - so the
    # pixel claims below are checked against the landing nobody pressed.
    landing = waited.get("landing", {})
    full = landing.get("report", {})
    report = full.get("composite", {})
    report.setdefault("coverage_bounds", full.get("coverage_bounds"))
    names = [layer["name"] for layer in wire.call("layer.list")["layers"]]
    p.check(bool(landing.get("landed")), f"{verb}: the answer landed itself, nothing pressed",
            str(landing.get("landed_layer")))
    p.check(str(landing.get("landed_layer", "")) == f"{label} {job_id}",
            f"{verb}: the layer is named for the verb and the job", str(names))
    applied = wire.call("job.apply", {"id": job_id})
    p.check(bool(applied.get("already_landed")),
            f"{verb}: asking for the same answer again adds no layer",
            str([layer["name"] for layer in wire.call("layer.list")["layers"]]))
    p.check(bool(report.get("composed")) and report.get("pixels_changed", 0) > 0,
            f"{verb}: the answer composited onto the document",
            f"changed={report.get('pixels_changed')} feather={report.get('feather_px')} "
            f"coverage={report.get('coverage_pixels')}")

    after = export(wire, tmp / f"{verb}-after.png")
    changed, outside = counts(scene_before, after, report["coverage_bounds"])
    # The report counts what was **written**, onto a fresh layer; the export shows the flattened result,
    # where a pixel the model reproduced exactly is no visible change. So the visible diff is a subset of
    # the write, never larger, and never outside the coverage.
    p.check(0 < changed <= report["pixels_changed"],
            f"{verb}: every visible change is one the report counted",
            f"visible={changed} written={report['pixels_changed']}")
    p.check(outside == 0, f"{verb}: nothing changed outside the coverage it claimed",
            f"{outside} pixels beyond {report['coverage_bounds']}")

    # Colour drift, on the scene shape that showed the bug: a textured backdrop with a hole in it. An
    # early build painted the answer far darker than the frame around it, which a flat fixture cannot
    # reveal at all. A model may legitimately put something darker or lighter in the marked area, so this
    # is a wide band around "believably the same scene", and the numbers are printed for docs/probes/
    # rather than tuned to. The estimator's own decision is reported, never required.
    if live:
        lum = lambda rgb: (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2])
        bx, by, bw, bh = (int(v) for v in report["coverage_bounds"])
        inset = 12
        patch = lum(after[by + inset : by + bh - inset, bx + inset : bx + bw - inset])
        ring_out = 24
        ry0, ry1 = max(by - ring_out, 0), min(by + bh + ring_out, scene_before.shape[0])
        rx0, rx1 = max(bx - ring_out, 0), min(bx + bw + ring_out, scene_before.shape[1])
        ring = lum(scene_before[ry0:ry1, rx0:rx1])
        patch_mean = float(patch.mean()) if patch.size else 0.0
        context_mean = float(ring.mean()) if ring.size else 0.0
        ratio = patch_mean / context_mean if context_mean > 0 else 0.0
        print(f"   {verb} drift: patch mean {patch_mean:.1f} vs context ring {context_mean:.1f} "
              f"-> ratio {ratio:.2f}; colour_corrected={report.get('colour_corrected')}")
        p.check(0.55 <= ratio <= 1.8, f"{verb}: the answer belongs to the scene it was cut from",
                f"ratio {ratio:.2f} outside [0.55, 1.8] - a patch painted dark, or blown out")

    wire.call("edit.undo")
    undone = export(wire, tmp / f"{verb}-undone.png")
    p.check((after != undone).any() and (scene_before == undone).all(),
            f"{verb}: undo gives the document back byte for byte")


def run_verbs(p: Probe, wire: wd.Wire, live: bool, args: argparse.Namespace, tmp: pathlib.Path,
              model: str, scene_before: np.ndarray, budget: int) -> int:
    """Every marked-area verb, end to end, each on a document returned to the scene before the next."""
    rows = verb_rows(wire)
    spent = 0
    for verb in ("remove", "fill", "change"):
        row = rows[verb]
        if live and spent >= budget:
            print(f"  skip {verb}: the --max-images {budget} budget is spent, and the box is shared")
            continue
        # Marked again for every verb: undoing the previous answer took the marked area back with it,
        # and a readiness read against no selection would report the bookkeeping, not the verb.
        wire.call("select.rect", {"rect": list(MARK)})
        # Read again with the area marked: `ready` is a statement about now.
        row.update(verb_rows(wire)[verb])
        p.check(bool(row["ready"]), f"{verb}: the verb row says it could run", str(row["reason"]))
        p.check(row["scope"] == "selection", f"{verb}: works on the marked area", str(row["scope"]))
        words = words_for(row["ask"])
        # Refused first, before anything is typed: the words are what the operation was told.
        press_refuses_without_the_words(p, wire, row)
        preview_claims(p, wire, row, words, args.seed, model)
        # The panel's door in the panel's own order: the row's box commits the words, then the verb
        # command - carrying nothing - presses. `ai.run` is deliberately nowhere in this file.
        wire.call("ai.intent", {"recipe": row["recipe"], "vars": words})
        started = time.time()
        ran = wire.call(row["command"], {"seed": args.seed})
        spent += 1
        job_id = ran["job"]["id"]
        waited = wire.call("job.wait", {"id": job_id, "timeout": 1800 if live else 30})
        landing_claims(p, wire, live, tmp, row, waited, scene_before, time.time() - started)
    return spent


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

        state = wire.call("server.state")
        p.check(state["tables"], "the role table and the recipes loaded",
                str(state.get("recipes_error", "")))
        # The app shook hands by itself. Nothing in this probe asked, so this is the host's own
        # behaviour - which lives in the frame loop, where no C++ test reaches it. It started life in
        # the panel, where a dock tab layout could swallow it entirely: an edit there that missed its
        # anchor still built, still tested green, and left the window with no way to connect at all.
        eager = wire.call("ai.intent", {})
        p.check(bool(eager.get("configured")), "a server is configured for this run",
                "set $THENOISE_URL (recordings count, and carry an .invalid address)")
        p.check(bool(eager.get("connect_attempted")), "the app asked for the handshake itself")
        p.check(bool(eager.get("connected")), "and the app connected before anyone asked it to",
                str(eager.get("reason") or eager.get("error")))

        connected = wire.call("server.connect")
        p.check(bool(connected.get("connected")), "server.connect reached a server",
                str(connected.get("server", "")))
        edit = next(r for r in connected["roles"]["roles"] if r["role"] == "edit")
        p.check(bool(edit["resolved"]), "the edit role resolved",
                f"model={edit['model']} ({edit['reason']})")
        # The box is shared and holds one image model, so whatever was resident when the probe started
        # goes back there whether or not the checks below pass.
        resident_before = str(connected.get("model_loaded") or "")

        before = export(wire, tmp / "before.png")
        spent = run_verbs(p, wire, live, args, tmp, edit["model"], before, args.max_images)

        if not live:
            # There is no answer cache: the same request asked again is a second job that goes out
            # again, with its own answer file. Asserted on the recordings because the invariant is about
            # the app not shortcutting you, and both asks are answered identically.
            wire.call("select.rect", {"rect": list(MARK)})
            fill = verb_rows(wire)["fill"]
            wire.call("ai.intent", {"recipe": fill["recipe"], "vars": words_for(fill["ask"])})
            again = wire.call(fill["command"], {"seed": args.seed})
            second = wire.call("job.wait", {"id": again["job"]["id"], "timeout": 30})
            first = wire.call(fill["command"], {"seed": args.seed})
            first_waited = wire.call("job.wait", {"id": first["job"]["id"], "timeout": 30})
            p.check(second["state"] == "succeeded" and first_waited["state"] == "succeeded",
                    "the repeat ask ran as its own job",
                    f"{second.get('failure')} {first_waited.get('failure')}")
            p.check(second["id"] != first_waited["id"], "a re-ask is a new job, not a reused one",
                    f"{second['id']} then {first_waited['id']}")
            p.check(bool(second["result"]) and second["result"] != first_waited["result"],
                    "and it wrote its own answer", str(second.get("result"))[-24:])
            wire.call("edit.undo")
            wire.call("edit.undo")
        print(f"  requests: {spent} this run; shots: {tmp}")
    finally:
        settled = ""
        if live and wire is not None:
            settled = str(wire.call("server.state").get("model_loaded") or "")
        if live and wire is not None and resident_before and settled != resident_before:
            try:
                # `pinned`, not `pin`: the command refuses an argument it does not declare, and a probe
                # that fails to put the box back leaves the next person a swapped model. Left unpinned,
                # which is the state a shared box should be in and what no status endpoint reports back.
                loaded = wire.call("server.load", {"model": resident_before, "pinned": False})
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
    if reason := wd.display_skip():
        return reason
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
    parser.add_argument("--max-images", type=int, default=3,
                        help="how many requests --live may make: one per verb, three verbs")
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
