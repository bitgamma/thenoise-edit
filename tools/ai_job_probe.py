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
# `job.wait` blocks its connection until the job settles, so the socket has to outlast the wait: on the
# box a first ask includes loading the role's model, which is tens of seconds.
JOB_WAIT_S = 1800
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


def seed_and_resample_claims(p: Probe, wire: wd.Wire, row: dict, words: dict, seed: int) -> None:
    """The two facts a press owns that a recording cannot show: whether the ask can differ at all, and
    whether the region it was built from survived the trip."""
    asked = {"recipe": row["recipe"], "vars": words}
    first = wire.call("ai.preview", asked)
    second = wire.call("ai.preview", asked)
    p.check(first["seed"] != second["seed"], f'{row["id"]}: two asks take two seeds',
            f'{first["seed"]} vs {second["seed"]}')
    p.check(first["seed_source"] == "random", f'{row["id"]}: an unpinned ask draws',
            str(first["seed_source"]))
    # The multipart form answers anything over `ai::kMaxSeed` - a signed 32-bit integer - with 400
    # `must be an integer`, a range error wearing a type error's clothes. The app draws 64 bits, so the
    # bound belongs on the ask rather than in the engine's message.
    for name, seed in (("first", first["seed"]), ("second", second["seed"])):
        p.check(isinstance(seed, int) and 0 < seed <= 2147483647,
                f'{row["id"]}: the {name} seed is inside what the form carries', str(seed))
    p.check(first["request"]["blob"] == second["request"]["blob"],
            f'{row["id"]}: the seed is the only thing that moved',
            "the upload hash changed between two asks of the same region")

    # A seed on a press belongs to that press. Holding one across asks is `seed_pin`, and a press that
    # pinned the session would make every later ask identical - the defect this probe found.
    wire.call("ai.intent", {"seed_pin": seed})
    pinned = wire.call("ai.preview", asked)
    p.check(pinned["seed"] == seed and pinned["seed_source"] == "pinned",
            f'{row["id"]}: a pinned seed is the seed that goes out',
            f'{pinned["seed"]}/{pinned["seed_source"]}')
    state = wire.call("server.state")
    p.check(state["seed_pinned"] is True and state["seed"] == seed,
            f'{row["id"]}: the session reports the pin', str(state.get("seed_pinned")))
    one_press = wire.call("ai.preview", {**asked, "seed": seed})
    p.check(one_press["seed"] == seed and one_press["seed_source"] == "named",
            f'{row["id"]}: a named seed belongs to this press only',
            f'{one_press["seed"]}/{one_press["seed_source"]}')
    wire.call("ai.intent", {"seed_pin": None})
    p.check(wire.call("server.state")["seed_pinned"] is False,
            f'{row["id"]}: letting go returns to one seed per ask')
    p.check(wire.call("ai.preview", asked)["seed_source"] == "random",
            f'{row["id"]}: the next ask draws again')

    # A mark this small is nowhere near a pixel ceiling, so the answer is asked for at the region's own
    # size: a resample here would mean the ceiling was invented rather than measured.
    p.check(first["region"]["resampled"] is False, f'{row["id"]}: the region was not shrunk',
            f'crop {first["region"]["crop"]} asked {first["region"]["requested"]}')
    intent = wire.call("ai.intent", {"recipe": row["recipe"]})
    p.check(intent["quote"]["resampled"] is False, f'{row["id"]}: the price line says the same')


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
                   scene_before: np.ndarray, took: float) -> bool:
    verb, label = row["id"], str(row["label"])
    job_id = waited["id"]
    p.check(waited["state"] == "succeeded", f"{verb}: job {job_id} succeeded",
            f"{took:.1f}s failure={waited.get('failure')} error={waited.get('error', '')}")
    if waited["state"] != "succeeded":
        return False
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
    if row.get("scope") == "selection":
        bar = wire.call("verb.bar")
        p.check(bar["mode"] == "editing" and
                wire.call("layer.plane")["plane"] == "mask",
                f"{verb}: a landed edit exposes its result mask", str(bar["mode"]))
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
    return True


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
        seed_and_resample_claims(p, wire, row, words, args.seed)
        # The panel's door in the panel's own order: the row's box commits the words, then the verb
        # command - carrying nothing - presses. `ai.run` is deliberately nowhere in this file.
        wire.call("ai.intent", {"recipe": row["recipe"], "vars": words})
        started = time.time()
        ran = wire.call(row["command"], {"seed": args.seed})
        spent += 1
        job_id = ran["job"]["id"]
        waited = wire.call("job.wait", {"id": job_id, "timeout": JOB_WAIT_S if live else 30})
        landing_claims(p, wire, live, tmp, row, waited, scene_before, time.time() - started)
    return spent


