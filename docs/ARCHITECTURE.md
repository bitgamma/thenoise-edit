# Architecture

## Stack

| Concern | Choice | Why |
|---|---|---|
| Language | **C++20** (no exceptions in hot paths, no RTTI) | ImGui is C++; doc model needs variants/RAII; sokol headers bind from C++ trivially |
| GPU | **sokol_gfx**, backend `SG_BACKEND_GL3` first | Metal/D3D11/WGPU one flag away; GL3.3 is the boring, works-everywhere choice |
| Platform | **sokol_app** (decided: no pen/tablet in MVP) | zero extra deps, same API shape; if tablet pressure ever becomes a requirement, SDL3 goes behind the same shim |
| UI toolkit | **Dear ImGui** + docking branch, via `sokol_glue.h` + `sokol_imgui.h` | fastest path to a full editor chrome |
| Image codecs | **libpng** (lossless, alpha), **libwebp** (opaque payloads), **turbojpeg** (export) | all present on this machine |
| HTTP | **libcurl** (easy interface, worker threads) | ubiquitous, no TLS surprises |
| JSON | **nlohmann/json** | config, model profiles, manifests |
| Compression | **libzstd** for blob store; deflate for zip entries | |
| Container | **miniz** (single-file, MIT) for the project zip | |
| Vector overlay | **sokol_gl** (immediate) or a tiny instanced-quad path | marching ants, brush cursor, ghost regions, selection handles |
| Inference backend | **Lemonade Server**, `thenoise` recipe only | we are a client; the model box is AMD/ROCm, usually on the LAN, bearer-keyed |

Build: CMake + `FetchContent` (sokol, imgui, miniz, nlohmann). System libs via `pkg-config`
where available. Everything statically linkable into one binary. No OpenCV (heavy, wrong
licence gravity for what we need; the ~400 lines of healing/filtering we want are ours).

**Cross-platform by design, Linux-first in practice.** No Windows/macOS build scripts yet,
but nothing may foreclose them:

* one `CMakeLists.txt`, no hand-written makefiles, no shell-only build steps;
* platform-specific code quarantined in `src/platform/` behind one header with three
  implementations (`_WIN32` / `__APPLE__` / else); everything above it is portable C++;
* UTF-8 internally, `std::filesystem::path` (from/to UTF-8) at the boundary;
* no POSIX-only primitives outside the shim: std threads/mutexes/atomics, no `fork`, no
  `poll`, no signal-based shutdown, no `dlopen`;
* case-insensitive-path-safe (never two assets differing only in case);
* sokol / ImGui / CMake cover Win32, Cocoa, X11+Wayland already — the port is a build task,
  not a refactor;
* CI is Linux now; Win/mac stay a documented `docs/porting.md` checklist rather than a
  rotting disabled workflow.

## Module layout

```
src/
  core/        # no deps, no GPU, no IO — pure data + maths, unit-testable
    image.{h,cpp}      ImageBuffer (RGBA8 straight alpha, stride-aware, tiles later)
    document.{h,cpp}   Document, Layer, Mask
    blend.{h,cpp}      blend mode maths (CPU reference impl, matched by GPU shader)
    maskops.{h,cpp}    rasterize shapes, flood fill, feather, morphology, bbox, colour-match
    heal.{h,cpp}       Telea / Chan fusion inpainting (local, offline)
    command.{h,cpp}    undoable command interface + pixel-delta commands
    blob.{h,cpp}       content-addressed store API (impl in io/)
  gfx/
    renderer.{h,cpp}   sg init, passes, resource lifetime
    compositor.{h,cpp} layer stack → composite texture (ping-pong, per-layer blend)
    texturecache.{h,cpp} blobId → sg_image, eviction, full-texture upload policy
    strokes.{h,cpp}    GPU live-stroke preview (instanced stamps)
    overlay.{h,cpp}    ants, guides, ghost regions, wipe slider
  app/
    main.cpp           sokol entry, frame loop, mailbox drain
    tools/             one file per tool; Tool = press/drag/release/overlay/commit state machine
    ui/                panels: layers, generate, journal, jobs, status, prefs
    selection.{h,cpp}  document-level floating selection + editing modes
  ai/
    server.{h,cpp}     ServerProfile: base URL, bearer key, TLS opts, adaptive timeouts;
                       /v1/health handshake; session-model state; load/pin; latency EMA
    registry.{h,cpp}   role resolution: `roles.toml` ∩ GET /v1/models → one model per role
                       per profile; nothing else in the codebase names a model
    provider.{h,cpp}   request builder; the only place a field can be added (tier-1 always,
                       tier-2 only if the profile opts in)
    profile.{h,cpp}    per-model policy (size multiple/budget, steps, roles, quirks),
                       merged over discovery
    compiler.{h,cpp}   THE region compiler (see AI-BACKEND.md)
    recipes.{h,cpp}    pre-shipped TOML loading + variable substitution
    composite.{h,cpp}  result resample / colour-match / feather blend back
    queue.{h,cpp}      jobs, cancellation, ETA, durable queue
    cache.{h,cpp}      request → blob cache
    http.{h,cpp}       curl wrappers: generations / edits / upscale / health / load / chat
                       (no variations — unsupported on the thenoise backend)
  dbg/
    wire.{h,cpp}       debug wire: JSON-lines server, request dispatch, event broadcast
    capture.{h,cpp}    viewport/window/composite → PNG on disk + sha256
    headless.{h,cpp}   run the same command surface without a window (SG_BACKEND_NULL)
  io/
    png.{h,cpp} webp.{h,cpp} jpeg.{h,cpp}
    project.{h,cpp}    .noise read/write
    ora.{h,cpp}        OpenRaster import/export
    psd_export.{h,cpp} (M5, optional)
    config.{h,cpp}     settings, keyring access
tests/         core + ai/compiler unit tests (no GPU), golden-image tests
shaders/       composite.glsl, blend modes, ants, resample
```

