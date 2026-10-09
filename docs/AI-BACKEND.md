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
| `edit` | `recipe==thenoise` + `image` + `edit` | instruction-based editing | FLUX.2 Klein (4 steps, fast) ↔ Qwen-Image-2.1 (28 steps, higher fidelity) | ✓ |
| `upscale` | `recipe==thenoise` + `upscaling` | pixel-space upscaler | factor + fidelity | ✓ |
| `enhance` | `chat` + `vision` | prompt rewriting / scene description | cheap-and-fast ↔ capable | declared, **unused in MVP** |

### Resolution order (per profile, recomputed on connect)

1. **candidates** = `GET /v1/models` filtered to `recipe == "thenoise"` (image roles) or by
   labels (`enhance`), restricted to `downloaded: true`;
2. narrowed by the role's capability predicate;
3. the **user's assignment** for that role, if it is still present and downloaded;
4. else the **app preference order** from `roles.toml` — INT8-ConvRot before bf16, a
   KV-cache checkpoint preferred for `edit`, ties broken by smaller download;
5. else the role is **unresolved**: dependent commands render disabled with the reason and
   the exact `lemonade import` / `lemonade pull` that would fix it. Never fail at click time.

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

`roles.toml` (shipped, and the single source of truth) holds role ids, predicates, style
class, tier ranks and preference order. **Model ids appear only in that file** — so adding,
retiring or re-ranking a model is a data change, and the future candidate picker is a UI
over rows that already exist.

### Reference data (used to author `roles.toml`; nothing in the code reads it)

| Model id | Roles it can fill | Steps | CFG | Native | Size | Notes |
|---|---|---|---|---|---|---|
| `Anima-Turbo` / `-INT8` | `generate.illustration` | 8 | 1.0 | 1024×1024 (recipe pins 768×1024) | ~5.4 GB | 2B, fastest (~5 s @1024×768) |
| `Anima-Aesthetic` / `-Base` (+`-INT8`) | `generate.illustration` (quality tier) | 20 | 4.0 | 1024×1024 | ~5.4 GB | prettier, slower |
| `Krea-2-Turbo-INT8` / `Krea-2-better-VAE` | `generate.photo` | 8 | 1.0 | 1024×1024 | ~21–35 GB | quality anchor (~19–26 s) |
| `Flux-2-Klein-4B-TheNoise` / `-INT8` | `edit` (speed tier), `generate.*` | 4 | 1.0 | 1024×1024 | ~12–15 GB | ~15 s edit |
| `Flux-2-Klein-9B-TheNoise` / `-INT8` / `-KV-INT8` | `edit` (quality tier), `generate.*` | 4 | 1.0 | 1024×1024 | ~25 GB | `-KV` enables the reference KV cache (19.2 s → **13.4 s**) |
| `Qwen-Image-2.1` / `-INT8` | `edit`, `generate.*` — one checkpoint, both jobs | 28 (6–8 with a turbo `sigmas` grid) | 1.0 | 1024×1024 | ~17–30 GB | RGBA VAE; KV cache exact & on by default; ~12.3 s edit |
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

Tier 2 is gated: `profile.noise_native = { <field>: value }` in the model profile, applied
only when `profile.id` is a `thenoise` model. If a future server rejects one, the field is
dropped and the deduction is recorded — same mechanism as tier-1 400-handling.

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
`max_models.image == 1`, and `model_loaded` is `None` until something is asked for. Krea 2
(21–35 GB) and Qwen-Image-2.1 (17–30 GB) are the same size class as the machine's memory budget,
so a swap is a real event: the first request that followed an unloaded model on the dev box cost
32.0 s for a 512² image. Tens of seconds, sometimes minutes, on a LAN box. Therefore:

* The app has a **session model** concept, shown persistently in the status bar:
  `server · model · loaded/pinned · ETA seed`.
* Recipes declare a preferred *role* (`generate` / `edit`), not a model. The picker resolves
  role → user's chosen model for that role.
* **Swap is explicit and priced.** When a command's role resolves to a model that isn't
  loaded, the confirm dialog says "this switches the server model (~Xs)" and offers to pin.