def aspect_ratio(name: str) -> float:
    wide, high = name.split(":")
    return int(wide) / int(high)


def round_up(value: int, multiple: int) -> int:
    return ((value + multiple - 1) // multiple) * multiple


def frame_outside_the_band(p: Probe, wire: wd.Wire, tmp: pathlib.Path, tag: str, waited: dict,
                           scene_before: np.ndarray, frame: tuple[int, int, int, int]) -> None:
    """The aspect promise, whole-image: past the feather, the frame's edge is the document's edge.

    `compose` softens the coverage boundary by `feather_px`, and that ramp reaches about half of it
    outside the mask, so the banded box is what may move and everything beyond it may not. Asserted here
    instead of trusting `coverage_bounds`, which reports the crop the answer was compiled against and is
    larger than the frame whenever the frame is not already on the policy's grid.
    """
    report = waited.get("landing", {}).get("report", {})
    feather = int(report.get("composite", report).get("feather_px") or 0)
    fx, fy, fw, fh = frame
    after = export(wire, tmp / f"{tag}-frame.png")
    moved = np.abs(after.astype(int) - scene_before.astype(int)).any(-1)
    pad = feather // 2 + 1
    allowed = np.zeros_like(moved)
    allowed[max(fy - pad, 0):min(fy + fh + pad, moved.shape[0]),
            max(fx - pad, 0):min(fx + fw + pad, moved.shape[1])] = True
    p.check(int((moved & ~allowed).sum()) == 0,
            f"{tag}: nothing changed past the frame and its feather",
            f"{int((moved & ~allowed).sum())} pixels beyond {frame} plus feather {feather}")
    inside = np.zeros_like(moved)
    inside[fy + pad:fy + fh - pad, fx + pad:fx + fw - pad] = True
    p.check(int((moved & inside).sum()) > 0, f"{tag}: the answer reached the inside of the frame",
            f"{int((moved & inside).sum())} of {int(inside.sum())} pixels")


def generate_cell(p: Probe, wire: wd.Wire, live: bool, args: argparse.Namespace,
                  tmp: pathlib.Path, scene_before: np.ndarray, budget: int, model: str) -> int:
    """The document-scope verb: every frame the recipe offers, then the style that stands in for it.

    The geometry is checked before anything is asked for, and the answer then makes the same claims the
    marked-area verbs make - which here is the aspect promise itself, because the coverage *is* the frame.
    The colour-drift band is skipped: it measures an answer against the scene around a hole, and a
    generation has no scene to belong to.
    """
    spent = 0
    row = verb_rows(wire)["generate"]
    doc_h, doc_w = int(scene_before.shape[0]), int(scene_before.shape[1])
    p.check(bool(row["ready"]), "generate: the verb row says it could run", str(row.get("reason")))
    p.check(row["scope"] == "document", "generate: works on the document, not a marked area",
            str(row["scope"]))
    # The same rule as the edit role: the swap refusal guards a hand on a key, and this run intends it.
    if live and row.get("swap") and row.get("model"):
        wire.call("server.load", {"model": row["model"], "pinned": False})
    aspects = list(row.get("aspects") or [])
    p.check(len(aspects) > 0, "generate: the recipe offers frames to choose from", str(aspects))
    press_refuses_without_the_words(p, wire, row)
    words = words_for(row["ask"])

    for choice in [None] + aspects:
        tag = f"generate {'canvas' if choice is None else choice}"
        intent = wire.call("ai.intent", {"recipe": row["recipe"], "vars": words, "aspect": choice})
        p.check(intent["aspect"] == choice, f"{tag}: the choice is what the intent holds",
                str(intent["aspect"]))
        if live and spent >= budget:
            print(f"  skip {tag}: the --max-images {budget} budget is spent, and the box is shared")
            continue
        preview = wire.call("ai.preview", {"recipe": row["recipe"], "vars": words,
                                           "seed": args.seed, "aspect": choice})
        want = doc_w / doc_h if choice is None else aspect_ratio(choice)

        # The frame is what was chosen, so *it* holds the ratio - to half a pixel of its short side - and
        # fills the canvas on one axis. The crop is that frame grown onto the policy's grid and the
        # requested size is the crop snapped up, both of which may sit further off: a claim written
        # against those would fail on a correct frame.
        frame = preview["region"]["mask_bounds"]
        fx, fy, fw, fh = (int(v) for v in frame)
        slop = 16.0 / min(fw, fh)
        p.check(abs(fw / fh - want) <= want * slop, f"{tag}: the frame holds the ratio",
                f"{fw}x{fh} for {choice or f'{doc_w}x{doc_h}'}")
        p.check(fw == doc_w or fh == doc_h, f"{tag}: one axis of the frame runs the whole canvas",
                f"{fw}x{fh} of {doc_w}x{doc_h}")
        p.check(fx + fw <= doc_w and fy + fh <= doc_h, f"{tag}: the frame is inside the document",
                f"{fw}x{fh} at {fx},{fy}")
        crop = preview["region"]["crop"]
        cx, cy, cw, ch = (int(v) for v in crop)
        p.check(cx <= fx and cy <= fy and cx + cw >= fx + fw and cy + ch >= fy + fh,
                f"{tag}: the crop covers the frame it was grown from", f"{cw}x{ch} at {cx},{cy}")
        asked_w, asked_h = (int(v) for v in preview["request"]["size"].split("x"))
        p.check(asked_w == round_up(cw, 16) and asked_h == round_up(ch, 16),
                f"{tag}: the requested size is the crop on the 16px grid",
                f"asked {asked_w}x{asked_h} for a {cw}x{ch} crop")
        p.check(preview["request"]["model"] == model, f"{tag}: the request names the resolved model")
        p.check("aspect" not in request_fields(preview), f"{tag}: no aspect field reaches the server",
                "a frame is geometry, not a request field")
        p.check("size" in request_fields(preview), f"{tag}: the size is still sent explicitly",
                "an edit without it answers the model default")
        p.check(intent["quote"]["size"] == [asked_w, asked_h],
                f"{tag}: the status line quotes the size a press asks for",
                f"{intent['quote']['size']} vs {asked_w}x{asked_h}")

        started = time.time()
        ran = wire.call(row["command"], {"seed": args.seed, "confirm": True})
        spent += 1
        waited = wire.call("job.wait", {"id": ran["job"]["id"],
                                        "timeout": JOB_WAIT_S if live else 30})
        # The report says the same box the preview priced: this is the artefact a person checks the
        # saved layer and mask against, and a 16:9 run once reported the grid-grown crop as its own.
        landed_region = waited.get("landing", {}).get("report", {}).get("region", {})
        p.check(list(landed_region.get("mask_bounds") or []) == frame,
                f"{tag}: the report names the frame the answer was asked to fill",
                f"{landed_region.get('mask_bounds')} vs {frame}")
        # Before `landing_claims`, which undoes: the frame claim is about the document as the landing
        # left it.
        frame_outside_the_band(p, wire, tmp, tag, waited, scene_before, (fx, fy, fw, fh))
        landing_claims(p, wire, False, tmp, row, waited, scene_before, time.time() - started)

    variants = list(row.get("variants") or [])
    if not variants:
        return spent
    labels = {r["id"]: r["label"] for r in wd.field(wire.call("ai.recipes"), "recipes")}
    pick = variants[-1]
    intent = wire.call("ai.intent", {"recipe": pick["id"], "vars": words})
    p.check(intent["recipe"] == pick["id"], "generate style: the intent holds the variant",
            str(intent["recipe"]))
    p.check(verb_rows(wire)["generate"]["active"] == pick["id"],
            "and the row reports it as the recipe a press would run")
    p.check(intent["aspects"] == aspects, "and choosing a style keeps the frames on offer",
            str(intent["aspects"]))
    # The model behind each style is reported once, on the intent's `family`: a style that silently
    # swapped the model on a shared box is the expensive part of the choice.
    family = {member["id"]: member for member in intent["family"]}
    p.check(bool(family.get(pick["id"], {}).get("model")), "and says which model answers it",
            str(family.get(pick["id"])))
    if live and spent >= budget:
        print("  skip generate style: the --max-images budget is spent")
        return spent
    started = time.time()
    ran = wire.call("ai.generate", {"seed": args.seed, "confirm": True})
    spent += 1
    waited = wire.call("job.wait", {"id": ran["job"]["id"], "timeout": JOB_WAIT_S if live else 30})
    landing_claims(p, wire, False, tmp,
                   {"id": "generate", "label": labels.get(pick["id"], pick["id"]),
                    "recipe": pick["id"]},
                   waited, scene_before, time.time() - started)
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
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0 if live else 40.0),
                       timeout=JOB_WAIT_S + 120.0 if live else 60.0)
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
        # The refusal that asks before swapping a model is for a hand on a key: it protects someone from
        # a tens-of-seconds load they did not mean to cause. A probe means it, so it loads the role's own
        # model here, names that it did, and the `finally` below puts the box back - the alternative is a
        # live tier that refuses on every verb and reports a green run of nothing.
        if live and edit["model"] != resident_before:
            loaded = wire.call("server.load", {"model": edit["model"], "pinned": False})
            p.check(bool(loaded.get("ok", True)), "the box loaded the model the edit role resolves to",
                    f"{resident_before} -> {edit['model']}")

        before = export(wire, tmp / "before.png")
        spent = 0
        if args.only != "generate":
            spent = run_verbs(p, wire, live, args, tmp, edit["model"], before, args.max_images)
        if args.generate or args.only == "generate":
            photo = next(r for r in connected["roles"]["roles"] if r["role"] == "generate.photo")
            p.check(bool(photo["resolved"]), "the generate.photo role resolved",
                    f"model={photo['model']} ({photo['reason']})")
            spent += generate_cell(p, wire, live, args, tmp, before, args.max_images - spent,
                                   photo["model"])

        if not live:
            # There is no answer cache: the same request asked again is a second job that goes out
            # again, with its own answer file. Asserted on the recordings because the invariant is about
            # the app not shortcutting you, and both asks are answered identically.
            wire.call("select.rect", {"rect": list(MARK)})
            fill = verb_rows(wire)["fill"]
            wire.call("ai.intent", {"recipe": fill["recipe"], "vars": words_for(fill["ask"])})
            again = wire.call(fill["command"], {"seed": args.seed})
            second = wire.call("job.wait", {"id": again["job"]["id"], "timeout": 30})
            p.check(wire.call("verb.bar")["mode"] == "editing",
                    "repeat fill: the first answer enters result editing")
            rearmed = wire.call("verb.new_edit")
            p.check(rearmed["mode"] == "actions" and
                    wire.call("layer.plane")["plane"] == "pixels",
                    "repeat fill: New AI edit rearms the marked area")
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
    parser.add_argument("--generate", action="store_true",
                        help="also run the document-scope verb: each aspect the recipe offers, then a "
                             "style that stands in for it. Free against the recordings, one image each "
                             "against the box")
    parser.add_argument("--only", choices=("verbs", "generate"),
                        help="run one cell rather than both, so a live run can be aimed at what changed")
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
