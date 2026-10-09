# MVP Scope & Milestones

## The one-screen idea

A layer editor where **AI operations are ordinary editing commands with a compile step in
the middle**. You paint a selection, you choose an *intent* ("remove", "replace", "fill"),
you get variants back, you accept. Prompt engineering, region cropping, size snapping,
pre-painting and result compositing all happen underneath — and are inspectable on demand.

```
 select region ─▶ pick intent ─▶ [compile: crop+prep+prompt] ─▶ lemonade/thenoise (image+words→image)
                                                                              │
              commit to layer ◀─ accept ◀─ A/B wipe ◀─ [composite under feathered mask]
```

Constraints that shape everything (see `AI-BACKEND.md`):
* **`recipe: thenoise` models only**; the engine has **no `mask`** ⇒ all spatial control is local.
* Only **FLUX.2 Klein** and **Qwen-Image-2.1** (and Qwen-Image-Edit) can *edit*; Anima and
  Krea 2 **generate only** ⇒ commands must be gated on model capability.
* The engine **loads one model at a time** ⇒ model switching is the expensive operation, not
  inference, and must be explicit and priced in the UI.

## MVP feature set (P0)

**Connect / setup**
- server profiles (base URL, bearer key from env/keyring/0600 file, TLS opt-out for
  self-signed LAN certs, connect/request timeouts)
- connect handshake: `/v1/health` → `/v1/models` → filter `recipe == "thenoise"` → resolve
  roles → enable/disable commands accordingly
- **role assignment** UI: generate model, edit model, upscale model, (optional) captioner
- **session model panel**: what is loaded now (`/v1/health` `model_loaded`, `pinned`), swap
  with an explicit cost estimate, pin/unpin (`/v1/load`), warm-up request after a swap
- missing-model guidance: the exact `lemonade import <recipe.json>` / `lemonade pull …` to run

**Canvas / editing**
- pan, zoom-to-cursor, fit/100%, checkerboard transparency
- brush (size/hardness/opacity/flow, no pressure — D1), smooth stroke interpolation,
  eraser, fill (fg/bg), eyedropper (layer | composite)
- import PNG/WebP/JPEG; export PNG/WebP/JPEG + `.noise`; document ceiling 8192²
- `Upscale document` (ESRGAN via `/v1/images/upscale`)

**Layers** (raster only)
- add/delete/duplicate/reorder (drag), visibility, inline rename, opacity, blend modes
  (normal, multiply, screen, overlay, darken, lighten, color-dodge, color-burn for MVP;
  soft/hard-light, difference, exclusion, add in M5)
- merge down, flatten, crop-to-selection, new-layer-via-copy/clone
- layer masks: add from selection / from layer, hide-all/show-all, enable/disable, apply
  (destructive), alt-click to view mask

**Selections**
- rect, ellipse, polygon/lasso (path → scanline fill), magic wand (tolerance,
  contiguous/global, anti-aliased) on composite or active layer
- new/add/subtract/intersect modifiers; invert; feather; expand/shrink; border
- marching ants, `Ctrl+D` clear, save selection ↔ layer mask

**AI**
- `New image from prompt` → new layer, aspect presets at the model's native scale,
  presets steered toward the model's strength (`-anime` on Anima, `-photo` on Krea 2)
- `Remove object` (flagship): paint → blur-prep → remove prompt → local composite
- `Replace`, `Fill region` — same plumbing, different template
- `Heal` — in-process Telea/Chan fusion when there is no server or no edit model; backend
  when available ("no network, no problem")
- `Upscale region / document`
- `Reimagine` — full-frame edit dropped in as a new layer at 50 % opacity
- `Free command` with exact-request preview (fields + the actual prepped PNG)
- draft (≤512 px) → refine → commit flow; ETA from client-measured latency EMA
- variants = N queued jobs with `seed+i` (not the `n` param), landing one-by-one in a tray;
  A/B wipe; Accept/Discard; every result journaled and cached
- request cache keyed on `(server, model, prompt, params, prep, inputBlobIds)`
- journal/provenance panel + "Regenerate with same params"

**Robustness**
- undo/redo including AI ops (free via blob indirection), autosave every 2 min to a recovery
  bundle, crash-recovery prompt at launch, durable job queue
- `ai.log` of every request/response (redacted, image bytes → blob hash)

## Explicitly NOT in MVP

text layers · vector/shape layers · adjustment layers · smart objects · artboards · colour
management/ICC · 16-bit · non-destructive transforms · clone stamp / source-point healing
brush · plugins/scripting · `mask` on any request · the `/images/variations` endpoint
(unsupported by `thenoise`) · `sd-cpp` models (SD-Turbo/SD-1.5/SDXL/Z-Image/GGUF) ·
TheNoise-native tier-2 request fields (`sigmas`, `kv_cache`, `ref_method`, `film_grain`,
`sharpening`, `pixel_upscaler`, `lora_specs`, `upscale*`) · PSD/KRA/XCF import · in-app
recipe authoring · Windows/macOS packaging scripts · timeline/animation · mobile/web ·
multi-user/collaborative · "select subject" segmentation (no contract for it) · batch
multi-image jobs · history slider UI (journal list instead) · telemetry, any kind

