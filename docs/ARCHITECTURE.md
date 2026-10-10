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
    document.{h,cpp}   Document: a stack of layers, path, revision, the composite over them
                       and the history under it
    layer.{h,cpp}      Layer (id, name, pixels, optional mask, visible, opacity, mode) and generated
                       names; a stack is bottom-first. `LayerPlane` names which of a layer's two
                       planes - `pixels`, `mask` - a write or a step belongs to, and its name table is
                       the vocabulary `mask.target` and `stroke.plane` speak
    history.{h,cpp}    undo/redo as stacked (layer, plane, rectangle) deltas, budgeted by stored bytes
    brush.{h,cpp}      dabs, stamp coverage, stroke compositing (the CPU paint path), and the same
                       coverage handed to the other plane: `scratch_coverage` as bytes for the mask
                       door, `mask_ink` as the coverage statement of `PaintMode`, and
                       `paint_stroke_mask` stamping a whole stroke into a `Mask`
    blend.{h,cpp}      the blend reference: source-over and erase on straight RGBA8, the layer
                       blend (mode + per-layer opacity) and `kBlendModes`, the one table of
                       modes and their manifest names, and the one place a blend result is
                       quantised. Byte tables sit in front of the arithmetic for whole-opaque
                       pixels and for a fixed (source alpha, backdrop alpha) pair; they are built
                       by calling the reference, so they cannot disagree with it, and `test_blend`
                       checks them against it rather than against themselves. A GPU pair has to be
                       checked against the reference when the GPU starts blending; today the
                       renderer uploads the flattened composite, so there is nothing second to
                       match yet.
    mask.{h,cpp}       the coverage plane: one byte per pixel, straight, and the one home of the
                       rules over it - `op(rect, replace|add|subtract)`, `op(rect, patch, op)` for
                       coverage a shape produced, `paint(rect, ink, reveal|hide)` for a stroke laying
                       coverage in coats towards one end of the scale, `invert`, the counts `facts()`
                       reports (pixels touched, the area those bytes claim, the tight bbox) and
                       `boundary()` - the **edges** between covered and uncovered pixels labelled with
                       the side they face: each edge once, consecutive edges of one side as one run.
                       Edges and not pixel groupings, because a boundary is only drawable along its
                       own length and a staircase must stay one line, and because rows grouped one
                       way and columns the other make one corner belong to two lines. The counts come
                       from per-row tallies that a write dirties only for the rows it reached, and the
                       boundary is cached apart from them and walked only over the box the mask covers
                       - an overlay walking edges and a panel reading counts must not pay for each
                       other, and at 2048² either one rescanned per pointer event is what made a mask
                       stroke and a marquee drag feel slow
                       (docs/probes/2026-10-09-mask-and-marquee-cost.md). `covers_all(region)` and
                       `covers_nothing(region)` are answered from the same tallies, which is what lets
                       the composite skip a pass it would only waste. Held by the mask that owns the
                       bytes and dropped by every write to them, per row where the write says which
                       rows. A document's
                       selection is a `Mask`; a layer mask is a `Mask`; converting between them is a
                       copy, which is why there is no `selection.{h,cpp}` and no second coverage
                       type. `restrict_to(mask, …)` is the write-door rule stated below, and
                       `scale_by_coverage(mask, …)` the same mix with nothing underneath, which is
                       how a composite reads a layer through its mask.
    maskops.{h,cpp}    coverage producers: `MaskShape` (`rect`, `ellipse`) applied to a `Rect` with a
                       `MaskOp`, `alpha_coverage(image)` turning an image's alpha into "how much layer is
                       here" so a write can be limited by it with the same multiply everything else uses,
                       and the table `kMaskShapeInfo` naming the shapes (and so naming the commands
                       and the tools that draw them). A shape writes the same coverage plane and is
                       never a mask type of its own, so an added shape costs a row here and nothing in
                       the document, the composite, the format or the overlay. The ellipse is
                       anti-aliased by exact vertical strips, 16 samples for the width a pixel straddles.
    blur.{h,cpp}       the box blur over an RGBA-ish plane, and `feathered_mask`: three passes of odd
                       box sizes, because anything wider is a different kernel. Radius 0 is the
                       identity, so "no feather" is a number rather than a branch, and the plane is
                       clamped at the ends rather than wrapped - a mask feathered at the canvas edge
                       must not pick up the far side of the image. `ai/region` prep and `ai/compose`
                       soften with this and nothing else.
    resample.{h,cpp}   Lanczos-3 over premultiplied alpha, with the support widened when the image
                       shrinks so no source pixel is dropped. Premultiplied because the coverage edge
                       of a returned image is the feather, and averaging straight alpha drags a
                       transparent pixel's colour into a visible one; the weights are normalised among
                       themselves, never by the coverage. At 1:1 it returns the input byte for byte -
                       the kernel is interpolating and the weights are exact, which is what lets the
                       result compositor resample unconditionally without breaking pixel-lock.
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
    brush_steps.h    the rows behind the step keys: which field, which direction, which key —
                     registered as commands by paint_commands and read back by the Brush panel
                     for its hover hint, so a binding exists once
    document_commands.cpp doc.* image.get
    paint_commands.cpp tool.stroke/pointer/set, tool.<name>, the brush step commands, edit.undo/redo
    view_commands.cpp  view.*, and the fit that waits for a real canvas area
    tools/             one file per tool: brush.cpp (press/drag/release over the provisional edit),
                       move.cpp (the same gesture, sliding the layer's pixels), select.cpp (the
                       marquee, which becomes the document's selection live during the drag). Which
                       file the pointer talks to is the `action` column of `core::kTools`
                       (`paint` / `move` / `select`), and which shape a marquee tool drags is its
                       `marquee` column - read when the button goes down, so switching tools
                       mid-gesture cannot change the shape already being drawn
    shortcuts.{h,cpp}  platform key event -> canonical shortcut text (the only sokol-aware
                       part of the app layer)
    ui/                panels: layers, generate, journal, jobs, status, prefs
    select_commands.cpp select.rect/all/clear/invert/get, and the `Ctrl+D`/`Ctrl+I` bindings
    ai_commands.cpp     `ai.compose {image, apply?, feather?, colour_match?, lock_band?, extent?}`:
                       lands a result file on the document through `Document::paint` with
                       `own_coverage` - one undo step, unfenced by the selection - or previews it, and
                       reports what the policy, the extent and the coverage resolved to
    mask_commands.cpp mask.from_selection, mask.add/remove/fill/invert, mask.target and mask.get -
                       the two directions between a selection and the active layer's mask (both copies
                       of one `Mask`), the mask's own life cycle, and where strokes are aimed at
  ai/
    server.{h,cpp}     ServerProfile: base URL, bearer key, TLS opts, adaptive timeouts;
                       /v1/health handshake; session-model state; load/pin; latency EMA
    registry.{h,cpp}   role resolution: `roles.json` ∩ GET /v1/models → one model per role
                       per profile; nothing else in the codebase names a model
    provider.{h,cpp}   request builder; the only place a field can be added (tier-1 always,
                       tier-2 only if the profile opts in)
    profile.{h,cpp}    per-model policy (size multiple/budget, steps, roles, quirks),
                       merged over discovery
    region.{h,cpp}     THE region compiler (see AI-BACKEND.md): `plan_region` for the geometry
                       (bbox → grow → clamp → snap → fit, with the inverse map carried as the two
                       rectangles rather than a scale) and `prep_region` for the bytes that would go
                       on the wire
    recipes.{h,cpp}    pre-shipped JSON loading + variable substitution
    compose.{h,cpp}    the result compositor (see AI-BACKEND.md): resample onto the crop, feather,
                       lock band, clip to layer alpha, blend through `Mask::restrict_to` - and the
                       `ComposeReport` whose effective coverage is what pixel-lock is asserted against
    colour_match.{h,cpp} Lab mean/σ transfer from the ring outside the coverage onto the patch inside
    queue.{h,cpp}      jobs, cancellation, ETA, durable queue
    cache.{h,cpp}      request → blob cache
    http.{h,cpp}       curl wrappers: generations / edits / upscale / health / load / chat
                       (no variations — unsupported on the thenoise backend)
  dbg/
    wire.{h,cpp}       debug wire: JSON-lines server, request dispatch, event broadcast
    capture.{h,cpp}    viewport/window/composite → PNG on disk + sha256
    headless.{h,cpp}   run the same command surface without a window (SG_BACKEND_NULL)
  io/
    png.{h,cpp} jpeg.{h,cpp} webp.{h,cpp}
                       the flat formats: decode to RGBA8, encode from it
    flat.{h,cpp}       those formats as one table - extension, sniffed magic, whether it holds
                       alpha, whether `quality` means anything - plus the default export quality
    project.{h,cpp}    .noise read/write (the only layered format we read or write)
    psd_export.{h,cpp} (M5, optional)
    config.{h,cpp}     settings, keyring access