* We call `POST /v1/load {model_name, pinned:true}` when the user pins, and
  `GET /v1/health` (`model_loaded`, `all_models_loaded[] {model_name, recipe, device,
  pinned, last_use}`) to keep the panel honest. Never auto-swap silently.
* **Warm-up**: a 256×256/1-step request right after a swap, so the first real generation
  isn't the one that pays model init.
* **Queue concurrency 1 per server**, always.
* `GET /v1/stats` is token-oriented, not image-oriented ⇒ we measure latency **client-side**
  per `(model, w×h, steps)` and keep an EMA for ETA display. **A request the server has already
  answered is served from its own cache in ~0.1 s**, so a replay must not feed the EMA — an ETA
  drifting toward the cache's latency would make every job look instant.

## Transport, auth, multi-server

Lemonade is usually remote (LAN), usually with `LEMONADE_API_KEY` set.

```
ServerProfile (N of them, one active):
  name              "Studio"
  base_url          https://<server>:13305/v1         (http:// allowed on a trusted LAN)
  api_key           from $NOISE_EDIT_API_KEY, OS keyring, or 0600 config — never in a .noise file
  timeout_connect   5 s
  timeout_request   adaptive: max(60 s, predicted ETA × 4)   ← local diffusion is slow by design
  verify_tls        on by default; per-profile opt-out for self-signed LAN certs
  prefixes          try /v1, fall back to /api/v1 (both documented)
```