## Feature availability matrix

Drives what the UI enables; the app must degrade honestly rather than error at click time.

| Capability | any `thenoise` image model | + `edit` model (Klein / Qwen-2.1) | + upscaler model |
|---|---|---|---|
| all local editing, layers, masks, selections | ✓ | ✓ | ✓ |
| `new-image*` | ✓ | ✓ | ✓ |
| `heal` | ✓ (local Telea) | ✓ (backend, better) | ✓ |
| `inpaint-remove` / `-replace` / `region-fill` / `reimagine` / `flatten-colorize` | disabled + reason | ✓ | ✓ |
| `upscale` | disabled + reason | ✓ | ✓ |
| `free-command` with `/edits` | disabled for generate-only models | ✓ | ✓ |

## Milestones

| M | Deliverable | Acceptance test | Est. |
|---|---|---|---|
| **M0** | Walking skeleton: CMake+FetchContent, sokol_app window, ImGui dockspace, checkerboard quad, PNG in/out, log panel, **debug wire with `screenshot`/`image.get`/`pixels.compare`**, `command.list`/`command.invoke`, Linux CI, `src/platform/` shim with one impl | `cmake -B build && cmake --build build && ./build/noise-edit foo.png` → window at 60 fps; **and**: drive a pan+zoom through the wire, capture the viewport, and have a test diff that capture against a golden | 3–4 d |
| **M0.5** | Probe harness `tools/tn_probe.py` (see AI-BACKEND.md): roles dump, generations/edits timing grid, drift pixel-diff, prep ablation, upscale, `n`/multi-image/timeout probes → `docs/probes/*.md`. **Deferred — needs the dev server; run it the moment the box is free** | committed report with *our* numbers; `size_policy`, role default order, ETA seeds and recipe wording updated from it | 2–3 d |
| **M1** | Paint core: single layer, brush/eraser/undo, zoom/pan, `.noise` v0, status bar | 5000-dab stroke at 2048² without dropped frames, 20× undo | 1–2 wk |
| **M2** | Layers + masks + selections, blend modes, ORA import/export | import a Krita `.ora`, edit a masked layer, re-export, diff-clean in Krita | 1–2 wk |
| **M3** | Server layer: profiles + bearer auth, health handshake, **role registry (`roles.toml`)**, role resolution + session model + swap/pin UI, jobs/cache/queue, exact-request preview, `new-image` | connect, resolve roles, generate into a new layer, restart → cached result instant; a `live`-tier test asserts roles resolve and the field policy holds against the real server | 1 wk |
| **M4** | Mask-less inpainting: region compiler, prep, recipes, local composite, draft/refine, variants tray, A/B wipe, journal | paint over an object → `Remove` → object gone, **everything outside the feather byte-identical** — asserted by `pixels.compare`, hermetically against recorded fixtures and, when `THENOISE_URL` is set, against the real server | 1–2 wk |
| **M5** | Polish: local `heal`, `upscale`, remaining blend modes, keymap file, prefs, Linux packaging (AppImage), docs | a stranger connects to a LAN server and removes an object in <5 min without opening a terminal | 1 wk |

Total ≈ 6–8 weeks solo. **M0 is the only thing to start on**: it is the smallest slice that
produces something we can observe, and the debug wire inside it is what makes every later
milestone verifiable instead of assumed. Editor chrome (M1/M2) is boring-but-known; model
behaviour is the unknown, so M0.5 and the `live` test tier retire that risk as soon as the
dev server is free.