tests/         core + ai unit tests (no GPU), golden-image tests
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
3. A live gesture holds its pixels aside: `Document::begin_provisional` snapshots a rectangle,
   `repaint_provisional` restores it and composites the scratch, `commit_provisional` records the
   snapshot as one undo step. That is what keeps opacity per stroke rather than per frame, and one
   gesture one step rather than one step per frame. Snapshot and coverage buffer both follow the
   stroke and grow by `kBufferGrowthFactor` rather than to fit: growing to fit copies every
   accumulated pixel on every event, and a 4 px dot must not cost a canvas-sized copy either.
4. **An event repaints the rectangle its own dabs touched, not the area the stroke has covered.**
   A pixel's composited value depends only on its own coverage, and the scratch already holds
   every earlier dab that reached it, so restoring and re-blending just the new rectangle is exact
   rather than approximate - `test_brush` pins piecewise == whole-scratch byte for byte. It is also
   the difference between an event costing what it added and costing the stroke's history: the same
   40-event diagonal drag at brush 24 on a 2048² document went from 2.9 ms at its first event to
   108 ms at its 40th, and is now 0.6-1.6 ms throughout
   (`probes/2026-10-09-paint-and-panel-cost.md`). The floor left under an event is the one
   whole-texture upload sokol allows, which is the dirty-rect upload item - and in Release the
   whole frame during such a gesture measures 2.4 ms, so what is left is the upload, not the blend.