Dependency direction: `app → {core, gfx, ai, io}`; `gfx → core`; `ai → core, io`; `io → core`.
Nothing imports ImGui except `app/ui`. That keeps the door open for a different UI later and
makes `core`+`ai` testable headless.

## Threading model

One render thread (sokol is not thread-safe), everything slow off it.

```
RENDER THREAD (60 fps)              WORKER POOL (N=4, I/O-bound mostly 1)
  input → Tool state machine          png/webp/jpeg encode+decode
  ui tick (ImGui)                     sha256, zstd
  drain Mailbox (≤2 msgs/frame)       curl requests (minutes-long!)
  upload new textures                 blur/feather/floodfill/colour-match/heal
  compositor + overlay draw           (never touches sg_*)
        ▲
        └── Mailbox: mpsc of JobResult { kind, blobId, bytes, progress, err }
```

Rules: workers produce **`BlobRef` + raw bytes**, never GPU handles. The render thread
creates `sg_image`s and uploads. Draining is budgeted per frame so a burst of completed
generations cannot stutter the canvas. UI reads jobs through an immutable snapshot pointer
swapped atomically, so panels never lock.

Workers never touch `sg_*`. The AI queue is **concurrency 1 per server** (the engine holds
one model at a time, so parallel jobs only thrash), and a model swap is a user-visible,
priced event. Because inference can take seconds-to-minutes over the LAN, the document must
stay fully editable while jobs run, autosave must be independent of them, and the queue must
reload after a crash. Hard requirement, not a nicety.

## Compositing

* Each layer is an RGBA8 texture (straight alpha in storage, premultiplied in shaders).
* Composite = ping-pong pass sequence: `dst = blend(dst, layer_i)` with `opacity_i`,
  `mask_i`, per-layer blend-mode branch. Cheap, correct, and handles arbitrary stacks.
  Sub-32 layers: full re-composite of a dirty rect only; the union of dirty rects from
  `sg_update` requests, clamped to the visible viewport ∪ margin.
* Composite result → `comp_tex`; viewport pass draws it with zoom/pan, checkerboard
  underneath, then overlay, then ImGui.
* Layer visibility/opacity/blend changes are uniform-only ⇒ free. Pixel edits are the only
  dirty source.

## Painting path (real sokol constraint)

`sg_update_texture` replaces a **whole** texture; there is no partial update on the GL
backend, so a naive "upload after every brush dab" on a 4096² layer = 64 MB/frame. Design:

1. CPU backing store per layer (source of truth; also what save/export/AI read).
2. During a stroke: stamp **GPU** preview quads over the canvas; no upload at all.
3. On stroke end: commit stamping to the CPU store, single full `sg_update_texture`
   (~6 ms at 4096² — one-time per stroke, invisible).
4. `history` records the touched tiles' old pixels (see below).

Upgrade path when we need it (not MVP): split each layer into 256×256 tiles, one
`sg_image` per tile → cheap partial updates, bounded VRAM, and free large-canvas support.
Keep `ImageBuffer` behind an accessor API so the tile migration is local to `core/gfx`.

## Undo, journal, and the blob store

Content-addressed storage makes destructive editing *and* cheap undo coexist:

* `blobs/<sha256>` — immutable, refcounted, zstd-compressed PNG or raw.
* `Layer.source` is just a `BlobRef`. **An AI op = write a new blob + swap the pointer**,
  so undo/redo of a 3-minute generation is a pointer swap, and no generation is ever lost.