## UI layout

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│ menu bar              [ studio · 192.168.1.20 · Klein-9B-INT8 (edit, pinned) ▸ ]   │
├──┬──────────────────────────────────────────────────────────────┬─────────────────┤
│T │                                                              │ ▸ Generate      │
│o │            canvas (checkerboard, ants, ghost regions)        │ ▸ Layers        │
│o │                                                              │ ▸ Journal       │
│l │                                                              │ ▸ Masks         │
│  │                                                              │ ▸ Jobs   [1/3]  │
├──┴──────────────────────────────────────────────────────────────┴─────────────────┤
│ 128 px · 45% · (1024,768) · Layer 2 · sel 43% · ⏱ ~13 s · autosaved 12:04         │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Shortcuts: `V` move `B` brush `E` eraser `M` rect-sel `L` lasso `W` wand `G` fill `I` pick
`[`/`]` size `,`/`.` hardness/opacity `Space`-drag pan `Alt` pick `Enter` apply sel
`Ctrl+D` deselect `Ctrl+I` invert `Ctrl+J` new-via-copy `Ctrl+E` merge down `Ctrl+Z/Y`
`Ctrl+S` `Tab` panels `F` fit. Commands are string-keyed in one table from day one, so a
command palette and a user keymap file cost nothing later. Live today: `F` fit, `Ctrl+S`
save, `Ctrl+Z`/`Ctrl+Y` undo and redo, `Escape`/`Ctrl+Q` quit, `B`/`E` brush and eraser,
`[`/`]` brush size, wheel zoom-to-cursor, right-drag pan and left-drag paint; every other key
above arrives with the tool or command it belongs to (and
`command.list` is the truth about what is bound). `Ctrl+S` writes the project and only the
project (`.noise`); a flat PNG is `doc.export`, which takes neither the document's path nor its
unsaved mark. The Layers panel is live too - `layer.list` / `add` / `remove` / `select` on the
wire, click-to-select and the two buttons in the UI, and strokes land on the selected layer while
each step stays bound to the layer it was made on. Visibility, opacity and blend modes are the
next layer increment and need a manifest decision first (`docs/PLAN.md`).

Design rules: nothing blocks the canvas, ever · every AI op shows *what it will send* ·
model swaps are explicit and priced · destructive ops are labelled but never nag (undo is
real) · disabled AI commands always explain why and what to pull.

## Risks

1. **Edit-model availability.** The flagship commands need Klein or Qwen-2.1 downloaded on
   the server (12–30 GB). Mitigation: honest capability gating, the exact pull command in
   the UI, and `heal`/`new-image` fully functional without it.
2. **Model swap thrash.** Generation-only models (Krea 2) and edit models are different
   loads of a 30 GB class. Mitigation: role-based recipe resolution, session model pinned,
   explicit swap dialog with ETA, warm-up request, and a "keep one model" default.
3. **Model drift** — instruction edits change the whole frame. *Mitigated by design*
   (local composite + lock band); asserted by a pixel-diff test in M4.
4. **Small-region quality** — a 120 px crop generated at 512² gives a texture mismatch.
   Mitigation: context padding, colour match, `prep=blur`, region sent at native scale when
   it fits `size_policy`.
5. **Seams.** Auto feather from perimeter/region size, colour match, and (M5+) the tier-2
   `film_grain` knob as a deliberate seam-breaker. 20-mask eyeball set.
6. **Output size is dictated by `size`**, not the input ⇒ always send explicit size, always
   resample back; unit tests over odd aspect ratios and off-canvas crops.
7. **ROCm-only server.** TheNoise runs on AMD/ROCm (Strix Halo `gfx1151` and friends). QA
   needs access to that box; the app must not assume localhost (LAN round-trips, TLS, keys).
8. **sokol full-texture updates** — `sg_update_texture` replaces whole textures. Mitigation
   in `ARCHITECTURE.md`; tile migration stays hidden behind `ImageBuffer` accessors.
9. **Scope creep toward "just like Photoshop"** — this list is the contract; a new P0 item
   must displace an existing one.

## Decision log (locked)

| | Decision |
|---|---|
| Platforms | cross-platform **by design**; Linux build/CI now, Win/mac deferred to `docs/porting.md` |
| Input | mouse/keyboard only, **no pen/tablet** in MVP → sokol_app |
| Backend | **Lemonade only**, `recipe: thenoise` only; API keys required; server often remote on LAN; no consent badge (server is user-controlled) |
| Content model | **raster only** |
| Canvas | 8192×8192 ceiling, ~512 MB budget |
| Recipes | pre-shipped, read-only; TOML (JSON also fine) |
| Naming | extension **`.noise`**, binary `noise-edit`, MIME `application/x-noise+zip` |
| Mask | never sent to the server; applied locally, always |
| Variations | endpoint not used (unsupported) |
| Model references | **roles, never model ids** (`generate.photo`, `generate.illustration`, `edit`, `upscale`, `enhance`); one model per role per profile; `roles.toml` ranks candidates by quality/speed |
| Prompt enhancement | the `enhance` role exists but is **off in every MVP recipe**; later tuning step, never a dependency |
| Licence | **Apache-2.0**, one SPDX line per source file |
| Observability | **debug wire from M0**, dispatched through the UI command table; every feature ships its wire command |
| Testing | four tiers (unit · golden image · recorded backend · `live`); the dev server is used for real when `THENOISE_URL`/`THENOISE_API_KEY` are set |
| Ground rules | **`AGENTS.md` is normative** and outranks the prose in these docs |

## Still open

1. **Layer count/variant budget expectations** for a typical session — my numbers assume
   ~50 layers, ~20 kept variants per doc; drives history budget (128 MB) and blob store GC.
2. **AppImage first or a plain tarball?** (packaging is M5, but Flatpak sandboxing affects
   how we reach a LAN box + keyring, so worth a thought now.)
