# The live server, probed for its contract

Measured 2026-10-09 against the dev box (`$THENOISE_URL`, `recipe: thenoise`), 9 image requests,
by `tools/tn_probe.py` — ctest `tn_probe_live`, `live`-labelled, skipped with a loud reason when
`THENOISE_URL`/`THENOISE_API_KEY` are not exported. Nothing here asserts image *content*; the
assertions are the ones the client branches on. Timings are records from one shared machine and
rotate with it.

The server's address is deliberately absent. It is printed at runtime by whatever talks to it and
never written down (`AGENTS.md` §7).

## Discovery

`GET /health` → `status: ok`, `version 2026.41.0~1639.29b21e73`, `model_loaded: None`, and
`max_models.image == 1`: **one image model resident at a time**, which is the fact the whole
swap/pin/warm-up UX is built around.

`GET /models` returned 11 entries, 3 of them `recipe: thenoise` with an `image` label:

| id | labels | downloaded | GB | `image_defaults` |
|---|---|---|---|---|
| `Flux-2-Klein-9B-TheNoise` | `image,edit` | ✓ | 32.5 | steps 4, cfg 1.0, 1024x1024 |
| `Krea-v0.3.1-INT8` | `custom,image` | ✓ | 21.7 | steps 8, cfg 1.0, 1024x1024 |
| `Krea-2-VAE` | `custom,image` | ✓ | 33.0 | steps 8, cfg 1.0, 1024x1024 |

`GET /models?show_all=true` returned 163. Two things follow, and both are load-bearing for
`roles.toml` (since renamed `roles.json` — the format decision moved to JSON
before any such file shipped; the ranking below is what it recorded):

* **The list is what this box holds, today.** The previous session's run on the same URL saw 187
  entries, `Flux-2-Klein-4B-TheNoise` present and `Flux-2-Klein-9B-TheNoise` not — the reverse of
  now. A probe or a role that hard-codes one id goes red for a reason that is not a defect, which
  is why `tn_probe.py` now resolves the `edit` role from `recipe + labels + downloaded` and takes
  the smallest download, with `--model` as the override for measuring a named model. The app's
  registry resolves the same way.
* **`image` alone does not mean a distinct model.** `Krea-2-VAE` is a VAE-swap alias with its own
  defaults and no machine-readable flag separating it from a real candidate, so preference order
  has to name candidates rather than trust a label set. `custom` here marks the Krea family —
  which on this box carries **no `edit` label**, so the `edit` role has exactly one resolution.
* `image_defaults` per entry is the authoritative source for role defaults (steps, cfg, native
  size). It is not the same for Klein (4 steps) and Krea (8), so a recipe must not carry a
  number it could read.

## Timings (records, not assertions)

Warm model after the first request; steps as the model's own default; the seed varies per
request because an identical request is answered from the server's cache (below).

| job | time |
|---|---|
| generate 512x512, 4 steps — first request, model not loaded | 32.0 s |
| generate 1024x1024, 4 steps | 14.4 s |
| edit 512x512, 4 steps, explicit `size` | 7.4 s |
| edit 512x512, 4 steps, **`size` omitted** | **30.3 s → 1024x1024** |
| upscale 512x512 → 2048x2048 (`RealESRGAN-4xplus-TheNoise`) | 7.4 s |

The first row is a model load, not a generation: `model_loaded` was `None` and 32.5 GB had to be
mapped. That is the price `POST /v1/load` + warm-up exists to move out of the user's way, and it
is why the EMA has to be seeded per model rather than per server.

## Contract findings

* **`size` is honoured and omitting it is expensive.** An edit asked for 512x512 returns 512x512.
  The same edit with `size` omitted returns the *model default* 1024x1024 — not the reference
  image's size — and costs 30.3 s against 7.4 s. The region compiler's "always send an explicit
  size" is therefore not a preference; it is a 4x cost difference and a wrong-dimension result.
  (This is the second measurement of it, so the old doc claim that the reference image sets the
  output size is dead.)
* **Requested sizes snap to a multiple of 16, silently.** 16, not 64: `size_policy.multiple = 16`,
  and a request is never refused for an awkward multiple — it comes back different. The compiler
  must expect output dims ≠ requested dims (the resample already handles it) and should send
  multiples of 16 itself so the crop it planned is the crop it gets.
* **The server caches responses.** Two identical requests returned byte-identical PNGs; the
  earlier session measured an identical request answered in 0.1 s. A timing probe must vary the
  seed (this one does) and our own request cache is a replay convenience, not proof that a model
  ran. *Open question for the human: is that cache intended, and is it configurable?*
* **Bad field values are accepted, not rejected.** `seed: "not-a-number"` → 200. A typo in a field
  name means "the default", silently. The request builder is the only place a field can be added
  (`AGENTS.md` §4) and it has to validate its own values, because the server will not.
* **Failure shapes to branch on**, both confirmed:
  * unknown model → `404` `{"error":{"code":"model_not_found","message":…,"param":"model","requested_model":…}}`
  * missing `prompt` → `400` `{"error":{"message":"Missing 'prompt' field in request","type":"invalid_request_error"}}`
* **The upscaler is served without being listed.** `/models` held nothing labelled `upscaling`,
  yet `POST /images/upscale` with `RealESRGAN-4xplus-TheNoise` worked (512² → 2048², 7.4 s). So a
  role must not be gated on discovery: attempt the endpoint, map the refusal. Discovery is a
  preference input here, not a capability verdict.

## What it changed

`docs/AI-BACKEND.md` — size policy 64 → 16 with the silent snap, the omitted-size behaviour, the
response cache and what it does to the ETA EMA, per-box discovery and `show_all=true`, upscaler
gating, self-validation of request fields, and the probe harness section (it exists now).
`tools/tn_probe.py` — role resolution instead of a baked-in model id, budget exhaustion reported
as a skip rather than a pass, the host kept out of the output.

## Not measured today

Prep ablation (`none|average|blur|pixelate|white`), drift pixel-diff of an edit over a large
region, cancel mid-request, and the multi-image/`n` behaviour. Each costs images; the prep
ablation is the one that feeds M4's recipe wording and is worth scheduling when the box is idle.