* Pixel edits push tile deltas (old pixels) into the history stack; budget by tile count
  (e.g. 128 MB) with disk spill. Structural commands (add/reorder/opacity/mask) store the
  inverse command.
* `history.jsonl` — append-only *journal*: every op with timestamp, params, prompt, model,
  seed, blob hashes, timings. Two jobs: (a) Provenance panel ("which prompt made this
  layer?" → one-click **Regenerate with same params**), (b) recovery/replay after a crash.

This is the strongest argument for our own container: provenance is not an afterthought,
it's a first-class file-format feature.

## File format

**Decision: our own container, laid out as a superset of OpenRaster.**
`.noise` = ZIP, atomic save (`tmp` + rename), unknown entries preserved on load.
MIME `application/x-noise+zip`, binary `noise-edit`.

```
mimetype                       "application/x-noise+zip"  (stored, first entry, no extra fields)
manifest.json                  app doc: format version, doc size, dpi, layer tree (our richer model),
                               selection state, viewport, prefs snapshot
stack.xml                      ORA-compatible layer stack (so Krita/MyPaint can open us *today*)
mergedimage.png                flattened composite, regenerated on save (for other apps / thumbnails)
layers/<n>/layer.png           per-layer payload (or .webp when fully opaque and large)
layers/<n>/mask.png            optional per-layer mask
provenance/<op>.json           AI request/response metadata per generated layer (prompt, model, seed,
                               timings, provider, prepped-input hash)
blobs/<hh>/<sha256>            immutable payload store (undo history, variants, drafts)
history.jsonl                  append-only journal
variants/<op>/<i>.png          kept AI variants not committed to a layer
Thumbnails/thumbnail.png
```

### Why not adopt an existing format wholesale

| Format | Verdict | Reason |
|---|---|---|
| **OpenRaster (.ora)** | adopt layout, write on save, read on import | The only real *open* layered interchange (Krita, MyPaint, GIMP plugin, KAlcura…). ZIP + `stack.xml` + per-layer PNG + `mask.png` + offsets + `mode="multiply:0.5"` + groups via nested `<stack>`. But: XML, no adjustment/text/vector layers, **nowhere to put generation provenance**, and it forces lossless-relevant assumptions. Perfect as the *compatibility projection* of our model, not as the model. |
| **Photoshop PSD/PSB** | export-only, M5 | Adobe published the official spec in 2024, so writing it is legitimate work rather than folklore — but blend-mode legacy pairs, channel packing and mask encoding are a week of pain. Interop value is real, fidelity of *our* features is not. |
| **KRA / XCF** | reject | app-private, no stable spec for third parties, reference libs are GPL (licence gravity onto our binary) |
| **Multi-page TIFF** | reject | pages ≠ layers: no blend modes, no masks, no stack semantics |
| **Layered EXR** | maybe, later | excellent for 16/32-bit float AOVs, useless for a blend-mode stack; keep as an export for a future linear-light pipeline |
| **PNG / WebP / AVIF / JPEG-XL** | flat export only | single frame; no stack |
| **SVG** | reject | not a raster layer model |

Round-trip guarantee we *can* honestly make: `.noise → .ora → Krita` (layers, masks,
opacity, blend, visibility) and `.ora → .noise` (same subset, provenance absent), and
`.noise → flat PNG/WebP/JPEG` always exact.

## Debug wire

The agent-maintained nature of this codebase makes observability a first-class component,
not a debugging afterthought: **if the wire cannot show you a state, you cannot verify it**.

* Transport: localhost TCP, JSON-lines, one object per request and per response, plus async
  event frames. `--debug-wire=<port>` (default on in debug builds, `--no-debug-wire` to
  disable). The bound port is written to `<runtime dir>/wire-port` so a driver can find an
  ephemeral one. Never bound to a non-loopback interface. `WireConfig::token` gates the first
  frame (`{"auth": …}`); the app binds loopback with an empty token — the 0600 token file is
  not generated yet.
* **It dispatches through the same string-keyed command table the UI and the keyboard use**
  (`command.list`, `command.invoke {name, args}`), so exercising the wire exercises the real
  path. The wire reads state and invokes commands; it never reimplements behaviour.
* Frame loop services the wire between input and draw, so requests are handled at frame
  boundaries and see consistent state; long commands return a job id rather than blocking.
* Baseline command set, growing with the app:

| Group | Commands | Why |
|---|---|---|
| state | `app.state`, `command.list`, `wire.info`, `doc.get`, `layer.list`, `selection.get`, `server.state`, `job.list`, `log.tail` | read what the app believes, cheaply |
| drive | `command.invoke`, `app.quit`, `doc.new/open/save`, `log.level`, `view.fit/set/pan/zoom`, `layer.add/set/delete`, `selection.set`, `tool.stroke {points}` | `tool.stroke` drives the *real* stroke path, so painting is testable without a mouse |
| pixels | `image.get {region, base64}`, `screenshot {region, name, base64}`, `pixels.compare {a, b}` | screenshots exist so a vision model can look; `pixels.compare` is the pixel-lock invariant made checkable |
| ai | `ai.preview {recipe, args}` (compiled request + prepped PNG, **no send**), `ai.run`, `job.wait {id}`, `cache.stats` | verify what we *would* send before spending minutes of someone's GPU |

Live now: the state/drive/pixels rows minus `layer.*`, `selection.*`, `server.state`,
`job.list`, `tool.stroke` and the whole ai row, which arrive with their milestones.
`app.state` reports `frame` (frames presented), `frame_ms` (last presented frame), `canvas`
(origin + size of the canvas content area in framebuffer px) and the view transform — enough
for a driver to place an image rect on screen and wait for the frame that drew it.

* `screenshot`/`image.get` write PNGs to a capture dir and answer with path + sha256, so a
  test can compare bytes and an agent can open the file.
* `--headless` boots `SG_BACKEND_NULL` with a CPU composite path: same commands, no window,
  for CI. Not implemented — CI renders under Xvfb + llvmpipe instead (`tools/wire_drive.py`
  skips with a loud reason when it can get neither). Anything that needs real GPU shading
  stays a golden-image test of the shader pair, not a headless assertion.
* **Definition of done includes the wire command that makes a new feature observable.**

## Testing

| Tier | Runs | Needs network | Content |
|---|---|---|---|
| **unit / property** | always, `ctest` | no | region compiler round-trips, mask ops, blend reference, blob store, format round-trips |
| **golden image** | always, `ctest` | no | named fixtures → exact bytes: codecs, mask ops, composite, heal |
| **recorded backend** | always, `ctest` | no | `ai/` against committed HTTP fixtures: request field policy, response parsing, error mapping, cache keys |
| **live backend** | on demand, tagged `live` | **yes** | the real contract against the dev server |

Live tests are expected, not exotic. The dev Lemonade server is available when the human
exports `THENOISE_URL` and `THENOISE_API_KEY`; ask for them when a change needs live
verification, and say which tests and roughly how many images it will cost. Behaviour:

* absent env ⇒ `skip` with a loud reason, never `fail`, never silently green;
* present env ⇒ they run and their failures are as real as any other failure;
* they assert **contract and invariants**, never model output: roles resolve, output dims
  satisfy `size_policy`, **pixel-lock outside the mask holds** (that part is ours, so it is
  exact regardless of what the model produced), error shapes and status codes, cancel works;
* they are budgeted — small sizes, model default steps, a per-run image cap, and they restore
  any model they swapped (a swap costs minutes on a shared box);
* timings are recorded to `docs/probes/` and never asserted exactly.

The bar for every test in every tier is in `AGENTS.md` §2: a plausible bug must turn it red.

## Decisions (locked)

| # | Question | Status |
|---|---|---|
| **D1** | Platform layer | **sokol_app** — no pen/tablet in the MVP. SDL3 behind the same shim if that ever changes. |
| **D2** | Colour/bit depth | RGBA8 straight alpha, sRGB-ish, no colour management in MVP. `PixelFormat` stays in `ImageBuffer` so 16-bit is a migration, not a rewrite. |
| **D3** | Canvas ceiling | 8192×8192, warn >4096², tile only when a real document needs it. |
| **D4** | Alpha of the flattened base sent to AI | flatten over a colour sampled from the mask ring, never over white/black. |
| **D5** | One image per document (no artboards) | yes |
| **D6** | Undo across AI ops | always, free via blob indirection |
| **D7** | Single-file vs directory project | single `.noise` zip; `--unpack/--repack` dev flags for git-friendliness |
| **D8** | Licence | **Apache-2.0**. One SPDX line per source file. GPL codec libs stay out. |
| **D9** | Content scope | raster layers only — no text/vector/adjustment layers in the MVP. |
| **D10** | Recipes | pre-shipped, read-only TOML in the app resource dir. No in-app authoring. |
| **D11** | Backend scope | Lemonade only, `thenoise` recipe only; N server profiles (LAN/localhost), always keyed. |
| **D12** | Naming | extension `.noise`, binary `noise-edit`, config dir `~/.config/noise-edit`, MIME `application/x-noise+zip`. |
| **D13** | Model references | roles only (`generate.photo`, `generate.illustration`, `edit`, `upscale`, `enhance`); one model per role per profile; candidates ranked by quality/speed in `roles.toml`. |
| **D14** | Observability | debug wire from M0, dispatched through the UI command table; every feature ships its wire command. |
| **D15** | Ground rules | `AGENTS.md` is normative and overrides prose elsewhere in this repo. |
