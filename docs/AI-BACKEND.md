# AI Backend Contract

**Backend: [Lemonade Server](https://lemonade-server.ai), models on the `thenoise` recipe only.**
TheNoise (https://github.com/lemonade-sdk/thenoise) is the image engine: an instruction-based
generation/editing engine tuned for ROCm AMD hardware (Strix Halo `gfx1151` and other
ROCm-capable iGPUs/dGPUs). Lemonade exposes it through an OpenAI-compatible HTTP surface.

The app is a **client**. It does not host models. Typical deployment: this editor on a
laptop/desktop + Lemonade on a box on the LAN (localhost counts as the degenerate case).

## Model policy: roles, never model names

No code, recipe, test or UI string references a model id. Recipes name a **role**; the
registry resolves role → exactly one concrete model, per server profile. Later the user
picks among a role's candidates along its quality/speed axis — which is why the axis is
already in the data.

### Roles (MVP)

| Role | Capability predicate | Class of model it wants | Quality/speed axis | MVP |
|---|---|---|---|---|
| `generate.photo` | `recipe==thenoise` + `image` | photoreal, quality anchor | slow = more detail | ✓ |
| `generate.illustration` | `recipe==thenoise` + `image` | fast, stylised/anime-friendly | fast = iterate on prompts | ✓ |
| `edit` | `recipe==thenoise` + `image` + `edit` | instruction-based editing | a fast Klein ↔ a slower higher-fidelity Qwen checkpoint; the pair rotates as the box changes, which is why the order lives in `roles.json` and not here | ✓ |
| `upscale` | `recipe==thenoise` + `upscaling` | pixel-space upscaler | factor + fidelity | ✓ |
| `enhance` | `chat` + `vision` | prompt rewriting / scene description | cheap-and-fast ↔ capable | declared, **unused in MVP** |

### Resolution order (per profile, recomputed on connect)

1. **candidates** = `GET /v1/models` restricted to `downloaded: true` and to the role's `recipe`
   where the role states one - image roles say `thenoise`, `enhance` states no recipe at all, because
   which runner a vision model happens to use (`llama-cpp`, `vllm`, ...) is the server's business and a
   role that named one would go unresolved the next time the box changed;
2. narrowed by the role's capability predicate (`requires`, at least one label — a role with no
   predicate would resolve to whichever model of the recipe came first, so `roles.json` refuses it);
3. the **user's assignment** for that role, if it is still present and downloaded;
4. else the **app preference order** from `roles.json` — INT8-ConvRot before bf16, a
   KV-cache checkpoint preferred for `edit`, ties broken by smaller download;
5. else the role is **unresolved**: dependent commands render disabled with the reason, plus the
   exact `lemonade import <id>` when — and only when — the model the file prefers is offered by this
   server and simply not downloaded. Naming a model the server does not list would send the user to an
   import that fails, so in that case the answer is the reason and nothing else. Never fail at click
   time: `ai.roles` answers `{role, model, resolved, reason, fix, candidates, assignment}` for every
   role, which is what the disabled row is built from.

What the dev box taught this list (`probes/2026-10-09-live-server.md`):

* **The list is the box's, today.** Two runs on one URL returned 187 and 163 entries under
  `show_all=true`, and the two Klein sizes swapped which one was downloaded between them. Nothing
  may bake in a model id — resolution is `recipe + labels + downloaded` over what is answered now.
  `GET /models` is the short list (the useful one); `?show_all=true` is the whole catalogue.
* **`image_defaults` per entry is the authoritative source of role defaults** (`steps`,
  `cfg_scale`, `width`, `height`) and they differ per model — Klein 4 steps, Krea 8. A recipe that
  carries a step count is restating a fact it could read.
* **An `image` label does not mean a distinct model.** `Krea-2-VAE` appears as a
  `thenoise`+`image` entry with its own defaults; it is a VAE-swap alias. No field separates it
  from a real candidate, so preference order names candidates rather than trusting labels.
* **Discovery cannot veto a capability.** `/models` listed nothing labelled `upscaling` while
  `POST /images/upscale` served `RealESRGAN-4xplus-TheNoise` fine (512²→2048², 7.4 s). For the
  `upscale` role: discovery picks a preference, the endpoint decides.

`roles.json` (shipped, and the single source of truth — JSON, the format `manifest.json` and the
wire already speak, so there is exactly one parser in the tree) holds role ids, predicates, style
class, tier ranks and preference order. **Model ids appear only in that file** — so adding,
retiring or re-ranking a model is a data change, and the future candidate picker is a UI
over rows that already exist.

### Reference data (used to author `roles.json`; nothing in the code reads it)

| Model id | Roles it can fill | Steps | CFG | Native | Size | Notes |
|---|---|---|---|---|---|---|
| `Anima-Turbo` / `-INT8` | `generate.illustration` | 8 | 1.0 | 1024×1024 (recipe pins 768×1024) | ~5.4 GB | 2B, fastest (~5 s @1024×768) |
| `Anima-Aesthetic` / `-Base` (+`-INT8`) | `generate.illustration` (quality tier) | 20 | 4.0 | 1024×1024 | ~5.4 GB | prettier, slower |
| `Krea-2-Turbo-INT8` / `Krea-2-better-VAE` | `generate.photo` | 8 | 1.0 | 1024×1024 | ~21–35 GB | quality anchor (~19–26 s) |
| `Flux-2-Klein-4B-TheNoise` / `-INT8` | `edit` (speed tier), `generate.*` | 4 | 1.0 | 1024×1024 | ~12–15 GB | ~15 s edit |
| `Flux-2-Klein-9B-TheNoise` / `-INT8` / `-KV-INT8` | `edit` (quality tier), `generate.*` | 4 | 1.0 | 1024×1024 | ~25 GB | `-KV` enables the reference KV cache (19.2 s → **13.4 s**) |
| `Qwen-Image-2.1-Turbo` / `-INT8` | `edit`, `generate.*` — one checkpoint, both jobs | fewer than the 28 the predecessor needed | 1.0 | 1024×1024 | ~17–30 GB | replaces `Qwen-Image-2.1`, which is leaving the box; the RGBA-VAE/KV-cache notes and the ~12.3 s edit below were measured on the predecessor and need re-measuring on Turbo |
| `Qwen-Image-Edit-2511` (+`-4steps`, `-INT8`) | `edit` | 4–28 | 1.0 | 1024×1024 | — | edit-specialised, a candidate once measured |
| `RealESRGAN-{2x,4x}plus`, `LSDIR-4x`, `Remacri-4x`, `NMKD-Siax-CX-200k` (all `-TheNoise`) | `upscale` | — | — | — | ~70 MB | work with no DiT loaded |

`sd-cpp` models (**SD-Turbo, SD-1.5, SDXL, Z-Image-Turbo, the GGUF builds**) are *not*
candidates for anything, even though the server lists them.

## Golden rule

> **The backend is a dumb `image + words → image` function. All spatial precision is ours.**

Non-negotiable, and this is now a hard engine fact rather than a portability preference:

* TheNoise's `/edit` accepts `prompt`, `negative_prompt`, `width`, `height`, `steps`,
  `sigmas`, `guidance_scale`, `seed`, `sampler`, `image`, `kv_cache`, `ref_method`, and the
  post-processing knobs. **There is no `mask` field in the engine at all.** (Lemonade's
  public OpenAI table lists `mask` for generic recipes; on `recipe: thenoise` it can only
  be ignored or forwarded into a field the engine does not define.)
* Instruction-based editors drift the whole frame anyway. With local compositing we can
  guarantee *nothing outside the selection changed*, which is the property users actually
  want.

So every AI command compiles to the same pipeline:

```
bbox(mask) → grow for context → snap to size policy → crop from a flattened base
  → PREP (destroy target identity, preserve scene statistics)
  → prompt template → POST /v1/images/edits → resample back to the crop rect
  → out = orig*(1-m) + result*m   with m = feathered mask   [+ colour match, + lock band]
```

Cropping is the spatial instruction. `PREP` is the hint. The mask is **ours**, and never
leaves the client.

## Field policy

Tiers exist so nothing accidental creeps in. The builder is the only place that can add a
field, and it refuses tier-2 unless the model profile opts in.

**The builder also validates its own values, because nobody downstream does.** `seed:
"not-a-number"` is answered with `200` and a default (measured). A misspelt field name is
therefore silent rather than loud, and the one place that can catch it is here.

**Tier 1 — OpenAI-level, used in MVP**

| Endpoint | Content type | Fields |
|---|---|---|
| `POST /v1/images/generations` | JSON | `model`, `prompt`, `size`, `steps`, `cfg_scale`, `seed`, `response_format="b64_json"` |
| `POST /v1/images/edits` | `multipart/form-data` | `model`, `image` (PNG part), `prompt`, `size`, `steps`, `cfg_scale`, `seed`, `response_format="b64_json"` |
| `POST /v1/images/upscale` | JSON | `image` (b64 PNG), `model` |
| `GET /v1/models`, `GET /v1/models/{id}` | — | — |
| `POST /v1/chat/completions` | JSON | `model`, `messages`, `stream=false` — reserved for the `enhance` role, unused in MVP |

**Never sent:** `mask`, `user`, `background`, `quality`, `input_fidelity`,
`output_compression`, `partial_images`, `style`, `response_format="url"`.

**Tier 2 — TheNoise-native, forwarded verbatim by the Lemonade adapter, NOT used in MVP**
(the adapter copies every unmapped request key straight to the engine, so these *work*;
they are simply out of scope until we need them). Kept here because several are exactly the
knobs we want later:

| Field | Effect | Why we care later |
|---|---|---|
| `negative_prompt` | engine-level negatives | cleaner than inlining "no text, no watermark" |
| `sigmas` | custom sigma grid replacing `steps` | the community turbo schedules (Viggle Turbo 6 steps, Pruna 5/8) make 28-step Qwen-2.1 draftable |
| `sampler` | `euler` \| `er_sde` | determinism experiments |
| `kv_cache`, `ref_method` | reference-latent KV cache / `index_timestep_zero` | 30 %+ faster editing; needs a `-KV` Klein checkpoint |
| `upscale`, `upscale_factor`, `upscale_type`, `pixel_upscaler` | in-generate 2× latent refine, or pixel upscaler | "generate big" without a second round trip |
| `film_grain` (0–10), `sharpening` (RCAS 0–1), `qwen_vae_enhance` | post-processing | **`film_grain` is a seam-hider**: grain on the generated patch masks the composite boundary; `qwen_vae_enhance` kills 2 px grid artifacts from Qwen VAEs |
| `lora_specs` | `["style:0.8"]` | style packs — server-side files, so a settings feature, not a per-request one |

Tier 2 is **not implemented**, and there is deliberately no opt-in path for it: `FieldSink` is the only
code that can put a field on a request, it checks every name against the endpoint's tier-1 row, and
`ai.run` refuses a field the table does not list. Adding tier 2 therefore means adding a row to that
table and a way for a recipe to ask for it — which is the point: a knob that works cannot be reached
by accident. When it is added, the gate is intended to be per-server rather than per-model, and a
rejected field dropped with the deduction recorded rather than the job failed.

### `n`: we do not use it

The adapter implements `n` as a **sequential loop inside one request**, for generations too
(the public doc only promises it for edits). One response body containing N base64 PNGs
means waiting for the *last* image. Our queue issues N separate jobs instead, so variants
appear in the tray as they land and one failure does not take the batch with it.

### Variations endpoint: not available

`/v1/images/variations` returns `UnsupportedOperationException` on the `thenoise` backend.
Removed from the contract, from the recipes, and from the UI. "Give me variations" = N jobs
with `seed = base + i` against the normal endpoint.

## Server session & model switching (the constraint that shapes the UX)

TheNoise **loads one model at a time** — `/health` says so in the data:
`max_models.image == 1`, and `model_loaded` is `None` until something is asked for. The
image checkpoints a box of this class holds are 17–35 GB, the same size class as its memory budget, so a
swap is a real event: the first request that followed an unloaded model on the dev box cost
32.0 s for a 512² image. Tens of seconds, sometimes minutes, on a LAN box. Therefore:

* The app has a **session model** concept, shown persistently in the status bar:
  `server · model · loaded/pinned · ETA seed`.
* Recipes declare a preferred *role* (`generate` / `edit`), not a model. The picker resolves
  role → user's chosen model for that role.
* **Swap is explicit and priced.** When a command's role resolves to a model that isn't
  loaded, the confirm dialog says "this switches the server model (~Xs)" and offers to pin.
* We call `POST /v1/load {model_name, pinned:true}` when the user pins, and
  `GET /v1/health` (`model_loaded`, `all_models_loaded[] {model_name, recipe, device,
  pinned, last_use}`) to keep the status line honest. Never auto-swap silently.
* **Warm-up** (a 256×256/1-step request right after a swap, so the first real generation is not the
  one that pays model init): designed, not built. What is built is `server.load {model, pin?}`, so the
  swap is at least something the user chooses a moment for.
* **Queue concurrency 1 per server**, always.
* `GET /v1/stats` is token-oriented, not image-oriented ⇒ we measure latency **client-side**
  per `(model, w×h, steps)` and keep an EMA for ETA display. **A request the server has already
  answered is served from its own cache in ~0.1 s**, so a replay must not feed the EMA — an ETA
  drifting toward the cache's latency would make every job look instant.

## Transport, auth, multi-server

Transport is **libcurl** (`pkg_check_modules(LIBCURL REQUIRED ...)`, like libpng/libjpeg/libwebp):
connect timeout, abort of an in-flight handle for cancel, chunked bodies and TLS verification are the
things the failure table below branches on, and they are not a slice to reimplement.

It sits behind one header, `ai/transport.h`: a request (method, URL, bearer, body, timeouts, a cancel
flag) and a response. `ai/http_curl.cpp` is the easy-interface implementation; `ai/recorded.cpp` answers
the same interface from `tests/fixtures/http` and is what the default `ctest` run talks to. Nothing above
`ai/transport.h` knows which of the two it is on, which is why the field policy, the error classes and the
queue are all testable without a socket — and why `NOISE_AI_FIXTURES` pointing at a recording directory
makes even the running app replay instead of reaching the network.

Lemonade is usually remote (LAN), usually with `LEMONADE_API_KEY` set.

```
ServerProfile (N of them, one active):
  name              "Studio"
  base_url          https://<server>:13305/v1         (http:// allowed on a trusted LAN)
  api_key           from $THENOISE_API_KEY today (the keyring and the 0600 config file are designed
                    and not built) — never in a .noise file, never in the repo, never in a log line
  timeout_connect   5 s
  timeout_request   adaptive: max(60 s, predicted ETA × 4)   ← local diffusion is slow by design
  verify_tls        on by default; per-profile opt-out for self-signed LAN certs
  prefixes          try /v1, fall back to /api/v1 (both documented)
```

Auth: `Authorization: Bearer <api_key>` (the mechanism Lemonade's own tests use);
`?api_key=` as a legacy fallback for servers that only support the query form.
`LEMONADE_ADMIN_API_KEY` is never used (config/admin endpoints are out of scope).

Connection handshake (`server.connect`; the status line's `Connect` button and the host's one automatic ask
both call it): `GET /v1/health` (alive? version? what's loaded?) →
`GET /v1/models` (filter to `recipe == "thenoise"`) → resolve roles → enable commands.

The automatic ask belongs to **the host, not the widget**, and runs once after a frame has been presented:
which dock tab happens to be front is not an answer to "is there a server", and a docked window that is
not the visible tab does not draw at all, so a panel-driven handshake only fires by luck of the layout.
`server.connect` records `connect_attempted` before anything can refuse it, which is what makes the host's
"once" mean once even when the transport is missing entirely.

One attempt is the whole cost when the machine is switched off - the connect timeout is paid on that one
frame, not per frame - and `Connect` is the retry. The flag is session state (`AiSession`), not widget
state, so a test can see the attempt and the UI holds no socket. With no URL configured the
command refuses and names `$THENOISE_URL` — libcurl's "unsupported protocol" for an empty URL describes
the string and hides the cause. A replay session needs no URL either: the transport answers locally and
the profile carries `http://recordings.invalid/v1`, because `.invalid` can never resolve and no real
host may enter a log, a preview or a fixture.
A server with **no `edit`-capable thenoise model** still supports: all local editing,
`new-image`, `upscale`, local Telea `heal` — the edit commands render disabled with the
reason, plus the `lemonade import` that would fix it whenever `ai.roles` can name a model this server
actually offers (and no command at all when it cannot — see the resolution order above). No privacy/consent
badge: the server is user-controlled by definition, wherever it lives.

## The Region Compiler

Input: `(document composite, selection mask, recipe)` → one compiled request.

```
1. BBOX      tight bbox of mask (canvas px)
2. GROW      bbox += max(context_ratio × bbox_size, min_context)      // default 0.35 / 64 px
3. CLAMP     clamp to canvas
4. SNAP      W,H up to a multiple of size_policy.multiple, which is **16**, measured — not 64.
             The engine snaps a request to a multiple of 16 and never refuses an awkward one:
             asking 500x300 answers 512x304. Send multiples of 16 so the crop you planned is the
             crop you get, and expect returned dims != requested dims regardless (the resample
             below already handles it). The number lives in roles.json / the recipe policy,
             nowhere else
5. FIT       if area > region_budget (default 1024×1024 equivalent) → uniform scale down;
             record `scale` *and* `requested` = that scale rounded **up** to the multiple, because
             the two are not proportional and the inverse map needs the size, not the scale
6. CROP      flattened base = visible layers below ∪ active layer (`Document::flatten_over`),
             alpha flattened over a colour sampled from the mask ring (never over plain
             white/black) — `flatten_over_ring(crop, coverage_in_crop)`
7. PREP      a *hole*, not a treatment of the region. Only pixels the coverage reaches are
             altered; every other byte of the crop is uploaded exactly as the document holds it,
             because the surroundings are the instruction. Modes: none | average | blur(gauss,
             r = max(kMinBlurRadiusPx, crop short side / 12)) | pixelate (block = that radius) |
             noise (seeded, so the bytes are cacheable) | white. Default blur: it destroys *what*
             the object is while leaving colour, luminance gradient and light direction readable.
             `average` and `noise` fill from the mean of the **context** — the hole's own mean is
             the colour of the thing being got rid of. Pure white is the worst option and is
             offered only for experiments.
             Only `operation: reconstruct` may ask for a prep; the parser refuses the pairing the
             other way round, which is what stopped `Replace the eyes` from uploading a blurred
             face (see "Two kinds of operation" below).
8. SIZE      always send explicit size = prepped region size, measured twice now: an edit with
             `size` returns it exactly, an edit without it returns the **model default** (1024²)
             rather than the reference image's size, at 30.3 s against 7.4 s for the same 512²
             edit. Omitting it is neither correct nor cheap.
9. WRITE     PNG encode (these exact bytes are the cache key)
```

## The Result Compositor

Always runs; this is what makes instruction editing usable at all. Built as `ai/compose` +
`ai/colour_match`, over `core/resample` and `core/blur`; asserted by `test_compose`.

**An option nobody passes is not an option that was declined.** `feather`, `colour_match` and `lock_band`
reach the compositor through `CompositePolicy`, whose values come from the recipe, and the wire
argument helpers leave the caller's variable alone when the key is absent. That sounds like a detail and
was a live defect: the boolean helper used to write `false` for an absent key, so `job.apply` and
`ai.compose` ran with the colour correction switched off while the recipe and the report both said it
was on. The unit tests had been green throughout, because their fixtures are flat fields — see the lesson
in `PLAN.md` about a flat fixture being a degenerate input for a colour operation.

### The colour match is a drift measurement, not a palette

`colour_match_drift` answers one question: *by how much did the model change the pixels it was not asked
to change?* The region compiler grows the box by a band of context, the answer contains that band, and the
document holds the same pixels, so the band is paired pixel against pixel and the mean Lab difference over
it is the drift. That difference is subtracted from every pixel of the answer — including inside the
replacement area, which has no reference of its own and inherits the correction the band paid for. Spread
is matched from the same paired band, clamped to `kMaxDeviationRatio`.

It used to be defined the other way round (`colour_match_ring`): the patch was pulled onto the Lab mean and
σ of the ring outside the mask. For a hole that is arguable; for anything else it is a defect with a name —
a change that turns a face's eyes green also returns the face, and mapping that face onto the mean of the
hair and background around it produced **a visibly darker patch**, which is what a person reported the
moment Replace stopped blurring. `test_an_answer_that_changed_only_the_subject_is_left_exactly_alone` is
the regression: an answer that changed the subject and nothing else must be left byte for byte.

`probes/2026-10-11-ai-job-live.md` measures the previous definition (9.0 L of lightness drift against the
ring when the correction was accidentally off, 0.08 L when it ran). Those numbers describe that estimator;
the paired one needs its own live measurement before the probe says anything about it.

```
1. RESAMPLE     returned image (its own dims) → the whole crop rect, Lanczos3 over premultiplied
                alpha, support widened when shrinking so no source pixel is dropped (core/resample).
                Premultiplied because the coverage edge is the feather, and a straight-alpha average
                drags a transparent edge's colour into a visible pixel.
2. COLOURMATCH  measure the drift the model put on the context band and take it off the whole answer
                (on for remove/heal, off for reimagine). The band is paired, not two populations: a
                pixel of the **reference** where the effective coverage does not reach, with alpha there,
                is compared with the *same pixel* of the returned image. Drift = mean(returned) − mean(reference)
                in Lab, subtracted from every pixel of the answer; the spread ratio comes from the same
                paired band, floored (kMinLabDeviation) so a flat band does not flatten the answer and capped
                (kMaxDeviationRatio) so a palette change — the edit that was asked for — survives. An answer
                that left the band alone is therefore corrected by nothing, which is the property the old
                two-population estimator lacked and what made a bright subject land dark against dark hair.
3. BLEND        out = dest*(1−m) + result*m,  m = mask ⊛ gauss(feather 2–8 px), through
                Mask::restrict_to — the same premultiplied write-door mix a stroke uses, exact at
                m = 0 and m = 255, which is what makes step 4 provable instead of approximate.
                auto feather = mean mask thickness / 8, clamped: thickness is 2·area/perimeter, the
                band width the ants' boundary runs already give. Region size is *not* the input — a
                crop is grown and snapped, so a small mask in a big crop would be given a feather far
                wider than the feature it is meant to hide.
4. LOCK         force outer N px (policy: 6) to 100 % original → kills seam drift — but only on the
                crop edges that have canvas outside them. At a crop edge that *is* the canvas edge
                there is nothing to protect, and a band there eats into the region the mask covers.
```

The **destination and the reference are different images**, and the compositor says so in its signature:
`compose_result(target, reference, result, plan, coverage, policy)`. `target` is what the answer is
blended onto — a fresh empty layer, since rule A of `docs/TOOLS.md` — and `reference` is the document the
request was compiled from, flattened over the crop. Step 2 measures drift against `reference` because
that is the colour the answer has to agree with; steps 3–4 blend against `target` because that is what
the answer covers. Passing the destination as its own reference is not a no-op any more: an empty layer
has no colour to disagree with, so the correction finds no drift and a dark patch lands. `test_compose`
asserts the correction runs, and runs toward the document, over an empty destination.

Nothing else limits how far a write may exist. An earlier build multiplied in a second mask called the
`extent` — the active layer's alpha for an edit, the canvas for a generation — and that door is gone: a
destination that is empty by construction cannot veto a write, and the coverage the model was shown is
the single restrictor. Which is what lets an answer fill pixels the document never had.

Invariant, asserted: **every pixel of the destination the *effective* coverage does not reach is
byte-identical to what the destination held**, and everything outside the crop is untouched — whole-image
diff, not a sampled point. Effective coverage means feathered and lock-banded: the mask that ran, which
`ComposeReport::coverage` returns, so the assertion is against what happened and not against what was
asked. Every M4 acceptance run asserts it with `pixels.compare`.

`ai.compose {image, apply?, feather?, colour_match?, lock_band?}` is that path on the wire, with the
document's selection as the mask and no selection meaning whole-document coverage. `apply` false (the
default) touches nothing and writes a preview PNG of what the new layer would hold; `apply` true adds a
layer above the active one and goes through `Document::add_layer_written`, so **the layer and its pixels
are one undo step** and one `Ctrl+Z` returns every byte — the write inside it is fenced by
`PaintFence::own_coverage`, **not** by the selection, because the effective coverage is the limit and a
fence at the selection would cut the feather off and leave the report describing an edit the document did
not perform (`test_ai_compose_probe` checks the report against the document's own pixels, and that the
feather reaches past the selection). The report carries the resolved policy, the effective coverage,
`pixels_changed`, the `layer` the answer landed on and its `layer_name`, and `answered_as_requested` —
false on that last one is not a failure (the engine answers a snapped size and the resample copes) but it
is where a drift investigation starts.

**Why coverage is the only restrictor.** The compositor's rule is one: blend by coverage. What an
operation is allowed to touch was already expressible as coverage, and the second mask added nothing the
first could not say once the destination stopped being the source — it only created a state where a
correct answer was silently discarded (`0 changed`, the old probe reporting `8 992` pixels its own
argument had vetoed). `pixels_changed` now counts what was written, and the flattened export can show
fewer pixels different than that: a pixel the model reproduced exactly is written without being a visible
change. Both numbers are reported and the probes compare them as a subset, not an equality.

## Draft → Refine

Real numbers on a Strix Halo, from the engine's own benchmarks — the workflow is sized
around them:

| Job | Model | Time |
|---|---|---|
| generate 1024×768, 8 steps | Anima-Turbo | ~5 s |
| generate 1024×1024, 8 steps | Krea-2-Turbo INT8 | ~19 s |
| generate 1536×2048, 8 steps | Krea-2-Turbo INT8 | ~99 s |
| edit 1024×768, 4 steps | FLUX.2 Klein 9B INT8 (KV off / on) | 19.2 s / 13.4 s |
| edit 1024×1024, 4 steps | Qwen-Image-2.1 INT8 (KV on) | ~12.3 s |

⇒ Edit drafts are *seconds*, so the interactive loop is genuinely fast **as long as we do
not thrash models**. A `512×512` region on Klein is a few seconds; a full-frame
`2048×2048` refine is a minute+. Hence:

1. **Draft** — region compiled ≤ 512 px, `steps` = model default (Klein 4, Anima-Turbo 8).
   ~2–6 s. Variants land one by one in a tray.
2. **Iterate** prompt / prep / feather / seed cheaply.
3. **Refine** — same recipe at region budget with default steps, seed carried over.
4. **Commit** — composite under the feathered mask, journal entry, done.

The expensive thing is not inference, it is **model switching**. Design around that.

Supporting machinery:

* **Request cache** keyed on the sha256 of the built request's canonical text — `server`, `endpoint`,
  `model`, `size`, `steps`, `cfg_scale`, `seed`, the blob id of the uploaded region and the prompt, in
  that order, always written out by the same function. Instant replay of anything ever generated,
  surviving restarts, because the key is a property of the request rather than of a session. The server
  keeps a cache of its own, so a fast response proves neither our cache nor a model run; a job record
  says which one answered (`from_cache`), and the latency EMA counts a reply inside one second as the
  server's cache rather than as a sample.
* **Latency EMA** per `(model, w×h, steps)` from client-measured timings → ETA before you
  commit, and it drives `timeout_request`. The engine's published benchmarks are the
  starting point, not the runtime source: every box differs.
* **Durable queue**: one worker per server, and `<runtime>/ai/jobs/{jobs.json,<id>-result.png,
  <id>-request.png,<id>-coverage.png}` — a 20-minute full-frame job survives an app crash, and an answer
  outlives the run that made it: `job.apply` works on any job in the record, including one from before
  the restart. A job found `running` at startup is marked `failed` with the `interrupted` class: the
  request died with the process and pretending otherwise would leave a job that never finishes.
* **Cancel**: the transport polls a flag the job owns (`curl_handle_abort` in flight, an erase from the
  queue while still queued), and cancelling never discards an answer that already arrived — a landed job
  answers `already succeeded; its result stays at <path>` instead of pretending to take it back.

## Prompt Recipes

Shipped as **read-only JSON files in the app resource directory**, one recipe per file (no in-app
authoring in the MVP). A recipe = operation + prompt template + region policy + hole policy +
composite policy.

JSON has no comments, so a recipe file carries no prose — this document is the prose, and a file that
fails to parse names the key and where. Two conventions follow from choosing JSON:

* `prompt` is an **array of strings**, joined with `\n` when the template is built, because a JSON
  string holding five lines of prompt is unreadable in review and unreadable in a diff.
* policy values are named and explicit (`context.ratio`, `context.min_px`, `composite.lock_band_px`),
  never bare numbers in the order a parser happens to read them.

```json
{
  "id": "inpaint-remove",
  "endpoint": "edits",
  "role": "edit",
  "operation": "reconstruct",
  "prep": "blur",
  "context": { "ratio": 0.35, "min_px": 64 },
  "enhance": "never",
  "prompt": [
    "Remove the {{target}} from this image.",
    "Reconstruct the area behind it as a seamless continuation of the surrounding",
    "{{context_desc}}, matching colour, brightness, light direction, perspective, texture,",
    "focus and grain.",
    "Change nothing else in the image."
  ],
  "composite": { "feather": "auto", "colour_match": true, "lock_band_px": 6 }
}
```

`role`, never a model id; the policy values live here, in the recipe, and nowhere else.

### Two kinds of operation, and the axis `role` does not cover

`operation` says what happens to the pixels; `role` says which model answers. They are different axes
and share no value: one role (`edit`) serves both `reconstruct` and `in_place`, which is exactly why
the second axis had to be said out loud.

| operation | what it means | hole (`prep`) | words |
|---|---|---|---|
| `reconstruct` | the area's content is unwanted; fill it from the surroundings | required | none needed |
| `in_place` | change a named thing; the rest of the picture is the reference | must be `none` | the whole operation |
| `transform` | change the picture, not its content (upscale) | must be `none` | none |
| `from_words` | no source image at all | must be `none` | the whole operation |

The parser refuses the contradictions — `in_place` with a prep, `reconstruct` without one,
`reconstruct` spanning the whole document, `from_words` on the `edits` endpoint, and a recipe that
states no operation at all — each naming the file and the reason. This is not tidiness: `inpaint-replace`
declared `prep: blur`, so "replace the eyes with green eyes" uploaded a smeared face, and the model
returned an unrelated face because the only thing it was shown was a smudge. `tools/prep_proof.py`
(`xvfb-run -a python3 tools/prep_proof.py`) renders the upload for every recipe the verbs can drive and asserts,
whole-image, that an `in_place` upload is byte-identical to the document (0 px changed) while a
`reconstruct` upload changes 0 context pixels and destroys the hole (16 384 of 16 384 for `heal`,
16 204 for the blur, where the rest is area a blur genuinely cannot alter).

Two more keys belong to the recipe because the interface is a consumer of the table, not of a
hard-coded palette:

* **`label`** is what the control says (`"Remove"`, `"Replace"`). The `id` is a key - greppable,
  quoted in errors, never a word for a human; a button wearing an id is a control nobody wrote.
* **`ask`** names the variables a person is asked to fill, in order (`["target"]`,
  `["target", "replacement"]`). Everything not listed runs on its `variables` default, which is what
  makes a headless `ai.run` and a click compile the *same* prompt - and what keeps `context_desc`
  unwritten until the `enhance` role is switched on to fill it. An `ask` name with no declared default
  is refused at parse: it would be collected from the user and then rejected by the prompt builder,
  which is the worst place in the pipeline for that to surface.

The palette of selection operations is therefore `recipes/*.json` filtered to `region: selection` with
an `edits` or `upscale` endpoint (`RecipeSet::selection_intents`): adding an operation means adding a
file, and a recipe that stops being a selection operation leaves the palette by itself.

Two more keys belong to the recipe because the interface is a consumer of the table, not of a
hard-coded palette:

* **`label`** is what the control says (`"Remove"`, `"Replace"`). The `id` is a key, greppable and
  quoted in errors; a button wearing an id is a control nobody wrote.
* **`ask`** names the variables a person is asked to fill, in order (`["target"]`,
  `["target", "replacement"]`). Everything not listed runs on its `variables` default, which is what
  makes a headless `ai.run` and a click compile the *same* prompt - and what keeps `context_desc`
  unwritten until the `enhance` role is switched on to fill it. An `ask` name with no declared default
  is refused at parse: it would be collected from the user and then rejected by the prompt builder,
  which is the worst place in the pipeline for that to surface.

So the palette of selection operations is `recipes/*.json` filtered to `region: selection` with an
`edits` or `upscale` endpoint: adding an operation means adding a file, and a recipe that stopped being
a selection operation leaves the palette on its own.

Variables come from the user's instruction (whatever the recipe's `ask` lists), the layer name and the
document aspect. **The seed is the session's**: `ai.preview` and `ai.run` take `seed` optionally and use
`AiSession::seed` when the call names none, and the status line shows that number. Nothing
draws a random seed inside a request path - the number on the screen is the number the request carried,
which is what makes "run that again" and "why does this one look different" answerable. Variants are the
explicit case: `seed + i`, asked for by name. The
`enhance` **role** (one `chat/completions` call on a downscaled crop — *"describe the
background surface, lighting and palette in two lines"*) is designed in but switched off in
every MVP recipe: it should buy better lighting/perspective matching for ~1 s of an LLM call
instead of minutes of re-generation, and it is a later tuning step, not a dependency. A
recipe that declares `enhance` must produce a valid request when the role is unresolved.

### MVP recipes (pre-shipped)

Every row obeys rule A: the answer arrives on its own layer above the one the region was flattened from,
which is where a strength control lives when one is wanted — the layer's opacity is the blend slider, and
fidelity is not a server knob.

| id | endpoint | role | operation | hole | region | notes |
|---|---|---|---|---|---|---|
| `new-image` | generations | generate | from_words | none | full doc | presets from doc aspect (1:1, 3:4, 4:3, 16:9 at the model's native scale) |
| `new-image-anime` | generations | generate | from_words | none | full doc | Anima-oriented phrasing hints, Anima preferred |
| `new-image-photo` | generations | generate | from_words | none | full doc | Krea-2-oriented phrasing hints |
| `inpaint-remove` | edits | edit | reconstruct | blur | mask bbox | the flagship: the only operation whose blur is a feature |
| `inpaint-replace` | edits | edit | in_place | none | mask bbox | one field of words from the person (`instruction`): "Change this picture: …" |
| `region-fill` | edits | edit | reconstruct | average | mask bbox | generate into the region from one field of words (`prompt`); the fill takes the surrounding colour, never the hole's own |
| `heal` | edits | edit | reconstruct | average | selection | the in-process Telea/Chan path for a box with no edit model is designed and **not built**, so `heal` refuses when the edit role is unresolved |
| `flatten-colorize` | edits | edit | in_place | none | full doc | e.g. "colorize this line art" — the highest-value raster trick for illustration workflows |
| `reimagine` | edits | edit | in_place | none | full doc | full-frame edit. The whole frame is the region, so there is no hole and nothing to blend at the edge; its answer lands on a layer like any other, and 50 % opacity is a number the user owns |
| `upscale` | upscale | — | transform | none | full doc or mask bbox | ESRGAN model choice + composite back into the layer |
| `free-command` | user's choice | user's choice | user's choice | user's choice | user's choice | escape hatch below |

(`variations` deleted — unsupported endpoint.)

**`ai.preview` is the only thing that preps, resamples and encodes an upload besides a run**, and both are
on being asked. Nothing on the frame path does: the window's price line is a `RequestQuote` (endpoint, the
size the region compiler settled on, the model, its default steps, the box's measured time), which is
geometry and table lookups. `AppState::upload_encodes` counts the encodes and `ai.intent` reports it, so a
regression that puts an encode back on a read is a red test rather than a slow window. The reason to keep
`ai.preview` at all is that the *upload* is a claim about the document - `tools/prep_proof.py` renders it
for every recipe, and that is how a dark patch and a blurred subject were caught - not that anyone can
preview an answer before the model makes one.

### What a recipe may ask for

`ask` names the fields **the person** supplies, and the parser enforces its shape, because the verb rows,
the panel's words box and the refusal messages are all built from it:

- an asked name may not also appear in `variables` — an asked field has no prefilled default, and a
  prefilled one is a guess shipped as the user's own words;
- an asked name must appear in at least one prompt line, and may be asked once;
- `reconstruct` and `in_place` ask at most one field (one box, beside the tool rows); `from_words` asks
  exactly one; `transform` asks none.

`build_prompt` refuses a run where an asked field is blank — `recipe 'region-fill' asks for 'prompt' and
was given nothing; pass vars {"prompt": "..."} - an operation with no words is a coin toss` — whitespace
included. `target` is gone from the masked recipes: the masked pixels are what the prompt already
describes, and prefilling it with "the marked area" produced answers that commented on the selection
instead of editing it.

**Built today.** `assets/roles.json` + `assets/recipes/*.json` (10 recipes; `free-command` is the one
row of the table not shipped), loaded by `ai/registry` and `ai/recipes` from `$NOISE_EDIT_ASSETS`, `assets`
beside the app, or the runtime directory — in that order, one lookup, reported by `server.state` as
`tables`. `ai.recipes` lists each recipe with the policy it states and the model its role resolved to, so
"a recipe that behaves oddly" is answerable from the wire. A recipe file whose name and `id` disagree is a
refusal, not two ids for one policy.

### `free-command`

Raw prompt + model + params, plus **"show exact request"**: the multipart field list, the
resolved `size`, and the actual prepped PNG that will be uploaded. Trust, debuggability, and
our own recipe-tuning workflow all depend on this panel existing early.

## Failure handling

Every class is `FailureClass` in `ai/http`, is carried on the job record (`failure`), and comes with a
`hint` — a class without one is a bug, and a test asserts none is empty. Detection is as built:

| Class | Detection | What the app does |
|---|---|---|
| `server_down` | connect refused / DNS / transport error | "is the server reachable at `<base_url>`?", Retry, and the server picker |
| `auth` | 401/403 | says which form the key was sent as (`Authorization: Bearer` or `?api_key=`) and to try the other; no retry loop |
| `no_model_loaded` | 503 | offers `server.load` and says the swap costs tens of seconds |
| `model_not_found` | 404 | the model is not on this server: re-resolve the role or import it. Discovery greys it in the picker and gives the `lemonade import` when the box offers it |
| `not_edit_capable` | 400 whose **body** names editing (`edit`, `instruct`, `image-to-image`) | resolve the edit role elsewhere. Endpoint is not evidence of cause: a 400 from `/images/edits` because a field was missing is a `refusal`, and classifying it by path once told the user to mark a working editor as incapable |
| `size_rejected` | 400 whose body names size/width/height/resolution | clamp to the size policy and retry — a non-multiple is *snapped*, not refused (measured), so this row means out of range |
| `refusal` | any other non-2xx, or a 200 whose body is not JSON from a non-image path | show the field list from the preview; the server's body is kept verbatim in the job record |
| `timeout` | transfer timeout | Retry at a smaller `size`/`steps`; the timeout was the ETA×4 the latency EMA predicted, so this is a real signal about the box |
| `bad_image` | 2xx with no `data[0].b64_json`, undecodable base64, or a PNG that does not decode | the answer is not in the reply: nothing was composited, the job is `failed`, and the raw body is on the record |
| `cancelled` | the job's cancel flag seen by the transport, or a queue entry dropped before it started | the job is `cancelled` and its request is retryable; a cancelled queue entry never reached the server at all |
| `interrupted` | a job found `running` when the queue was reopened | the request died with the app: `failed`, with the answer's path if one had already arrived |

Not built, from the earlier design: stripping a rejected field and retrying with the deduction
persisted; rephrasing a prompt from a "refusal-safe" template variant; marking a blob bad and
auto-retrying it once with a new seed. Each is a policy on top of a class that exists, not a missing
class.

Every request/response (headers redacted, image bytes replaced by blob hash, plus timings
and HTTP status) goes to a rolling `ai.log`. That log is how we tune recipes.

The two refusal bodies that exist today, verbatim from the probe, because the client branches on
their shape and not on their prose:

```
404 {"error":{"code":"model_not_found","message":…, "param":"model","requested_model":…}}
400 {"error":{"message":"Missing 'prompt' field in request","type":"invalid_request_error"}}
```

Neither names a server address; the base URL the profile supplied is what the UI shows back.

## Probe harness

`tools/tn_probe.py` exercises a real server and its output becomes `docs/probes/*.md`. It resolves
the `edit` role the way the registry will (capability predicate over what the box has downloaded,
smallest first; `--model` overrides), then asserts only what the client branches on — discovery
fields, that an explicit `size` comes back exact, that a snap lands on a multiple of 16, the shape
of a 404 and a 400 — and prints timings instead of asserting them, because the box is shared and
a resident model changes every number. Requests vary the seed, since an identical request is a
server-cache hit rather than a run, and `--max-images` caps the spend (a full pass is 9).

```sh
cmake -B build -DNOISE_LIVE_TESTS=ON && ctest --test-dir build -L live   # or run the tool directly
```

`tools/ai_job_probe.py` is the second live tool, and the app-level one: it drives a running
`noise-edit` over the wire through `server.connect` → `ai.preview` → `ai.run` → `job.wait` →
`job.apply`, checks that the request obeyed the size policy and sent no `mask`, that the pixels which
changed are exactly the ones the report counted, that none of them lie outside the coverage the job
claimed, and that undo returns the document byte for byte. Without `--live` it points the app at
`tests/fixtures/http` and runs in the default suite (`test_ai_job_probe`), which is why the whole path is
checked hermetically and then again cheaply against the box; `--live --max-images 2` is its budget.

Both live tools are registered under label `live` and **only registered when
configured on**, so a plain `ctest` never touches the network even in a shell that exports the
credentials; the tool still skips with a `SKIP:` line (and `SKIP:` means skip, not pass) when
`THENOISE_URL` / `THENOISE_API_KEY` are absent. A budget that runs out mid-probe reports a skip,
never a pass over checks that were never made.

The live tier asserts **contract and invariants, never model output**. What has been measured so
far is in `probes/2026-10-09-live-server.md`; the latencies quoted elsewhere in this document are
the engine's published numbers — a sound baseline, not per-box truth, which is why the runtime
uses a measured EMA and an adaptive `timeout_request`. Not yet probed: the prep ablation
(`none|average|blur|pixelate|white`), edit drift over a large region, cancel mid-request, and
`n`/multi-image behaviour. See `AGENTS.md` §2 and `ARCHITECTURE.md` §Testing.