Auth: `Authorization: Bearer <api_key>` (the mechanism Lemonade's own tests use);
`?api_key=` as a legacy fallback for servers that only support the query form.
`LEMONADE_ADMIN_API_KEY` is never used (config/admin endpoints are out of scope).

Connection handshake on select/connect: `GET /v1/health` (alive? version? what's loaded?) →
`GET /v1/models` (filter to `recipe == "thenoise"`) → resolve roles → enable commands.
A server with **no `edit`-capable thenoise model** still supports: all local editing,
`new-image`, `upscale`, local Telea `heal` — the edit commands render disabled with the
reason and the `lemonade` command needed to pull Klein or Qwen-2.1. No privacy/consent
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
             below already handles it). The number lives in roles.toml / the recipe policy,
             nowhere else
5. FIT       if area > region_budget (default 1024×1024 equivalent) → uniform scale down;
             record scale. Never upscale a region beyond the source resolution.
6. CROP      flattened base = visible layers below ∪ active layer; alpha flattened over a
             colour sampled from the mask ring (never over plain white/black)
7. PREP      none | average | blur(gauss, r ≈ bbox/12) | pixelate | noise | white
             default blur: it destroys *what* the object is while preserving the colour,
             luminance gradient and light direction of what is behind it. Pure white is the
             worst option and is offered only for experiments.
8. SIZE      always send explicit size = prepped region size, measured twice now: an edit with
             `size` returns it exactly, an edit without it returns the **model default** (1024²)
             rather than the reference image's size, at 30.3 s against 7.4 s for the same 512²
             edit. Omitting it is neither correct nor cheap.
9. WRITE     PNG encode (these exact bytes are the cache key)
```

## The Result Compositor

Always runs; this is what makes instruction editing usable at all.

```
1. RESAMPLE     returned image (its own dims) → crop rect, Lanczos3
2. COLOURMATCH  match result Lab mean/σ to the original ring *outside* the mask
                (on for remove/heal, off for reimagine)
3. BLEND        out = orig*(1−m) + result*m,  m = mask ⊛ gauss(feather 2–8 px, auto from
                mask perimeter length and region size)
4. LOCK         force outer N px of the crop rect to 100 % original → kills seam drift
5. CLIP         apply active-layer alpha: never paint where the layer is transparent
```

Invariant we will test: **pixels outside the feathered mask are byte-identical to the
original.** Every M4 acceptance run asserts it with a pixel diff.

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

* **Request cache** keyed `sha256(server|model|prompt|params|prep|inputBlobIds)` — instant
  replay of anything ever generated, survives restarts. The server keeps a cache of its own, so
  a fast response proves neither our cache nor a model run; a job record says which one answered.
* **Latency EMA** per `(model, w×h, steps)` from client-measured timings → ETA before you
  commit, and it drives `timeout_request`. The engine's published benchmarks are the
  starting point, not the runtime source: every box differs.
* **Durable queue**: jobs and results land on disk; a 20-minute full-frame job survives an
  app crash; abandoned results are still stored (never discard a finished generation).
* **Cancel** best effort via curl handle abort.

## Prompt Recipes

Shipped as **read-only TOML files in the app resource directory** (no in-app authoring in
the MVP). A recipe = prompt template + region policy + prep policy + composite policy.

```toml
[inpaint-remove]
endpoint  = "edits"
role      = "edit"          # a role, never a model id
prep      = "blur"
context   = 0.35            # policy values live here, in the recipe
enhance   = "never"         # role-based prompt enhancement: declared, off in every MVP recipe
prompt = """
Remove the {{target}} from this image.
Reconstruct the area behind it as a seamless continuation of the surrounding
{{context_desc}}, matching colour, brightness, light direction, perspective, texture,
focus and grain.
Change nothing else in the image.
"""
composite = { feather = "auto", colour_match = true, lock_band = 6 }
```

Variables come from the user's instruction, the layer name and the document aspect. The
`enhance` **role** (one `chat/completions` call on a downscaled crop — *"describe the
background surface, lighting and palette in two lines"*) is designed in but switched off in
every MVP recipe: it should buy better lighting/perspective matching for ~1 s of an LLM call
instead of minutes of re-generation, and it is a later tuning step, not a dependency. A
recipe that declares `enhance` must produce a valid request when the role is unresolved.

### MVP recipes (pre-shipped)

| id | endpoint | role | region | notes |
|---|---|---|---|---|
| `new-image` | generations | generate | full doc | presets from doc aspect (1:1, 3:4, 4:3, 16:9 at the model's native scale) |
| `new-image-anime` | generations | generate | full doc | Anima-oriented phrasing hints, Anima preferred |
| `new-image-photo` | generations | generate | full doc | Krea-2-oriented phrasing hints |
| `inpaint-remove` | edits | edit | mask bbox | the flagship |
| `inpaint-replace` | edits | edit | mask bbox | "replace {{target}} with {{replacement}}" |
| `region-fill` | edits | edit | mask bbox | generate into the region from prompt only |
| `heal` | local, else edits | edit | small mask | Telea/Chan fusion in-process when the server is absent or lacks an edit model |
| `flatten-colorize` | edits | edit | full doc | e.g. "colorize this line art" — the highest-value raster trick for illustration workflows |
| `reimagine` | edits | edit | full doc | full-frame edit dropped in as a **new layer at 50 % opacity** (strength is controlled locally, since fidelity is not a server knob) |
| `upscale` | upscale | — | full doc or mask bbox | ESRGAN model choice + composite back into the layer |
| `free-command` | user's choice | user's choice | user's choice | escape hatch below |

(`variations` deleted — unsupported endpoint.)

### `free-command`

Raw prompt + model + params, plus **"show exact request"**: the multipart field list, the
resolved `size`, and the actual prepped PNG that will be uploaded. Trust, debuggability, and
our own recipe-tuning workflow all depend on this panel existing early.

## Failure handling

| Class | Detection | Action |
|---|---|---|
| server down | connect refused/timeout | "Is Lemonade running at `<base_url>`?" + Retry + server picker |
| auth | 401/403 | key prompt, echo the header name we used, no retry loop |
| no model loaded | engine `503` | offer `POST /v1/load` (and explain the swap cost) |
| model not loaded/`downloaded:false` | discovery | greyed in picker + the `lemonade` command to pull it |
| not edit-capable | engine `400` on `/edit` | should be impossible (role gating); if it happens, mark the profile `edit=false` and persist |
| unsupported field | `400` | strip least-important field, retry once, persist deduction to profile |
| size rejected | `400` naming size | clamp to profile `size_policy`, retry. Measured: a non-multiple is *not* refused, it is snapped, so this row is for a size out of range rather than an odd one |
| policy refusal | model-specific text | rephrase via the recipe's refusal-safe template variant |
| timeout | adaptive per-profile | Retry at lower `size`/`steps` offered |
| bad image | blank/flat/undecodable PNG | mark blob bad, retry once with a new seed |

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

It is registered as ctest `tn_probe_live` under label `live` and **only registered when
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
