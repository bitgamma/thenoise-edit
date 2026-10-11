# The whole AI path, driven through the app against the dev box

Measured 2026-10-11 with `tools/ai_job_probe.py --live --max-images 2` — two image requests, run
twice in the session (2 spent in total). The probe starts a real `noise-edit`, drives it over the
debug wire through `server.connect → ai.preview → ai.run → job.wait → job.apply`, and asserts only
contract and invariants. The server's address is absent here and everywhere (`AGENTS.md` §7); it is
printed at runtime by the app.

## What the box answered

`/health`: `version 2026.41.0~1639.29b21e73`, `model_loaded: Flux-2-Klein-9B-TheNoise`,
`max_models.image == 1`. `/models` offered six `recipe: thenoise` entries, all downloaded:
Klein-4B, Klein-9B (edit), Qwen-Image-2.1-INT8 (edit), Krea-2-VAE, Krea-v0.3.1-INT8,
RealESRGAN-4xplus. The `edit` role resolved to **`Flux-2-Klein-9B-TheNoise`**, by preference order
in `roles.json`, and that model was already resident — so the run made no `/v1/load` call and the
shared box kept the model it had. The probe would have put the resident model back afterwards if
the resolution had asked for a different one.

## The request, as it went out

`POST /images/edits`, recipe `inpaint-remove`, `size 256x256` (a 120×120 selection grown by the
table's 35 % context and snapped to the 16 px multiple), `steps 4`, `cfg_scale 1.0`, explicit
`seed`, `response_format b64_json`, multipart with the prepped PNG. **No `mask` field**, and the
field list was exactly the tier-1 row the provider table states. The answer came back
`256×256` — the size asked for, so `answered_as_requested` was true and the compositor had nothing
to resample.

| request | wall | note |
|---|---|---|
| first, model resident | 3.4 s | 256×256, 4 steps |
| identical repeat (same seed, same input blob) | 0.7 s | the **server's** cache answered |

The second number is the reason `LatencyModel` refuses to learn from a reply under one second: fed
to the EMA it would make every future job on that `(model, size, steps)` look instant.

## Landing, and the defect this run existed to find

Both runs reported the same geometry and both held: `pixels_changed` equalled the whole-image diff
(19 276 then 19 881), zero pixels differed outside the reported `coverage_bounds`, and undo returned
the document byte for byte.

The difference between the two runs was the point. Measured in Lab on the composited document, the
patch the model returned was **9.0 L units lighter than the ring it landed in**:

| | patch core L | ring L | \|ΔL\| |
|---|---|---|---|
| first run | 53.75 | 44.74 | **9.01** |
| second run, after the fix | 44.61 | 44.53 | **0.08** |

The first run's `colour_match` had never fired: `arg_bool` in `app/command_args.cpp` wrote `false`
into its output whenever the key was absent, so `job.apply` and `ai.compose` — called without the
flag, which is what the docs tell a caller to do — ran with the correction every edit recipe asks
for switched off, while `policy` reported whatever the caller had been handed. The unit tests stayed
green because their fixtures are flat fields: matching a flat answer to a flat ring reproduces the
ring, so "correction ran" and "correction did not run" both look like a composite that worked. The
live answer is a picture, not a fixture, and a light square in it.

Fixed by having an absent optional argument leave the caller's value alone, plus `test_app`'s
`test_an_option_left_out_keeps_the_policy_the_table_asked_for`, which paints a frame with structure
so the correction has something to match and asserts both the report and the landed pixels.

## What the picture still shows

With the correction running, the lightness step is gone (0.08 L), and what remains is **texture**:
the replacement region carries a finer, busier grain than the frame it sits in (L std 4.4 inside vs
2.0 outside in the first run), and a faint rectangular edge survives the 8 px feather. Neither is a
client bug — they are recipe questions (`prep` choice, how much context the crop carries, whether
`inpaint-remove` should ask for more steps at small sizes), which is the tuning pass this probe
record is the starting point for. The grain is worth measuring again after any `prep` change, since
`prep: blur` at 256² is a plausible cause of a smooth-then-regranulated patch.
