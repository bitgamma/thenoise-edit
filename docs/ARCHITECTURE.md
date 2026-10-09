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
    document.{h,cpp}   Document: pixels, path, revision, and the history under it
    history.{h,cpp}    undo/redo as stacked rectangle deltas, budgeted by stored bytes
    brush.{h,cpp}      dabs, stamp coverage, stroke compositing (the CPU paint path)
    blend.{h,cpp}      blend mode maths (CPU reference impl, matched by GPU shader)
    blend.{h,cpp}      blend mode maths (CPU reference impl, matched by GPU shader)
    maskops.{h,cpp}    rasterize shapes, flood fill, feather, morphology, bbox, colour-match
    heal.{h,cpp}       Telea / Chan fusion inpainting (local, offline)
    command.{h,cpp}    invokable command table: commands, titles and key bindings
    shortcut.{h,cpp}   canonical shortcut text: grammar, vocabulary, normalization
    blob.{h,cpp}       content-addressed store API (impl in io/)
  gfx/
    renderer.{h,cpp}   sg init, passes, resource lifetime
    compositor.{h,cpp} layer stack → composite texture (ping-pong, per-layer blend)
    texturecache.{h,cpp} blobId → sg_image, eviction, full-texture upload policy
    strokes.{h,cpp}    GPU live-stroke preview (instanced stamps)
    overlay.{h,cpp}    ants, guides, ghost regions, wipe slider
  app/
    main.cpp           sokol entry, frame loop, mailbox drain
    commands.cpp       assembles the whole command table from the groups below
    command_args.{h,cpp} shared argument readers and the JSON each command answers with
    document_commands.cpp doc.* image.get
    paint_commands.cpp tool.stroke/pointer/set, tool.<name>, brush.larger/smaller, edit.undo/redo
    view_commands.cpp  view.*, and the fit that waits for a real canvas area
    tools/             one file per tool; Tool = press/drag/release/overlay/commit state machine
    shortcuts.{h,cpp}  platform key event -> canonical shortcut text (the only sokol-aware
                       part of the app layer)
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
  underneath, then overlay, then ImGui. The transparency checker is one `2x2` texel tile
  drawn repeat-sampled over the canvas rect (`gfx/checker.h`), so it costs four vertices at
  any zoom. Drawing it as one overlay element per screen square instead costs
  `canvas_area / square²` and is unbounded in zoom, because the canvas rect grows with the
  scale: that overflowed the UI vertex buffer and blanked the canvas at fit zoom on a
  2400×1600 document.
* Layer visibility/opacity/blend changes are uniform-only ⇒ free. Pixel edits are the only
  dirty source.

## Painting path (real sokol constraint)

`sg_update_texture` replaces a **whole** texture; there is no partial update on the GL
backend, so a naive "upload after every brush dab" on a 4096² layer = 64 MB/frame. What runs:

1. CPU backing store per layer (source of truth; also what save/export/AI read).
2. A stroke accumulates coverage in one `StrokeScratch` - a rectangle plus one `double` per
   pixel - instead of painting into the document dab by dab. Dab spacing lives in a `DabWalk`,
   which is carried between pointer events, so a polyline fed in event-by-event emits exactly
   the dabs the same polyline handed over at once would.
3. A live gesture holds its pixels aside: `Document::begin_provisional` snapshots the
   rectangle the stroke has reached, `repaint_provisional` restores that snapshot and
   composites the scratch once, and `commit_provisional` records the snapshot as one undo step.
   That is what keeps opacity per stroke rather than per frame, and one gesture one step
   rather than one step per frame. The snapshot follows the stroke's own bounds, not the
   canvas: a 4 px dot must not cost a canvas-sized copy.
4. On stroke end the pixels are already in place; the only upload is the one the revision bump
   causes anyway. Cost per pointer event is snapshot-restore over the stroke bounds + one
   whole-texture upload, which is why the measured cost grows with document area (2.0 ms median
   at 640², 9.2 ms at 2048² - `probes/2026-10-09-paint-core.md`).
5. `history` records the old pixels of the rectangle the stroke touched (see below), which is
   why `core::Document::paint` is *given* the rectangle instead of diffing afterwards: diffing
   the whole image per stroke would cost what the stroke was saved.
6. `tool.stroke` (whole polyline) and `tool.pointer` (press/drag/release) share `walk_dabs`,
   `scratch_stamp` and `scratch_composite`, so a wire client and the mouse land on identical
   bytes - asserted, not assumed, in `test_app`.

Two ways out if a document gets big enough for the whole-texture upload to dominate, cheapest
first: a dirty-rect upload (needs a backend that can do partial updates, or the tile cache
below), or the GPU stamp preview that was the original plan - stamp quads during the stroke,
no upload until release. Neither is implemented; the measurements above decide.

Upgrade path when we need it (not MVP): split each layer into 256×256 tiles, one
`sg_image` per tile → cheap partial updates, bounded VRAM, and free large-canvas support.
Keep `ImageBuffer` behind an accessor API so the tile migration is local to `core/gfx`.

## Undo, journal, and the blob store

Content-addressed storage makes destructive editing *and* cheap undo coexist:

* `blobs/<sha256>` — immutable, refcounted, zstd-compressed PNG or raw.
* `Layer.source` is just a `BlobRef`. **An AI op = write a new blob + swap the pointer**,
  so undo/redo of a 3-minute generation is a pointer swap, and no generation is ever lost.
* Pixel edits push rectangle deltas into the undo stack: one entry is the rectangle an
  operation declared it would touch plus the bytes it held, and an undo hands the pixels it
  displaces to the redo stack instead of keeping a second copy. Budget by stored bytes
  (`kHistoryBudgetBytes`, 128 MB) dropping **whole steps oldest-first**: losing an old step is
  a shorter history, losing part of a step would be a half-restored edit. Tiles become the
  storage granularity when partial texture uploads need them. Structural commands
  (add/reorder/opacity/mask) store the inverse command.