5. `history` records the old pixels of the rectangle the stroke touched (see below), which is
   why `core::Document::paint` is *given* the rectangle instead of diffing afterwards: diffing
   the whole image per stroke would cost what the stroke was saved.
6. `tool.stroke` (whole polyline) and `tool.pointer` (press/drag/release) share `walk_dabs`,
   `scratch_stamp` and `scratch_composite`, so a wire client and the mouse land on identical
   bytes - asserted, not assumed, in `test_app`.

### Composite

`Document::composite()` is the one flattened image: what the renderer uploads, what a flat
export writes, what `mergedimage.png` holds. It is derived, owned by the document, and repaired
only over the rectangles the layers changed since it was last asked for (`blend_layer` takes a
region for exactly this, and applies the layer's own `opacity` there rather than baking it into
the layer's pixels, so a layer shown at 30 % is still painted on at full strength). Two rules
keep it cheap and honest:

* **A stack of one visible layer at full strength is that layer**, returned by reference with no
  blend at all. Blending a single layer has no second operand, and doing it anyway puts a
  canvas-sized pass on every frame - which is what the first version of this did, at 71 ms per
  pointer event on a three-layer 2048² document. The condition is `visible && opacity == 1`, and
  deliberately *not* `&& mode == normal`: with an empty backdrop the composite formula collapses to
  the source whatever the separable arithmetic said, so a lone `multiply` layer is still itself and
  the shortcut stays legal as modes are added. `test_document` pins this per mode - the shortcut is
  compared with the blend path's answer, coverage everywhere and colour wherever there is coverage
  (bytes under zero alpha may differ between the two paths and no renderer can show them, which is
  why that test compares pixels rather than a checksum). One hidden layer, by contrast, is a
  transparent canvas and not that layer, so ignoring `visible` would be a wrong picture rather than
  a slow one - `test_a_lone_layer_is_shown_whole_only_at_full_strength` holds that line.
* **A pixel with no coverage is absent, not black, and an opaque one at full strength needs no
  arithmetic.** Those two facts, plus a table per alpha pair, are what a whole-canvas repair costs
  14 ms instead of 82 ms at 3x2048² (`docs/probes/2026-10-09-blend-cost.md`). They are fast paths in
  front of the reference rather than a second implementation of it, and the tests compare their
  output byte for byte against the reference path - a fast path that quietly changed a colour by one
  step would turn the pixel tests red before anybody saw it.

* **A selection limits writes, at the door, and mixes in premultiplied space.** The document holds
  its selection as a `Mask` and applies `restrict_to` inside `paint` and `repaint_provisional`: the
  result is pulled back towards the pixels held before by the coverage of the mask, so a brush, a move
  or a fill cannot write outside it without each one carrying the rule, and a writer cannot forget it.
  A write names which fence it wants (`PaintFence`): `selection` for a stroke, `own_coverage` for an edit
  that arrives with coverage of its own — an AI composite's feathered mask is meant to reach past the
  selection it was compiled from, and fencing it there deletes the feather and leaves the report
  describing an edit the document did not perform. With `own_coverage` staying inside the intended region
  is the caller's claim, and `bounds` is what the undo step was recorded against. A composite carries one
  more claim, also a mask rather than a fence: its `extent` says how far a write may exist (a layer's
  `alpha_coverage` for an edit, the canvas filled for a generation) and is multiplied after the feather,
  so confining a write to where its layer exists and keeping that write's edge soft are one operation.
  Mixing *premultiplied* rather than straight RGBA is what makes partial
  coverage mean "part of this stroke happened": straight-alpha mixing changes a pixel's colour when
  only its coverage was meant to change. `test_mask` compares the result against a separate
  double-precision scaling of the write, and pins the two ends exactly: coverage 0 leaves the pixel
  untouched, coverage 255 leaves the write whole, and in between alpha is within 1 LSB and colour
  within 2 of scaled arithmetic — two roundings, the blend's and this one's.
* **Shapes are producers of coverage, never kinds of selection.** `core/maskops` turns a `MaskShape`
and a `Rect` into coverage bytes and hands them to `Mask::op`, so the rectangle and the anti-aliased
ellipse go through the same `combined()` arithmetic as everything else that writes a mask. That is why
the two commands, the two marquee tools and the file format exist at all: a row in `kMaskShapeInfo`
plus a row in `kTools`, with no new type anywhere. It is also what makes an ellipse's soft edge
meaningful — a stroke landing half on a half-covered pixel happens half, and `add`/`subtract` compose
curves and boxes without a case for the pair.

* **A layer mask hides, and never edits.** `Layer` holds `std::optional<Mask>`, and the composite
  reads the layer through it (`scale_by_coverage`, the same premultiplied mix as `restrict_to`, with
  nothing underneath) except where the mask answers for itself: coverage reaching nowhere in the region
  takes the layer out of it, and a wholly shown mask leaves the layer's own bytes to blend, which is the
  state `mask.add` and `mask.fill{show}` produce. Both questions are answered from the mask's per-row
  tallies, so neither costs a pass and each saves two; what a *partly* covered mask costs is the blend
  of semi-transparent pixels, which any 50%-opacity layer costs too.
  Painting is unaffected: a mask says what shows, not what exists, so a stroke
  under a masked-out area is real and comes back when the mask moves. Two consequences that are not
  obvious from either type: a *lone* masked layer no longer takes the "a stack of one is that layer"
  shortcut, because that shortcut hands back the layer's own bytes and would show what the file hides;
  and a mask change widens the pending region to the **whole canvas**, because what a mask hides lies
  outside its bounds too, so a box taken from either mask would leave the composite showing the mask
  that used to be (`test_document` compares the incremental repair against a rebuild after a mask
  moves, precisely because that is the bug a bounds-based region would hide).
* **A mask is a plane, so painting it is a write and not a subsystem.** `Mask::paint(rect, ink, mask_ink)`
mixes coverage towards one end of the scale by the ink, the same rule the colour door uses, so two coats
at half strength come to three quarters rather than to a union of two claims, and the ends stay exact.
`Document::paint_mask` is the door: it takes the selection fence, the `unchanged` test that keeps a
stroke which moved nothing from costing a step, the repair rectangle and the history record, and
`History` steps carry their `LayerPlane` so undo restores the plane it was taken from. Three things a
gesture has to get right and no type checks for you: it holds **one** plane for the whole press (the
held-aside bytes are that plane's, so `PointerStroke::plane` is fixed at button-down and the document
refuses a write from the other); the plane rows are copied addressed from each side's own rectangle,
which is the arithmetic that ran off the end of the plane the first time a stroke started below row 0;
and taking a mask off drops the steps that name its coverage, because restoring them into a layer
without one would replace the whole canvas with a mask nobody painted. `mask.add` and `mask.remove` are
therefore not steps, while `mask.fill` and `mask.invert` are: they write coverage through the same door
a brush does.
* **`paint_target` is a claim, not a shadow of the document.** Strokes go to the active layer's
  `pixels` or `mask`; nothing resets it when the layer changes or the mask goes away, because a reset
  policy would be a second fact to keep true. A stroke aimed at a plane the layer lacks is refused
  naming the layer and the plane, `app.state` reports the aim and `stroke.plane` the plane the press is
  holding, and `mask.get` answers whether that plane is there at all.
* **`select.from_mask` and `mask.from_selection` are copies**, which is why a selection and a layer
  mask are one type rather than two with an adapter between them: an adapter needs rules, and rules
  are where "the mask moved by a pixel" and "the partial coverage got rounded" come from. Converting
  costs no undo step on either side and leaves the source where it was.
* **An edit that changes nothing is not an edit.** A stroke the selection stopped entirely writes no
  bytes, so it marks nothing dirty, bumps no revision and costs no undo step — otherwise painting
  against a small selection would spend the history restoring bytes that already match, and the undo
  depth would stop meaning "edits".

* **A repair rebuilds its region from transparent** rather than patching it, so the number of
  layers that came and went since the region was last correct is irrelevant. A structural change
  widens the pending region to the whole canvas, because nothing is known correct.

`tests/test_document.cpp` holds the property that makes partial repair safe: after strokes,
layer additions, selection moves, undo, redo, a deletion, a hidden layer and a faded one, the
incrementally repaired composite equals one built from scratch, byte for byte. Hiding and fading
are in that list because they change what the composite is made of without changing a pixel, and
they are not in history: like adding and removing a layer, they enter no undo step.

Two ways out if a document gets big enough for the whole-texture upload to dominate, cheapest
first: a dirty-rect upload (the composite already knows which rectangles changed; the backend
either needs partial updates or the tile cache below), or the GPU stamp preview that was the
original plan - stamp quads during the stroke, no upload until release. Neither is implemented;
the measurements above decide.

Upgrade path when we need it (not MVP): split each layer into 256×256 tiles, one
`sg_image` per tile → cheap partial updates, bounded VRAM, and free large-canvas support.
Keep `ImageBuffer` behind an accessor API so the tile migration is local to `core/gfx`.

## Undo, journal, and the blob store

Content-addressed storage makes destructive editing *and* cheap undo coexist:

* `blobs/<sha256>` — immutable, refcounted, zstd-compressed PNG or raw.
* `Layer.source` is just a `BlobRef`. **An AI op = write a new blob + swap the pointer**,
  so undo/redo of a 3-minute generation is a pointer swap, and no generation is ever lost.
* Pixel edits push rectangle deltas into the undo stack: one entry is a layer, the rectangle an
  operation declared it would touch, and the bytes it held - so a step belongs to the layer it
  was made on and stays undoable after the selection moves. An undo hands the pixels it
  displaces to the redo stack instead of keeping a second copy. Budget by stored bytes
  (`kHistoryBudgetBytes`, 128 MB) dropping **whole steps oldest-first**: losing an old step is
  a shorter history, losing part of a step would be a half-restored edit. Tiles become the
  storage granularity when partial texture uploads need them. Deleting a layer drops the entries
  naming it from both stacks: a change to a layer that is gone cannot be put back, and the
  unrelated steps around it are worth more than the half-restored one that would remain.
  Structural commands (add/reorder/visibility/opacity/mask) are planned to store the inverse
  command; today they are **not** undoable, and adding or deleting a layer, hiding one or fading
  one does not enter the timeline.
* Undo and redo are commands like any other (`edit.undo`, `edit.redo`, bound to `Ctrl+Z` and
  `Ctrl+Y`), so a driver can put the pixels back and compare checksums without a keyboard.
* `history.jsonl` — append-only *journal*: every op with timestamp, params, prompt, model,
  seed, blob hashes, timings. Two jobs: (a) Provenance panel ("which prompt made this
  layer?" → one-click **Regenerate with same params**), (b) recovery/replay after a crash.

This is the strongest argument for our own container: provenance is not an afterthought,
it's a first-class file-format feature.

## File format

**Decision: our own container, and only ours.** `.noise` = ZIP, atomic save (`tmp` + rename),
unknown entries preserved on load. MIME `application/x-noise+zip`, binary `noise-edit`.
**No OpenRaster**: no `stack.xml`, no `.ora` import or export (MVP decision log). The layout below
is not a compatibility projection of anyone else's format — nothing in it exists to be read by
another application, which is what lets the members say exactly what the model means.

```
mimetype                       "application/x-noise+zip"  (stored, first entry, no extra fields)
manifest.json                  app doc: format version, doc size, dpi, layer tree (our richer model),
                               selection state, viewport, prefs snapshot
mergedimage.png                flattened composite, regenerated on save: a thumbnail and a quick
                               look without decoding the layer PNGs
layers/<n>/layer.png           per-layer payload (or .webp when fully opaque and large)
layers/<n>/mask.png            optional per-layer mask
provenance/<op>.json           AI request/response metadata per generated layer (prompt, model, seed,
                               timings, provider, prepped-input hash)
blobs/<hh>/<sha256>            immutable payload store (undo history, variants, drafts)
history.jsonl                  append-only journal
variants/<op>/<i>.png          kept AI variants not committed to a layer
Thumbnails/thumbnail.png
```

### What is written today: `format` 4

The subset the app can represent, read and written by `io/project.{h,cpp}`, which refuses what it
would have to guess about. Costs of the v0 layout are in
`docs/probes/2026-10-09-container-v0.md`.

**Version policy: always bump, always refuse.** A new field bumps `format` even when it is
optional, and a file of any other version is refused naming the number it met and the number
this build speaks. There is one writer, it is us, and the alternative — defaulting a field the
layout stopped writing — is how a document opens subtly wrong. `format` 2 added `visible`,
`opacity` and `mode` to every layer row, which is what the Layers panel and the compositor act
on; a row that omits one is refused with the field and the layer named, and a `mode` outside
`core::kBlendModes` is refused naming both the value and the names that would have worked.
`format` 3 added the selection plane: an extra member the manifest declares, absent when the
document has no selection. `format` 4 added a layer's own mask plane, named by that layer's row for
the same reason - "no mask" and "a mask that hides everything" are different documents. Each is a
layout change rather than a widened name, so earlier numbers are refused.

`mode` grew from one name to the MVP eight (`normal`, `multiply`, `screen`, `overlay`, `darken`,
`lighten`, `color-dodge`, `color-burn`, spelled as PDF 1.4 and CSS spell them) **without a `format`
bump**, and that is deliberate rather than an oversight: a wider vocabulary does not change the
meaning of any field, and a build too old to know `color-burn` refuses the file by naming the mode
and the list - a loud refusal, which is exactly what `format` exists to guarantee. Bumping on every
added enum value would make the number mean "the day someone edited a table" and stop meaning
"this layout is a different layout".

| member | written | read |
|---|---|---|
| `mimetype` | stored, first member, no extra fields | must match exactly or the file is not ours |
| `manifest.json` | `format`, `size: [w,h]`, `layers: [{name, src, visible, opacity, mode}]`, and `selection: {src}` when there is one | `format` must be 3; `size` must agree with the PNG, and both numbers are named if they do not; `src`/`visible`/`opacity`/`mode` are required, `name` is not, because a file from elsewhere may not have named its layers |
| `layers/<n>/mask.m8` | a layer's mask plane, same shape, written only when that layer has one and named by that layer's row | read only when the row declares it: no row is no mask, and `covered == 0` is a mask that hides the layer, so the two must not collapse |
| `selection.m8` | the selection's coverage plane, one byte per pixel, canvas-sized, written only when the document has a selection | read only when the manifest declares it: **no row is no selection, and a declared plane that is missing or the wrong size is refused** — guessing which would turn a corrupt file into a silently unclamped document |
| `layers/<n>/layer.png` | one per layer, bottom-first | one PNG per declared row, each covering the whole canvas: a layer of another size is refused naming both sizes, because cropping or padding it silently is data loss |
| `mergedimage.png` | the composite — which is the layer only when there is one layer and it is visible at full strength | optional |
| anything else | — | carried through and rewritten byte-identical, so opening a file written by a newer build cannot delete what this one cannot show |

Save is `<path>.noise-tmp` + rename: a write that fails leaves the previous file intact and
readable. ZIP members are stored (not deflated) with fixed DOS timestamps, so equal content gives
equal bytes and a container can be diffed; the reader accepts stored *and* deflated members, and
validates bounds, CRC, declared sizes, compression method, encryption, zip64 and multi-disk limits
against a file it treats as hostile. `doc.save` writes only `.noise`; a flat image is
`doc.export`, which does not take the document's path nor clear its unsaved mark.

The layer's *state* is file content, so the two checks below also assert that a `visible: false`
the manifest declares is a `mergedimage.png` that actually dropped the layer: the flattened image
is read by things that never read the stack.

Two checks carry this section, because our own reader agreeing with our own writer proves nothing:
`tests/test_zip.cpp` parses the end-of-directory record by hand (a central-directory size that does
not reach the end record is refused - `unzip` tolerates that error, Python's `zipfile` does not),
and `tools/container_probe.py` has `zipfile` read a project the app saved and decode its layer PNG.

`provenance/`, `blobs/`, `history.jsonl`, `variants/` and `Thumbnails/` are not written yet; nothing
about them is invented ahead of the milestone that needs them.

### Why not adopt an existing format wholesale

| Format | Verdict | Reason |
|---|---|---|
| **OpenRaster (.ora)** | **rejected** (human decision, out of the MVP and of the design) | It was the only *open* layered interchange (Krita, MyPaint, GIMP…), and adopting its layout cost us nothing on day one because both formats are ZIP + per-layer PNG. What it bought was the ability to open someone else's file, and what it charged was a **second semantics for the same stack**: `stack.xml` attributes against `manifest.json` fields, `mode="multiply:0.5"` against `BlendMode` + per-layer `opacity`, masks at ORA offsets against our own — every layer feature then lands in two places that have to agree. Provenance, the one thing this project exists to keep, has nowhere to go in it. Flat PNG/WebP/JPEG carry everything the MVP needs to hand out. |
| **Photoshop PSD/PSB** | export-only, M5 | Adobe published the official spec in 2024, so writing it is legitimate work rather than folklore — but blend-mode legacy pairs, channel packing and mask encoding are a week of pain. Interop value is real, fidelity of *our* features is not. |
| **KRA / XCF** | reject | app-private, no stable spec for third parties, reference libs are GPL (licence gravity onto our binary) |
| **Multi-page TIFF** | reject | pages ≠ layers: no blend modes, no masks, no stack semantics |
| **Layered EXR** | maybe, later | excellent for 16/32-bit float AOVs, useless for a blend-mode stack; keep as an export for a future linear-light pipeline |
| **PNG, JPEG, WebP** | **read and write, flat** (MVP) | One frame, no stack, so they are exchange formats and nothing more: `doc.open` reads them as a single layer and `doc.export` writes one. Read by content, written by extension. JPEG has no alpha and composites against white, which the export answers with. AVIF / JPEG-XL are not read or written: nothing in the MVP needs them. |
| **SVG** | reject | not a raster layer model |

Round-trip guarantee we *can* honestly make: `.noise` written by this build and read back is
byte-equal in every member it does not rewrite, and a document saved and reopened keeps its layer
state — visibility, opacity, blend mode — exactly.

Flat exports are exact for PNG and lossy for JPEG and WebP, because that is what those formats are;
what is guaranteed about them is that **another implementation reads what we wrote and we read what
it wrote** — dimensions, colour within the codec's own error, and alpha straight rather than
premultiplied. That is measured, not assumed: `tools/image_interop_probe.py` (ctest
`test_image_interop`) sends every export to Pillow and opens files Pillow wrote, having found that
measuring the difference in int16 overflows and invents a mostly-black reference.

A file that is not ours (an `.ora`, a ZIP without our `mimetype`, a `.noise` of another `format`)
is **refused naming what met us**, not opened partially and not guessed at.

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
* **A command declares the argument names it reads, and the table refuses every other key** naming
  the argument, the command and the names that would have worked (`'colour' is not an argument of
  tool.set; it takes 'color', 'flow', …`). An argument that is quietly ignored is the worst kind of
  wrong: the call answers `ok` and the caller goes on believing it - `tool.brush` selects a tool and
  reads no fields, and a brush sweep that set `size` through it measured one default brush under five
  labels (`probes/2026-10-09-paint-and-panel-cost.md`). `command.list` carries each command's `args`
  so a caller asks instead of guessing, and a registration that declares a name twice or declares the
  empty name is refused, so the refusal cannot be made to lie about itself. A command whose fields come
  from a table of its own (the brush fields, `kBrushFields`) builds its declaration from that table
  rather than restating the names.
* **Key bindings are part of a command's registration**, so `command.list` reports the whole
  keymap and nothing can be bound in one place and documented in another. Shortcut text is
  canonical (`core/shortcut.h`: modifiers Ctrl, Alt, Shift in that order, then a key - `F`,
  `Ctrl+S`, `Space`, `PageUp`), the table refuses a malformed one and refuses to bind a key
  twice, and the host translates a platform key event into the same text (`app/shortcuts.h`).
  `tools/wire_input.py` presses keys and moves the pointer for real, because a binding that
  no event can produce would otherwise look fine on the wire - and because the host's own routing
  is invisible otherwise: gating the button release on the *paint* gesture made every wire-driven
  move gesture work and every real drag hang, and no command-level test could see the difference.
* Frame loop services the wire between input and draw, so requests are handled at frame
  boundaries and see consistent state; long commands return a job id rather than blocking.
* Baseline command set, growing with the app:

| Group | Commands | Why |
|---|---|---|
| state | `app.state`, `command.list`, `wire.info`, `doc.get`, `layer.list`, `select.get`, `server.state`, `job.list`, `log.tail` | read what the app believes, cheaply |
| drive | `select.rect {rect, mode?}`, `select.all`, `select.clear`, `select.invert`, `select.from_mask`, `select.get`, `mask.from_selection`, `command.invoke`, `app.quit`, `doc.new/open/save/export {path, quality?}/get`, `image.get`, `log.level`, `view.fit/set/pan/zoom`, `edit.undo/redo`, `tool.stroke {points, tool?, size?, hardness?, opacity?, flow?, spacing?, color?}`, `tool.pointer {phase, at}`, `tool.set {tool?, size?, hardness?, opacity?, flow?, spacing?, color?}`, `tool.<name>`, `brush.larger/smaller`, `layer.list/add/remove/select/set {index?, visible?, opacity?, mode?}`, `mask.from_selection/add/remove/fill {to}/invert/target {plane}/get`, `screenshot`, `pixels.compare`, `wire.info` |
 `tool.stroke` drives the *real* stroke path, so painting is testable without a mouse; its brush fields are declared and every one is optional, inheriting what the app is on, so a recorded stroke that sends them replays without knowing what the UI had selected. The fields that belong to `tool.set` are not accepted here and vice versa, and both say so. `tool.pointer` is the same path one event at a time - the UI drives the pointer through this command, so a gesture can be recorded and replayed. `tool.<name>` is generated per row of `core::kTools`, so a tool exists in exactly one table |
| pixels | `image.get {region, base64}`, `screenshot {region, name, base64}`, `pixels.compare {a, b}` | screenshots exist so a vision model can look; `pixels.compare` is the pixel-lock invariant made checkable. It decodes either path through the flat-format table, so a golden may be a PNG, JPEG or WebP — what is compared is pixels, not a container |
| ai | `ai.compose {image, apply?, feather?, colour_match?, lock_band?, extent?}`, `ai.preview {recipe, args}` (compiled request + prepped PNG, **no send**), `ai.run`, `job.wait {id}`, `cache.stats` | `ai.compose` lands a returned image on the document and reports the effective coverage, so pixel-lock is checkable without a server; the rest verify what we *would* send before spending minutes of someone's GPU |

Live now: the state/drive/pixels rows minus `server.state` and `job.list`, and `ai.compose` from the ai
row; `ai.preview`, `ai.run`, `job.wait` and `cache.stats` arrive with their milestones.
`app.state` reports `frame` (frames presented), `frame_ms` (last presented frame), `canvas`
(origin + size of the canvas content area in framebuffer px) and the view transform
(`scale`, `origin`, `image`, `fit_pending`), the brush and tool the last stroke ran with, the
document's `history` depths, and its `selection` (`active`, `bounds`, `covered`, `area`, `canvas`,
with `canvas` present only while one exists, because a sized mask that covers nothing is a real state
and must not read as no selection). `covered` counts pixels touched, `area` adds the coverage bytes
up: for a rectangle they are equal, for an anti-aliased curve `area` is what the ellipse measures and
`covered` is the pixels it grazes. While a pointer gesture is down, `stroke` also reports `marquee` —
the box the select tool is drawing, live, and `marquee_shape`, which of them it is: the only way to
tell a marquee that tracks the pointer from one that appears on release, and the only way to see which
tool row a gesture was started with. The mask is reported the same way a selection is - `layer.list`
rows carry `mask` (`covered`, `area`, `bounds`, or `null` for none), `mask.get` adds `target`, and
`brush.paint_target` says where the next stroke goes - so a driver can tell a mask that hides
everything from a layer that has no mask, and a stroke that painted colour from one that painted
coverage. Enough for a driver to place an image rect on screen, wait
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
| **D10** | Recipes | pre-shipped, read-only JSON in the app resource dir — the format the manifest and the wire already speak, so no second parser. No in-app authoring. |
| **D11** | Backend scope | Lemonade only, `thenoise` recipe only; N server profiles (LAN/localhost), always keyed. |
| **D12** | Naming | extension `.noise`, binary `noise-edit`, config dir `~/.config/noise-edit`, MIME `application/x-noise+zip`. |
| **D13** | Model references | roles only (`generate.photo`, `generate.illustration`, `edit`, `upscale`, `enhance`); one model per role per profile; candidates ranked by quality/speed in `roles.json`. |
| **D14** | Observability | debug wire from M0, dispatched through the UI command table; every feature ships its wire command. |
| **D15** | Ground rules | `AGENTS.md` is normative and overrides prose elsewhere in this repo. |
