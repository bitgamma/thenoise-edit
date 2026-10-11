# Colour drift on the dev box — one image, the scene that showed the bug

`python3 tools/ai_job_probe.py --bin build-release/noise-edit --live --max-images 1`
(registered as `ai_job_probe_live`, `-L live`). One request, 3.5 s of model time. The box was reached
through `$THENOISE_URL`; nothing here records where it is.

## Why this scene

The bug was a returned patch painted **far darker** than the frame it was cut out of, and the hermetic
fixtures could not see it: a flat recorded answer compared against a flat fill hides a dark patch as
readily as it shows one. So the scene is a grainy graded backdrop (blue-grey, per-pixel noise, two
gradients) with a hard-edged red marker inside the marked area, and the measurement is luminance:

- **patch** = the answer, inside the coverage, inset 12 px so the feather's edge is not being measured;
- **context** = a 24 px ring around the coverage taken from the **document before** the answer — the
  backdrop the model was supposed to continue, with the marker excluded by construction;
- **ratio** = patch mean / context mean. The estimator's own decision is printed and never required:
  it refuses to correct when the paired spread says there is nothing to correct.

## What the box answered

| fact | value |
|---|---|
| edit role | `Flux-2-Klein-9B-TheNoise`, already resident — no swap was needed or caused |
| compiled request | `/images/edits`, `256x256` (policy: multiple of 16), fields exactly the tier-1 set, no `mask` field |
| prompt | the wordless one: `Remove the marked content from this image.` + the context line |
| job | `j0001 succeeded` in **3.5 s** |
| landing | arrived by itself: layers `['Layer 1', 'Remove j0001']`, no `job.apply` pressed |
| compose | `pixels_changed=20620`, `coverage_pixels=20620`, `feather=8`, `colour_corrected=True` |
| pixel-lock | `visible=18023` of `written=20620`, and **0 pixels** outside `[134, 88, 152, 136]` |
| **drift** | patch mean **106.6** vs context ring mean **101.5** → **ratio 1.05** |
| undo | `Ctrl+Z`-equivalent returned the document byte for byte |

The bound asserted is wide — `0.55 … 1.8`, "believably the same scene" — because a model may legitimately
put something shadowed or bright into a hole. Measured: **1.05**. The behaviour this guards is the one that
was shipped a week ago, which measured in the 0.2s against the same backdrop: not "slightly off", not a
tuning matter, the wrong picture.

Looking at it confirms the numbers: the marker is gone, the gradient continues through where it was, and
the grain survives the round trip — no dark square, no seam, no smeared blur disc.

## Two things this run says about the box, not about us

- `roles.json` prefers `Flux-2-Klein-9B-TheNoise` for `edit`; `tools/tn_probe.py` picks the *smallest*
  edit-capable model (`Flux-2-Klein-4B-TheNoise`) to keep its own cost down. Both are defensible and they
  disagree, which is fine while each says which it chose — a probe that silently inherited the other's
  pick would be measuring a different box.
- 3.5 s at 256×256 with 4 steps on the resident model is the number the ETA is allowed to quote only
  after it has seen that triple; the latency model still says "no timing for this size" until then, and
  the status line says nothing rather than guessing.