* Undo and redo are commands like any other (`edit.undo`, `edit.redo`, bound to `Ctrl+Z` and
  `Ctrl+Y`), so a driver can put the pixels back and compare checksums without a keyboard.
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

### What is written today: `format` 1

Measured in `docs/probes/2026-10-09-container-v0.md`.

`io/project.{h,cpp}` reads and writes the subset the app can represent, and refuses what it would
have to guess about. The manifest already describes a layer array, so adding layers needs no
version bump; `format` moves when a field becomes required.

| member | written | read |
|---|---|---|
| `mimetype` | stored, first member, no extra fields | must match exactly or the file is not ours |
| `manifest.json` | `format`, `size: [w,h]`, `layers: [{name, src}]` | `format` must be 1; `size` must agree with the PNG, and both numbers are named if they do not |
| `layers/<n>/layer.png` | `layers/0/layer.png` for the one layer | exactly one layer accepted: a file declaring more is refused by count, because opening it as one throws a layer away |
| `mergedimage.png` | the composite, which with one layer is the layer | optional |
| anything else | — | carried through and rewritten byte-identical, so opening a file written by a newer build cannot delete what this one cannot show |

Save is `<path>.noise-tmp` + rename: a write that fails leaves the previous file intact and
readable. ZIP members are stored (not deflated) with fixed DOS timestamps, so equal content gives
equal bytes and a container can be diffed; the reader accepts stored *and* deflated members, and
validates bounds, CRC, declared sizes, compression method, encryption, zip64 and multi-disk limits
against a file it treats as hostile. `doc.save` writes only `.noise`; a flat image is
`doc.export`, which does not take the document's path nor clear its unsaved mark.

Two checks carry this section, because our own reader agreeing with our own writer proves nothing:
`tests/test_zip.cpp` parses the end-of-directory record by hand (a central-directory size that does
not reach the end record is refused - `unzip` tolerates that error, Python's `zipfile` does not),
and `tools/container_probe.py` has `zipfile` read a project the app saved and decode its layer PNG.

`stack.xml`, `provenance/`, `blobs/`, `history.jsonl`, `variants/` and `Thumbnails/` are not
written yet; nothing about them is invented ahead of the milestone that needs them.

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
* **Key bindings are part of a command's registration**, so `command.list` reports the whole
  keymap and nothing can be bound in one place and documented in another. Shortcut text is
  canonical (`core/shortcut.h`: modifiers Ctrl, Alt, Shift in that order, then a key - `F`,
  `Ctrl+S`, `Space`, `PageUp`), the table refuses a malformed one and refuses to bind a key
  twice, and the host translates a platform key event into the same text (`app/shortcuts.h`).
  `tools/wire_input.py` presses keys and moves the pointer for real, because a binding that
  no event can produce would otherwise look fine on the wire.
* Frame loop services the wire between input and draw, so requests are handled at frame
  boundaries and see consistent state; long commands return a job id rather than blocking.
* Baseline command set, growing with the app:

| Group | Commands | Why |
|---|---|---|
| state | `app.state`, `command.list`, `wire.info`, `doc.get`, `layer.list`, `selection.get`, `server.state`, `job.list`, `log.tail` | read what the app believes, cheaply |
| drive | `command.invoke`, `app.quit`, `doc.new/open/save`, `log.level`, `view.fit/set/pan/zoom`, `edit.undo/redo`, `tool.stroke {points, tool, size, hardness, opacity, flow, spacing, color}`, `tool.pointer {phase, at}`, `tool.set {tool?, size?, hardness?, opacity?, flow?, spacing?, color?}`, `tool.<name>`, `brush.larger/smaller`, `layer.add/set/delete`, `selection.set` | `tool.stroke` drives the *real* stroke path, so painting is testable without a mouse; its brush fields are all required, so a recorded stroke replays without knowing what the UI had selected. `tool.pointer` is the same path one event at a time - the UI drives the pointer through this command, so a gesture can be recorded and replayed. `tool.<name>` is generated per row of `core::kTools`, so a tool exists in exactly one table |
| pixels | `image.get {region, base64}`, `screenshot {region, name, base64}`, `pixels.compare {a, b}` | screenshots exist so a vision model can look; `pixels.compare` is the pixel-lock invariant made checkable |
| ai | `ai.preview {recipe, args}` (compiled request + prepped PNG, **no send**), `ai.run`, `job.wait {id}`, `cache.stats` | verify what we *would* send before spending minutes of someone's GPU |

Live now: the state/drive/pixels rows minus `layer.*`, `selection.*`, `server.state`,
`job.list` and the whole ai row, which arrive with their milestones.
`app.state` reports `frame` (frames presented), `frame_ms` (last presented frame), `canvas`
(origin + size of the canvas content area in framebuffer px) and the view transform
(`scale`, `origin`, `image`, `fit_pending`), the brush and tool the last stroke ran with, and
the document's `history` depths — enough for a driver to place an image rect on screen, wait
for the frame that drew it, and tell a stroke from an undo. `doc.get` is the call that hashes
the pixels and answers with a checksum; `app.state` deliberately does not, because a driver
polls it every frame. `fit_pending` is true while a fit waits for a
frame to publish a canvas area wide enough to hold the document: the framebuffer is not the
canvas, and fitting against it latches a scale that spills past the panel.
`ui_vertices` / `ui_vertex_limit` report the geometry the last frame demanded against the
buffer simgui stages it into — past the limit simgui drops the frame's draw commands, so any
feature generating one element per canvas square or per log line shows up here first.

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
