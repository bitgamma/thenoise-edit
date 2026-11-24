# Architecture

Every claim here is either in the tree or marked **(planned)** / **(out)**. What a person is doing is
[PRODUCT.md](PRODUCT.md); the order of work is [PLAN.md](PLAN.md); how any of it is proven is
[TESTING.md](TESTING.md).

1. **Stack**, **Module layout**, **Threading model** - what the binary is made of and who may call whom.
2. **Compositing**, **Painting path**, **Undo, journal and the blob store**, **File format** - the editor:
   layers, strokes, history, and the `.noise` container.
3. **The AI path** - the server contract and the two machines in it: golden rule, roles, region compiler,
   field policy, server session, transport, result compositor, draft/refine, recipes, failure handling.
4. **Debug wire**, **Decisions (locked)**.

---

## Stack

| Concern | Choice | Why |
|---|---|---|
| Language | **C++20** (no exceptions in hot paths, no RTTI) | ImGui is C++; doc model needs variants/RAII; sokol headers bind from C++ trivially |
| GPU | **sokol_gfx**, backend `SG_BACKEND_GL3` first | Metal/D3D11/WGPU one flag away; GL3.3 is the boring, works-everywhere choice |
| Platform | **sokol_app** (decided: no pen/tablet in MVP) | zero extra deps, same API shape; if tablet pressure ever becomes a requirement, SDL3 goes behind the same shim |
| UI toolkit | **Dear ImGui** + docking branch, via `sokol_glue.h` + `sokol_imgui.h` | fastest path to a full editor chrome |
| Native file dialogs | **Native File Dialog Extended 1.4.1** | separate OS-owned windows, with a parent handle from Sokol |
| Image codecs | **libpng** (lossless, alpha), **libwebp** (opaque payloads), **libjpeg** (JPEG export) | system libraries found by `pkg-config`, all present on this machine; `libjpeg-turbo` is a drop-in if export speed ever matters |
| HTTP | **libcurl** (easy interface, worker threads) | ubiquitous, no TLS surprises |
| JSON | **nlohmann/json** | config, model profiles, manifests |
| Compression | planned: **libzstd** for blobs, deflate for zip entries | nothing is compressed yet: the blob store and `.noise` entries are stored today |
| Container | **miniz** (single-file, MIT) for the project zip | |
| Vector overlay | **the ImGui draw list** today; **sokol_gl** (immediate) or a tiny instanced-quad path if an overlay ever has to be shaded | marching ants and the selection outline are line segments on `ImDrawList`, with the *geometry* - which pixel of the mask boundary is on which side - from `app/ants.{h,cpp}`. A draw-list element per dash is fine at a mask's perimeter and would not be at one per canvas square, which is the mistake `ui_vertices` exists to catch |
| Inference backend | **Lemonade Server**, `thenoise` recipe only | we are a client; the model box is AMD/ROCm, usually on the LAN, bearer-keyed |

Build: CMake + `FetchContent` (sokol, imgui, miniz, nlohmann, Native File Dialog Extended). System
libs via `pkg-config`, all required for the native target: libpng16, libjpeg, libwebp, libcurl (the
AI transport) and GTK 3 development files for Linux native dialogs.
The optional Emscripten target uses Emscripten codec ports and WebGL2, without libcurl or a
worker thread; it is a local-only editor until an asynchronous browser AI transport exists.
Everything statically linkable into one binary on Linux or a WASM/JS asset bundle in the browser. No OpenCV (heavy, wrong
licence gravity for what we need; the ~400 lines of healing/filtering we want are ours).

**Cross-platform by design, Linux-first in practice.** No Windows/macOS build scripts yet,
but nothing may foreclose them:

* one `CMakeLists.txt`, no hand-written makefiles, no shell-only build steps;
* platform-specific code remains behind the `src/platform/` boundary for time,
  paths and sockets. `file.pick` uses Native File Dialog Extended through
  `src/platform/file_dialog_nfd.cpp`: requests can be posted from the debug wire,
  and the host opens a parented OS dialog on the main thread. The dialog is a
  separate movable native window; the command and result routing stay in the
  shared file command table.
  `file.pick.state` and `file.answer` remain the shared command interface. The
  WASM build retains the separate JavaScript file picker and browser downloads.
  The rest of the application is still Linux-first, and a full Windows/macOS
  application build will require separate platform/backend work;
* UTF-8 internally, `std::filesystem::path` (from/to UTF-8) at the boundary;
* no POSIX-only primitives outside the shim: std threads/mutexes/atomics, no `fork`, no
  `poll`, no signal-based shutdown, no `dlopen`;
* case-insensitive-path-safe (never two assets differing only in case);
* sokol / ImGui / CMake cover Win32, Cocoa, X11+Wayland already — the port is a build task,
  not a refactor;
* CI is Linux now; Win/mac stay a written-down seam rather than a rotting disabled workflow: the port is
  a build file plus one `file_dialog_*` implementation per target, and nothing else has to change.

## Module layout

Two markers, so the map can carry intent without lying about the tree. **(planned)** = scope from
[PRODUCT.md](PRODUCT.md) with no file and no caller yet; the line says what it is for and what stands in for it
today. **(out)** = built and removed by decision; the line carries the reason, and the decision is
recorded in §Decisions (locked). Anything unmarked exists and is imported.

```
src/
  core/        # no deps, no GPU, no IO — pure data + maths, unit-testable
    image.{h,cpp}      ImageBuffer (RGBA8 straight alpha, stride-aware, tiles later), and
                       `content_bounds`: the smallest rectangle holding every pixel with any alpha -
                       the *picture*, as distinct from the canvas-sized rectangle a layer stores. The
                       move tool and `scale_layer` are both aimed at that, so dragging a layer and
                       scaling it agree on which part of a layer "it" is.
    document.{h,cpp}   Document: one active version's stack, path, revision and composite,
                       plus archived native-resolution versions with independent history.
                       `scale_layer` is the only place that says what resizing
                       a layer means (see §Undo): the layer keeps its canvas-sized store, the picture
                       inside it is resampled about its own middle along with any non-trivial coverage,
                       and a factor that would put either of them outside the canvas is refused with the
                       largest one that fits
    layer.{h,cpp}      Layer (id, name, pixels, optional mask, visible, opacity, mode) and generated
                       names; a stack is bottom-first. `normalize_layer_name` and `layer_name_rule` are
                       the whole of what a layer may be called - the panel's field, `layer.rename` and the
                       container reader all ask here, so no file and no keystroke can leave a layer named
                       something another of them would refuse. `LayerPlane` names which of a layer's two
                       planes - `pixels`, `mask` - a write or a step belongs to, and its name table is
                       the vocabulary `layer.plane` and `stroke.plane` speak
    history.{h,cpp}    undo/redo as stacked (layer, plane, rectangle) deltas, budgeted by stored bytes,
                       as structural entries that put a layer back, take one away, reorder the stack or
                       replace it whole, as swapped planes for the two coverages - the document's marked
                       area and one layer's mask, present or absent - and as a swapped name, so an edit
                       that created a layer is one step, not a layer plus a write, and so is a gesture that
                       moved the mark, put a mask on, or mistyped a name
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
                       `MaskOp`, and `rasterize_lasso` which fills document-coordinate paths with an
                       even-odd scanline rule into a bounded `MaskPatch`. Both write the existing
                       coverage plane; neither introduces a selection type. Shape names come from
                       `kMaskShapeInfo`; lasso is a path command and a row in `kTools`. The ellipse
                       uses 16 vertical strips per boundary pixel and the lasso uses 16 subscanlines
                       per row for deterministic antialiased coverage.
    blur.{h,cpp}       the box blur over an RGBA-ish plane, and `feathered_mask`: three passes of odd
                       box sizes, because anything wider is a different kernel. Radius 0 is the
                       identity, so "no feather" is a number rather than a branch, and the plane is
                       clamped at the ends rather than wrapped - a mask feathered at the canvas edge
                       must not pick up the far side of the image. `ai/region` prep and `ai/compose`
                       soften with this and nothing else.
    resample.{h,cpp}   Lanczos-3 over premultiplied alpha, with the support widened when the image
                       shrinks so no source pixel is dropped. A coverage plane goes through
                       `resample_coverage_lanczos3` - the same taps, clamped at the edge rather than
                       wrapped, and never renormalised, so an in-outside boundary cannot manufacture
                       coverage and a fully-covered area stays fully covered. `resample_margin_px` says
                       how many output pixels the filter's reach adds at a given stretch, which is how a
                       caller cuts a resampled result at the bounds it asked for instead of keeping the
                       ringing as if it were content. Premultiplied because the coverage edge
                       of a returned image is the feather, and averaging straight alpha drags a
                       transparent pixel's colour into a visible one; the weights are normalised among
                       themselves, never by the coverage. At 1:1 it returns the input byte for byte -
                       the kernel is interpolating and the weights are exact, which is what lets the
                       result compositor resample unconditionally without breaking pixel-lock.
    heal.{h,cpp}       (planned, M5) Telea / Chan fusion inpainting, local and offline: the half of the
                       `heal` recipe that runs with no server. Today that recipe goes to the server, and
                       without one the command says so rather than inventing pixels.
    command.{h,cpp}    invokable command table: commands, titles and key bindings
    shortcut.{h,cpp}   canonical shortcut text: grammar, vocabulary, normalization
    blob.{h,cpp}       (planned) content-addressed store API (impl in io/). Nothing stores by digest yet:
                       undo keeps rectangle deltas in memory and answer PNGs are per-run scratch files.
                       See "Undo, journal, and the blob store" for what has to be true when it arrives.
  gfx/
    renderer.{h,cpp}   sg init, passes, resource lifetime, `read_framebuffer` (what `screenshot` reads)
                       and `Texture::sync`, which uploads the document's composite when its revision has
                       moved and not otherwise
    checker.{h,cpp}    the transparency checker as one 2x2 texel tile, repeat-sampled across the canvas
                       rect: four vertices at any zoom (Compositing says what the alternative costs)
    compositor.{h,cpp} (planned) layer stack → composite texture (ping-pong, per-layer blend). The stack
                       is composited on the CPU by `Document::composite` and uploaded as one texture;
                       the GPU pair is a migration behind that same call, and `core/blend`'s reference is
                       what it will have to match, mode for mode
    texturecache.{h,cpp} (planned, with the compositor) blobId → sg_image, eviction, upload policy
    strokes.{h,cpp}    (planned) GPU live-stroke preview. What paints today is the CPU provisional edit:
                       `Document::begin_provisional` and friends, under the Painting path below
    overlay.{h,cpp}    (planned as a module) guides, ghost regions, wipe slider. Ants exist and are not
                       here: `app/ants.{h,cpp}` draws them, `ants.freeze` pins their phase for a capture
  app/
    main.cpp           (src/main.cpp, above the app dir with the other entry glue) sokol entry, frame
                       loop, wire poll, shortcut gate, and verified Noto Sans atlas loading
    commands.cpp       assembles the whole command table from the groups below
    command_args.{h,cpp} shared argument readers and the JSON each command answers with
    brush_steps.h    the rows behind the step keys: which field, which direction, which key —
                     registered as commands by paint_commands and read back by the Brush panel
                     for its hover hint, so a binding exists once
    document_commands.cpp doc.* image.get
    paint_commands.cpp tool.stroke/pointer/set, tool.<name>, the brush step commands, edit.undo/redo
    view_commands.cpp  view.*, and the fit that waits for a stable docked canvas area
    tools/             one file per tool: brush.cpp (press/drag/release over the provisional edit),
                       move.cpp (the same gesture, sliding the layer's pixels), select.cpp (rectangle,
                       ellipse and freehand path gestures over the marked selection). Lasso paths
                       stay in document coordinates and are rasterized once on release. Which file
                       the pointer talks to is the `action` column of `core::kTools`; selection rows
                       also name their gesture there, while `PointerSelection` holds the gesture
                       and operation captured at press.
    shortcuts.{h,cpp}  platform key event -> canonical shortcut text (the only sokol-aware
                       part of the app layer)
    ui.cpp             compact application menu bar, zoom selector, status bar and dockable AI
                       workspace; it restores a valid ImGui dock layout and builds the default only
                       when the saved dockspace is missing or invalid, constraining dock-node widths
                       only during the dockspace pass; the central Canvas accepts no other dock tabs,
                       and legacy hidden central tabs recover into an existing dock or float
    ui_titlebar.cpp    on Linux/X11, one client-rendered title/menu/zoom/window-control row with the
                       bundled brand logo, clipped document label and scoped dropdown spacing; other
                       hosts retain native decorations and show the logo in the ordinary ImGui menu row
    ui_recent_menu.cpp recent-file labels use basenames, bounded width and full-path tooltips; opening
                       one dispatches the original UTF-8 path through the existing document command
    ui_text.{h,cpp}   width-constrained filename and label truncation at UTF-8 codepoint boundaries
    ui_canvas.cpp      checker, document composite, pointer interaction, selection overlays and brush
                       footprint, with the left tool palette beside the image and no persistent controls
                       above it
    ui_front_door.cpp  new-document and quit dialogs
    ui_layers_panel.cpp layer cards and stack actions in an independently scrolling upper region;
                       mask, optional edge controls and collapsed Tool Settings live in a separate
                       scrolling lower region; card names and scale factors remain command-backed fields
    ui_layer_splitter.cpp draggable division between those regions; its ratio is read and written by
                       ImGui's existing settings file
    ui_log_panel.cpp  the Log dock tab
    ui_palette.cpp    persistent grouped tool rail with selection, AI coverage and paint distinctions
    verb_bar_draw.cpp selection-relative Heal / Remove / Fill / Change actions, clamped to the canvas;
                       offline clicks use the existing ai.intent refusal and show an actionable notice
    ui_image_dialogs.cpp Image Size and AI Upscale dialogs over the existing resample and upscale commands
    ui_canvas_dialogs.cpp Canvas Size, anchor, added-area and crop dialogs over existing commands
    ui_mask_controls.cpp editable result-edge controls inside the Layers and Mask dock
    ui_workspace_settings.cpp ImGui ini handler for the AI Workspace open preference; docking
                       position and selected tab remain owned by Dear ImGui's docking settings
    ui_tool_panel.cpp  dockable AI Workspace tab, references, recipe-backed verbs and AI status
    ui_tool_settings.cpp compact, tool-aware brush settings below the mask controls
    ui_references.cpp  ordered reference rows, thumbnails, file/layer actions and drop target
    reference_thumbs.{h,cpp} GPU thumbnails keyed by reference id and content revision
    ui_verbs.cpp       recipe-backed verb rows, their fields, aspect/style selectors and model-swap
                       confirmation; owns no parallel verb list or prompt state
    ui_theme.{h,cpp}   shared semantic palette and ImGui style for dark and light themes; no
                       window control chooses between them - `ui.theme` and the persisted pref do
    ui_internal.h     declarations shared by the UI translation units    select_commands.cpp select.rect/all/clear/invert/get, and the `Ctrl+D`/`Ctrl+I` bindings
    layer_commands.cpp   `layer.plane {index?,plane?}` selects a layer's image or mask thumbnail;
                         Brush/Eraser write that plane, selection tools never do
    verb_bar.cpp          the marked-area verbs and the floating bar's wire (`verb.bar`,
                          `verb.ask`); mark-relative geometry without result-edit controls
    selection_edit.{h,cpp} job lifecycle and post-landing bar suppression: tracks a
                          job and its result layer, derives state from the queue, not a UI timer
    verb_fields.cpp       the growable phrase buffers shared by panel and floating verb boxes
    ui_prompt.h           wrapped multiline input with height fitting to the text and available UI room

    ai_commands.cpp     `ai.compose` (land a result file), `ai.roles`, `ai.recipes`, `ai.preview`
                       (the compiled request, unsent) and `ai.run` (queue it): each reports what the
                       policy and the coverage resolved to
    ai_references.{h,cpp} session-only independent references from files or one selected layer;
                       caches lossless PNG bytes and a thumbnail, tracks linked layer content revisions,
                       and exposes add/remove/move/read through the command table
    ai_job_commands.cpp `server.connect/state/load`, `job.list/get/wait/cancel/apply`,
                       the queue and the session, observable and driveable. No `job.retry`: pressing the
                       verb again *is* the retry, against what the document is now
    ai_session.{h,cpp} the tables, the transport (recordings if `$NOISE_AI_FIXTURES`, else libcurl),
                       the client, queue and registry: one object, opened once by the host
    ai_landing.{h,cpp} the one door an answer goes through the document by (`land_job`), shared by the
                       beat that lands finished jobs (`pump_ai_landings`, called by every reader of the
                       queue) and by `job.apply`, so neither can disagree about feather or coverage and
                       neither can land an answer anywhere but on its own new layer, in the document
                       whose `document_epoch` the job was queued under
    ai_intent.{h,cpp}  `ai.intent`: the AI action's state (which recipe the last verb chose, the values
                       typed for the fields it asks about, the aspect chosen for a recipe that offers
                       them, the seed pinned, if any, the job) and its one action,
                       plus the readiness every door forecasts from (`assess`, `missing_region`,
                       `needs_swap`). A refusal is recorded on the intent by the command wrapper, so the
                       next read - the panel's, every frame - still carries the sentence: a words box that
                       will not take what was typed has to say so. The window reads it once a
                       frame (status line and verb rows share that one read), which is why it encodes
                       nothing: what a press would send comes from `quote_request` - the region compiler's
                       geometry and the model's own defaults - and `upload_encodes` counts every prep +
                       resample + PNG encode, so "not running costs nothing" is a number on the wire. The window draws this command's answer and owns nothing else, so a
                       scripted run of an intent and a click are the same call sequence
    ai_verbs.{h,cpp}   `kVerbs`: one row per verb - `{VerbId, recipe, key}` - generating `ai.heal`,
                       `ai.remove`, `ai.fill`, `ai.change`, `ai.generate`, `ai.blend` and the readable
                       `ai.verbs`. A row names no
                       label, no words and no scope of its own: those are the recipe's, and
                       `verb_table_error` refuses the table at session open when a row has drifted from
                       what its recipe does - including a variant that would stand in at the key and mean
                       a different verb. What a press runs is `pressed_recipe`: the row's recipe, or the
                       family member the intent holds
    mask_commands.cpp mask.from_selection, mask.add/remove/fill/invert and mask.get -
                       the two directions between a selection and the active layer's mask (both copies
                       of one `Mask`) and the mask's own life cycle; the selected mask thumbnail
                       controls whether Brush/Eraser paint coverage instead of image pixels
  ai/
    profile.{h,cpp}    what `/models` and `/health` say: a model's labels, recipe, whether it is
                       downloaded, its `recipe_options` (steps, cfg, size). Discovery parsing only -
                       no policy lives here, because the server publishes it
    registry.{h,cpp}   `roles.json` ∩ discovery → one model per role: assignment, then the file's
                       preference order, then a capability match. Nothing else in the codebase names a
                       model, and an unresolved role carries a reason and, when the box offers it, the
                       command that would fix it. The file also carries the size and context policy the
                       region compiler and the request builder are not allowed to invent
    recipes.{h,cpp}    `recipes/*.json`: endpoint, role, operation, region scope, hole prep, enhance,
                       additional-reference default, prompt lines, variables, composite policy,
                       the `aspects` a generation offers and
                       the `variants` that stand in for it at its key. A recipe names a role,
                       never a model, refuses keys that belong to the model (`steps`) or to nothing
                       (`mask`), refuses any pairing of `operation` with a hole it may not cut, and
                       `variant_errors` refuses a family whose members are not interchangeable once the
                       whole table exists to resolve them
    provider.{h,cpp}   THE request builder and the only place a field can be added: one ordered field
                       list plus an ordered image collection, encoded as JSON or multipart by the
                       transport, and the readable per-image summary that `ai.preview` answers with
                       before anything is sent
    transport.h        the HTTP seam: method, URL, headers, body, timeouts, a cancel flag. Everything
                       above it is the same whether the answer comes from libcurl or a recording
    http.{h,cpp}       `Client`: health / models / load / run, and the failure classes - cancelled,
                       server_down, timeout, auth, no_model_loaded, model_not_found, not_edit_capable,
                       size_rejected, refusal, bad_image - each with the thing to do
    http_curl.cpp      the libcurl easy transport: TLS verification, timeouts, bearer header,
                       cancellation from the transfer callback
    recorded.{h,cpp}   the same transport from committed JSON recordings, answering on the caller's
                       thread: captures ordered multipart parts and exact PNG bytes, replays delays,
                       transport errors and every refusal shape. The default `ctest` run is this
    server.{h,cpp}     ServerProfile (base URL, key, TLS, auth form, `/v1` vs `/api/v1`) read from
                       `$THENOISE_URL` / `$THENOISE_API_KEY`, and the latency EMA that turns a shape
                       into an ETA and a request timeout
    region.{h,cpp}     THE region compiler (see §The AI path): `plan_region` for the geometry
                       (bbox → grow → clamp → snap → fit, with the inverse map carried as the two
                       rectangles rather than a scale), `frame_for_aspect` for the rectangle a chosen
                       ratio picks on a canvas (the largest that fits, centred, one axis full - the grid
                       snap stays in `plan_region`, which owns it), `prep_region` for the bytes that
                       would go on the wire (a hole inside the coverage, every other pixel uploaded as
                       the document holds it), and `flatten_over_ring` for the colour a transparent edge
                       is dropped on
    compose.{h,cpp}    the result compositor (see §The AI path): resample onto the crop, colour drift
                       measured against the document and corrected on the answer, feather, lock band,
                       blend through `restrict_to` - given the destination it writes and the reference it
                       matches, which are different images now - and the `ComposeReport` whose effective
                       coverage is what pixel-lock is asserted against
    colour_match.{h,cpp} the drift the model put on the context band, paired pixel against pixel with
                       the document, taken back off the whole answer (spread included, clamped) — never a
                       transfer of the surrounding colours onto the new content
    queue.{h,cpp}      jobs: one worker (the box holds one image model resident), cancellation that
                       never discards an answer that arrived, and in-memory: answer PNGs are per-run
                       scratch, nothing is read back at startup, so a re-ask is a second request
    (no cache module)   (out) an answer cache existed and was deleted: keyed on a byte-identical re-ask,
                       which is an artefact of probing rather than of editing, with no bound and no
                       cleanup. MVP's decision log carries the terms any replacement has to state first

  dbg/
    wire.{h,cpp}       debug wire: JSON-lines server, request dispatch, event broadcast
    capture.{h,cpp}    (planned as a module) viewport/window/composite → PNG on disk + sha256. Today the
                       read is `gfx::read_framebuffer` and the file and digest are the `screenshot`
                       command's; a separate module is only worth having when a second capture path exists
    headless.{h,cpp}   (planned) run the same command surface without a window (SG_BACKEND_NULL)
  io/
    png.{h,cpp} jpeg.{h,cpp} webp.{h,cpp}
                       the flat formats: decode to RGBA8, encode from it
    flat.{h,cpp}       those formats as one table - extension, sniffed magic, whether it holds
                       alpha, whether `quality` means anything - plus the default export quality
    project.{h,cpp}    .noise read/write (the only layered format we read or write)
    config.{h,cpp}     app preferences: recent files and `ui_theme` in `$NOISE_CONFIG_DIR`; keymap and
                       keyring access remain planned
assets/        shipped role and recipe tables plus the Noto Sans UI font; CMake installs this tree
               in the native install prefix's shared data directory and preloads it for the browser
tests/         core + ai unit tests (no GPU), golden-image tests, and `fixtures/http` - the recorded
               server the default `ctest` run answers from
tools/         the probes that drive the running app over the wire, and the shared harness
               `wire_drive.py` (display claim, child reaping, golden capture). [PRODUCT.md](PRODUCT.md)
               tracks the editing-concept work they assert
vendor/        pinned dependencies fetched by CMake; nothing is downloaded at test time
```

Shader sources are inline in the module that compiles them (renderer, checker, ants) - there is no
`shaders/` directory, and a shader that lived in its own file would be a second place to keep a
uniform layout true.

Dependency direction: `app → {core, gfx, ai, io}`; `gfx → core`; `ai → core, io`; `io → core`.
Only `src/main.cpp`, `src/app/ui_*.cpp`, `src/app/ui_theme.h` and the UI-specific
`src/app/verb_bar_draw.cpp` import ImGui. `core`+`ai` stay testable headless.

## Threading model

One render thread (sokol is not thread-safe), one AI worker, and the truth about which one pays
for an encode.

```
RENDER THREAD (60 fps)                 AI QUEUE WORKER (exactly one, std::thread in ai/queue)
  input → tool state machine             the whole request: build, send, wait, decode
  ui tick (ImGui)                        (curl transfer, cancel flag polled in the callback)
  wire poll - commands execute here
  Document::composite → one upload
  draw canvas, ants, ImGui
```

What runs there today: PNG/JPEG/WebP encode and decode, sha256, region prep and resample, feather
and colour-match all execute on the render thread, inside the command that asked for them. That is a
cost a frame can feel, which is exactly why it is a number on the wire rather than an assumption:
`AppState::upload_encodes` counts every upload encode and `ai.intent` reports it, so "reading the
panel encodes nothing" is a test (`test_app`) and not a promise. Long work is off it only where it has
to be - the request itself - because that is the one thing measured in seconds.

**(planned)** The worker pool, the `Mailbox` of `JobResult`s and the budgeted per-frame drain are the
design for the point where decode/save of a big document stops fitting in one frame: workers produce
bytes and digests, never `sg_*` handles, and only the render thread creates and uploads textures. The
`BlobRef` in that diagram belongs to the blob store, which is planned too.

What is already load-bearing, and tested:

* The AI queue is **concurrency 1** (the engine holds one image model resident, so parallel jobs only
  thrash it), guarded by one mutex for the records and one `atomic<bool>` per job for cancellation.
  A command that reads the queue takes the mutex; a panel reads through those commands.
* A model swap is a user-visible, priced event: a press that would swap is refused until it is
  confirmed, and says which model, which is resident, and what loading it costs.
* The document stays fully editable while a job runs, and a finished answer lands itself at the first
  door that reads the queue - so nothing waits on the canvas, and no frame waits on a job.
* Autosave and crash recovery of the *document* remain MVP scope and are not built. A **durable job
  queue is out by decision**, not by deadline: a request in flight belongs to a document revision that
  a restart cannot guarantee, so the answer would land somewhere that no longer means the same thing.
  Losing an in-flight job at a crash costs one request; landing it in the wrong document costs an edit
  nobody asked for (`document_epoch` is the same argument inside one run).

## Compositing

* Today the stack is composited on the CPU: `Document::composite()` walks the layers through
  `core/blend`'s reference into one RGBA8 image, and `gfx::Texture::sync` uploads it when the
  document's revision has moved. Straight alpha in storage, premultiplied where it is blended - the
  resampler premultiplies for the same reason and says so in `core/resample`.
* **(planned)** The GPU compositor: ping-pong passes, `dst = blend(dst, layer_i)` with `opacity_i`,
  `mask_i` and a per-mode branch, plus a dirty-rect re-composite clamped to the visible viewport ∪
  margin. It is a migration behind `Document::composite`'s call site, and `tests/test_blend` holds the
  numbers it has to reproduce - the CPU reference is the definition, the shader is the implementation,
  and one of the two being wrong is a red test either way.
* The canvas pass draws that one texture with zoom/pan, checkerboard underneath, then the ants, then
  ImGui. The transparency checker is one `2x2` texel tile
  drawn repeat-sampled over the canvas rect (`gfx/checker.h`), so it costs four vertices at
  any zoom. Drawing it as one overlay element per screen square instead costs
  `canvas_area / square²` and is unbounded in zoom, because the canvas rect grows with the
  scale: that overflowed the UI vertex buffer and blanked the canvas at fit zoom on a
  2400×1600 document.
* On the CPU path a visibility, opacity or blend change marks the composite dirty and costs a
  re-composite; on the planned GPU path it is uniform-only and free. Pixel edits are the only dirty
  source either way, and `Document::revision` is what says whether the upload has anything to do.

### Up to and including the active layer, not all layers

If the upload composited layers *above* the active one, those pixels would appear twice after landing: once
baked into the answer, once as the layers that are still there. Flattening up to the active layer and
landing the new layer directly above it is the only combination that composes without double-applying
content. When nothing is above the active layer the two definitions coincide, which is the common case and
why the distinction has not bitten yet.

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
  is the caller's claim, and `bounds` is what the undo step was recorded against. A composite brings its
  coverage and needs no fence of its own: the destination is empty — a fresh layer, or the blank canvas
  rule 1 of [PRODUCT.md](PRODUCT.md) lets an answer fill — so there is nothing there for a second limit to
  protect.
  Mixing *premultiplied* rather than straight RGBA is what makes partial
  coverage mean "part of this stroke happened": straight-alpha mixing changes a pixel's colour when
  only its coverage was meant to change. `test_mask` compares the result against a separate
  double-precision scaling of the write, and pins the two ends exactly: coverage 0 leaves the pixel
  untouched, coverage 255 leaves the write whole, and in between alpha is within 1 LSB and colour
  within 2 of scaled arithmetic — two roundings, the blend's and this one's.
* **Selections are coverage, never a second plane.** `core/maskops` turns a `MaskShape` and a `Rect`
  into bytes or fills a freehand path into a bounded patch, then hands either to `Mask::op`. The lasso
  uses even-odd scan conversion, including for self-crossings, and antialiases by integrating sixteen
  vertical subscanlines per pixel row. `replace`, `add` and `subtract` therefore keep the same coverage
  arithmetic for a path, a rectangle, an ellipse, a layer mask and the marked area.

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
* **The marked selection and a layer mask have different ownership and controls.** `cover`,
  `un-cover`, marquee, ellipse and lasso always change the document's transient selection. That
  area stays the input to the next selection-scoped AI edit. The selected image or mask thumbnail
  determines whether Brush/Eraser write layer colour or layer coverage: `layer.plane
  {index?,plane?}` is the wire equivalent of clicking the thumbnail, and `brush.edit_plane`
  and `stroke.plane` report the current target and the press-time target respectively. A mask
  brush uses a provisional coverage transaction and its own history step; a live mask stroke does
  not inherit an unrelated AI selection as a fence. There is no global `coverage.target` switch.
  After a selection AI result lands, the image plane remains selected; the floating
  bar is hidden until the marked selection changes. The mask remains a thumbnail click away.
  Clicking the mask thumbnail while a non-paint tool is active selects Brush immediately,
  so the red wash shows masked-out regions without an extra tool click or paint stroke.
  Eraser stays selected when already active. Selecting Cover or a marquee tool again shows the
  marked area in blue. The red wash can be toggled with `view.mask_overlay`; neither preview
  changes saved pixels.
* **`select.from_mask` and `mask.from_selection` are copies**, which is why a selection and a layer
  mask are one type rather than two with an adapter between them: an adapter needs rules, and rules
  are where "the mask moved by a pixel" and "the partial coverage got rounded" come from. Converting
  leaves the source where it was. The copy is one undo step wherever it lands: the marked area's is the
  coverage it replaced, a layer's is the mask plane it replaced - including no mask at all (§Undo).
* **Marking is an edit, and a gesture of it is one step.** One press — marquee, `cover`, `un-cover` —
  holds the coverage it found and commits it once at release (`begin_marked`, `repaint_marked`,
  `commit_marked`, `cancel_marked`), which is the door the layer's provisional edit uses for its own
  bytes and buys the same two things: one step rather than one per pointer event, and one **coat** at
  the brush's opacity rather than one coat per frame. `select.all`, `select.invert`, `select.clear` and
  `select.from_mask` are each the one step a command is. Writing the coverage that is already there is
  not an edit, by the rule every write follows; a `cover` press that would rub out a mark and a brush
  press that would paint colour are separate presses, because one gesture holds one plane.
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
they are not in history, the one kind of change the timeline does not carry: a layer arriving, going,
reordering or having its mask put on or taken off is a step, a visibility click is not.

Two ways out if a document gets big enough for the whole-texture upload to dominate, cheapest
first: a dirty-rect upload (the composite already knows which rectangles changed; the backend
either needs partial updates or the tile cache below), or the GPU stamp preview that was the
original plan - stamp quads during the stroke, no upload until release. Neither is implemented;
the measurements above decide.

Upgrade path when we need it (not MVP): split each layer into 256×256 tiles, one
`sg_image` per tile → cheap partial updates, bounded VRAM, and free large-canvas support.
Keep `ImageBuffer` behind an accessor API so the tile migration is local to `core/gfx`.

## Undo, journal, and the blob store

Two mechanisms, at two stages. **Built:** rectangle deltas in memory, which is what undo is today.
**Planned:** content-addressed storage, which is what makes destructive editing and cheap undo coexist
once a rectangle delta stops being the cheap thing. The planned half is MVP scope (`journal/provenance
panel`, "Regenerate with same params", kept variants) and nothing in the tree stores by digest yet.

* **(planned)** `blobs/<sha256>` — immutable, refcounted, compressed PNG or raw.
* **(planned)** `Layer.source` is just a `BlobRef`. **An AI op = write a new blob + swap the pointer**,
  so undo/redo of a 3-minute generation is a pointer swap, and no generation is ever lost. Today an
  answer lands as a layer holding its own pixels and the PNG behind it is per-run scratch: the *layer*
  is the thing undo removes, which is why an answer's pixels survive a `Ctrl+Z` only as long as the
  history step naming them does.
* Pixel edits push rectangle deltas into the undo stack: one entry is a layer, the rectangle an
  operation declared it would touch, and the bytes it held - so a step belongs to the layer it
  was made on and stays undoable after the selection moves. An undo hands the pixels it
  displaces to the redo stack instead of keeping a second copy. Budget by stored bytes
  (`kHistoryBudgetBytes`, 128 MB) dropping **whole steps oldest-first**: losing an old step is
  a shorter history, losing part of a step would be a half-restored edit. Tiles become the
  storage granularity when partial texture uploads need them.
  Structural edits live in the timeline as state swaps or reversible entries rather than as pixels:
  a reorder (`layer.move`) records the stack's ids in order and keeps the order it displaced, because no pixel moved and what the composite shows changed everywhere; a
  **layer arriving** (`layer.add`, and the layer an answer lands on) records the arrival alone - the entry
  that takes the layer away carries the whole layer, so what it arrived wearing comes back with it, and a
  layer created *and then written* records that write beside it because the write has a `before` of its own;
  a **layer departing** (`layer.remove`, `layer.merge`) records the whole
  layer that went away, so undo returns its pixels, mask, mode, opacity and name rather than an empty layer
  wearing the right name; a flatten (`layer.flatten`) records the **whole stack** and applies it as a swap,
  so undo and redo are the same operation - the only encoding that needs no position for a layer, since a
  removal sees a stack shrinking while an insert needs one growing; changing the **marked coverage** records
  the coverage that is not current; changing **which plane a layer has** (`Kind::mask_replaced`) records
  its mask, `nullopt` included, so a mask put on is taken off by undo and a mask taken off comes back with
  the coverage it was hiding; and a **rename** (`layer.rename`, the field a card's title row turns into)
  records the one name that is not current, and is the only property of a layer worth a step of its own -
  typing a name should not cost the person the stroke under it, while hiding, fading or re-modding a layer
  is a state checked by looking rather than a mistake undone. Canvas resizing and cropping add a
  **whole-canvas replacement** that swaps the layer stack and marked coverage together. The replaced
  stack, marked coverage, mask plane, name and canvas entry all hold the state that is not current, so
  undo and redo can apply the same swap.
  A flatten carries every pixel of both sides and therefore records nothing else, and neither it nor a
  coverage entry names a layer, because the stack and the mark outlive any one layer. A coverage entry is not
  `structure` either — it puts coverage back and no pixel, so undoing one re-uploads no texture and resamples
  no thumbnail, and a rename entry is not either, for the same reason. What the document reads is therefore
  "no rectangle painted and no layer moved" - `!structure` with an empty `rect` - rather than a flag that
  would have to be kept in step with the kinds that paint nothing. A mask entry *is* `structure`, because
  what a mask hides is everywhere the layer composites, not only inside the mask's bounds.
  A scale (`layer.scale`, the field on a card behind its `Scale` button) is the one operation that rewrites
  a layer's pixels *and* its coverage as a single step: what it resamples is the picture -
  `Image::content_bounds`, not the layer's canvas-sized rectangle - about the middle of that picture, with a
  mask that covers something but not everything going through the same factor so the two keep covering the
  same part of the document. Chained scales are two resamples rather than an identity: doubled then halved,
  a hard-edged stroke came back in the box it started, flat inside and differing along a one-pixel rim
  (measured: 4411 px, worst 112 of 255, no byte changed anywhere else). A round trip of round factors is
  therefore not a way to lose nothing, and undo is what is exact. A layer that holds nothing, a factor that
  is not a finite positive number, and a factor that would push either plane off the canvas are all refused
  without a step, the last of those quoting the largest factor that does fit. A factor of exactly 1 changes
  nothing, records nothing and costs no revision.
* **Nothing is invalidated by layer id, and that is what recording the departure bought.** `History` has no
  `drop_layer` and no `drop_mask`: they existed because a layer or a mask that could only go forever made
  every step naming it unrestorable, and dropping the unrelated steps around it was the least bad answer.
  With every arrival, departure and plane change recorded, a step naming a layer that is not there *for the
  moment* is one undo away from being applicable rather than a lie - the paint on a deleted layer is
  reached by undoing the deletion first, and the layer comes back to be painted on. The fence that remains
  is the one nothing can reach by editing: `step_back` validates every entry against the stack it is about
  to walk, and a layer that went missing **without** a recorded departure drops the timeline whole rather
  than writing bytes into a layer that is not there (`test_a_history_that_no_longer_fits_its_layers_is_dropped`).
  Steps written into a mask before it went stay in the timeline for the same reason: they are reached by
  undoing the plane's step first, which returns the mask with the coverage the writes ended on.
* What a layer *is*, as opposed to what it holds or where it sits, enters no step but one: its name, and
  only because a mistyped one is a mistake a person should be able to take back without losing the stroke
  under it. Visibility, opacity and blend mode are composite state the stack carries, restored only as part
  of a layer that arrives, goes or is replaced. That is the whole of what is outside the timeline now.
* **The boundary of the coverage rule is therefore one gesture, not one kind of coverage.** Coverage
  written *into* a layer's mask is a pixel entry against that plane (`mask.fill`, `mask.invert`, a `cover`
  stroke, a marquee aimed at the mask); the plane arriving or going is the plane entry above; the mark on
  the document is the coverage entry. So *one gesture that changes coverage is one undo step* holds with
  no exception, and what stays outside the timeline is the layer's *properties* - visibility, opacity,
  mode - which the stack carries rather than the plane.
* Undo and redo are commands like any other (`edit.undo`, `edit.redo`, bound to `Ctrl+Z` and
  `Ctrl+Y` or `Ctrl+Shift+Z`), so a driver can put the pixels back and compare checksums without a keyboard.
  The host translates modified letters through the active keyboard layout, not the physical
  US key position (German QWERTZ maps Sokol's physical Y to Ctrl+Z).
* **(planned)** `history.jsonl` — append-only *journal*: every op with timestamp, params, prompt,
  model, seed, digests, timings. Two jobs: (a) the Provenance panel ("which prompt made this
  layer?" → one-click **Regenerate with same params**), (b) recovery/replay after a crash. Nothing is
  appended today; what a job remembered lived in the queue's memory and in `job.get`/`job.list`, and
  the request a person wants provenance about is still readable on the wire while the job is there.
  A journal is a file that outlives a run, so it inherits the question the queue was deleted for:
  say what identifies an entry, and what evicts it, before writing the store.

This is the strongest argument for our own container: provenance is not an afterthought,
it's a first-class file-format feature.

### Geometry steps and editable AI results (implemented 2026-10-29)

`Document::resize_canvas(Extent, offset_x, offset_y, mark_added)` defines the new canvas
extent and moves **all** layer pixels and layer masks in document pixel coordinates; it does
not resample them. The new exposure can replace the marked selection (the default) so Fill
uses only those pixels. `Document::crop_canvas(Rect)` is the corresponding negative-offset
extent change without adding a selection. `Document::rotate_canvas(bool)` turns every pixel
and coverage plane through 90 degrees with no resampling. Both run through `History::canvas_replaced`, which
swaps the layer stack and marked coverage together in one step. Ordinary paint/selection steps
from before a resize become applicable again after its undo. A single oversized geometry
step is retained even over the nominal history budget rather than making resize irreversible.
The canvas geometry revision also increments on undo/redo, so the application increments
`document_epoch` to reject AI answers queued against obsolete geometry.

`ai::compose_result` still supplies the old preview/flat image contract. The real landing
instead uses `ai::compose_editable_result`, which puts the entire colour-matched crop on the
new result layer and saves the effective coverage (feather and locked crop borders included)
in that layer's optional mask. The first display is unchanged; after landing, `mask.edge`
can threshold, dilate/erode or blur the mask, and ordinary mask brushes can hide/reveal areas
locally, independent of any still-marked area. `mask.edge.state` reads absolute size/feather values; `mask.edge.preview`/`finish`/`cancel` use the
existing provisional mask plane: each preview derives from the original mask and the final
release enters history exactly once. The mask operation and the provisional
history region are clipped to the union of the old and new coverage footprints, with a halo
that includes every box-blur pass. The cropped computation is byte-identical to running
it over the whole canvas, and avoids whole-canvas copies and reblends on each pixel click.
The panel has no separate Apply step. A result can only be revealed inside the crop actually returned by the model.
On transparent exposure, the selection coverage is not feathered away at the outer canvas
boundary: with no existing source alpha, there is nothing to blend into, so the selected
new pixels retain full coverage. The original-image seam can still feather normally.
`doc.outpaint.prepare` composes `doc.resize`, the existing Fill prompt and its intent; it
never queues generation without an explicit Run. Its default suggestion is `extended the background`. Resizing the **canvas** does not promise
to send the full new extent at native pixels to the model: the region policy may rescale
an AI request down to its published pixel budget.
See [CANVAS_OUTPAINT.md](CANVAS_OUTPAINT.md).

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

### What is written today: `format` 5

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
layout change rather than a widened name, so earlier numbers are refused. `format` 5
stores the active document version plus archived versions, each with its own native canvas
size, layer order, selection and masks. A version switch never resamples these planes.
Upscaling creates a new version with a single composite layer; the original stack is
not silently resized or hidden beneath the AI result.

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
| `manifest.json` | `format`, `size: [w,h]`, `layers: [{name, src, visible, opacity, mode, mask?}]`, `version: {id, name, active_layer}`, `archived_versions: [...]` and `selection: {src}` when there is one | `format` must be 5; `size` must agree with the PNG, and both numbers are named if they do not; `src`/`visible`/`opacity`/`mode` are required, `name` is not, because a file from elsewhere may not have named its layers |
| `layers/<n>/mask.m8` | a layer's mask plane, same shape, written only when that layer has one and named by that layer's row | read only when the row declares it: no row is no mask, and `covered == 0` is a mask that hides the layer, so the two must not collapse |
| `selection.m8` | the selection's coverage plane, one byte per pixel, canvas-sized, written only when the document has a selection | read only when the manifest declares it: **no row is no selection, and a declared plane that is missing or the wrong size is refused** — guessing which would turn a corrupt file into a silently unclamped document |
| `versions/<id>/layers/<n>/layer.png` | PNG for each layer in an inactive resolution version, independent of the active canvas | decoded at that version's declared dimensions, and rejected if they disagree |
| `versions/<id>/layers/<n>/mask.m8`, `versions/<id>/selection.m8` | mask/selection planes of archived versions, present only when declared | decoded at the version's dimensions, not those of the active canvas |
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

## The AI path

**Backend: [Lemonade Server](https://lemonade-server.ai), models on the `thenoise` recipe only.** TheNoise
is the image engine: an instruction-based generation/editing engine. Lemonade exposes it through an
OpenAI-compatible HTTP surface, and this app is a **client** - it does not host models. Typical deployment
is this editor on a laptop and Lemonade on a box on the LAN, localhost being the degenerate case.

Everything below is that contract and the two pieces of machinery that implement it: the region compiler
(intention → the rectangle that gets sent) and the result compositor (answer → the pixels that land). What
a person is doing while that runs is [PRODUCT.md](PRODUCT.md); how it is proven is
[TESTING.md](TESTING.md).

### Golden rule

> **The backend is a dumb ordered `images + words → image` function. All spatial precision is ours.**

Non-negotiable, and this is now a hard engine fact rather than a portability preference:

* TheNoise's `/edit` accepts `prompt`, `negative_prompt`, `width`, `height`, `steps`,
  `sigmas`, `guidance_scale`, `seed`, `sampler`, `image`, `kv_cache`, `ref_method`, and the
  post-processing knobs. **There is no `mask` field in the engine at all.** (Lemonade's
  public OpenAI table lists `mask` for generic recipes; on `recipe: thenoise` it can only
  be ignored or forwarded into a field the engine does not define.)
* Instruction-based editors drift the whole frame anyway. With local compositing we can
  guarantee *nothing outside the selection changed*, which is the property users actually
  want.

So every AI edit compiles the primary image through the same pipeline. Selected references follow it as
independent inputs and never alter the coverage or the landing plan:

```
bbox(mask) → grow for context → snap to size policy → crop from a flattened base
  → PREP (destroy target identity, preserve scene statistics)
  → Image 1: prepared primary, Image 2…N: explicitly selected references
  → prompt template → POST /v1/images/edits → resample back to the crop rect
  → out = orig*(1-m) + result*m   with m = feathered mask   [+ colour match, + lock band]
```

Cropping is the spatial instruction. `PREP` is the hint. The mask is **ours**, and never
leaves the client.

### Model policy: roles, never model names

No code, recipe, test or UI string references a model id. Recipes name a **role**; the
registry resolves role → exactly one concrete model, per server profile. Later the user
picks among a role's candidates along its quality/speed axis — which is why the axis is
already in the data.

#### Roles (MVP)

| Role | Capability predicate | Class of model it wants | Quality/speed axis | MVP |
|---|---|---|---|---|
| `generate.photo` | `recipe==thenoise` + `image` | photoreal, quality anchor | slow = more detail | ✓ |
| `generate.illustration` | `recipe==thenoise` + `image` | fast, stylised/anime-friendly | fast = iterate on prompts | ✓ |
| `edit` | `recipe==thenoise` + `image` + `edit` | instruction-based editing | a fast Klein ↔ a slower higher-fidelity Qwen checkpoint; the pair rotates as the box changes, which is why the order lives in `roles.json` and not here | ✓ |
| `upscale` | `recipe==thenoise` + `upscaling` | pixel-space upscaler | factor + fidelity | ✓ |
| `enhance` | `chat` + `vision` | prompt rewriting / scene description | cheap-and-fast ↔ capable | declared, **unused in MVP** |

#### Resolution order (per profile, recomputed on connect)

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
* **`recipe_options` per entry describes the model's settings** (`steps`, `cfg_scale`,
  `width`, `height`, `soft_pixel_cap`) for planning and estimates. The API accepts omitted
  `steps`/`cfg_scale` and applies its own defaults; the editor sends these only as explicit overrides.
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

#### Reference data (used to author `roles.json`; nothing in the code reads it)

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

### The Region Compiler

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
5. FIT       a crop inside the model's pixel ceiling is asked for at its **own** size — the upload,
             the `size` field and the answer describe the same pixels, so nothing is resampled at
             either end. Only above the ceiling does it scale down, uniformly by area; record
             `scale` *and* `requested` = that scale rounded **up** to the multiple, because the two
             are not proportional and the inverse map needs the size, not the scale.
             The ceiling is the model's `soft_pixel_cap` from `/v1/models` (2 MP on
             `Flux-2-Klein-9B-TheNoise`, measured 2026-10-23) and `roles.json`'s `max_area_px` only
             for a model that publishes none — a ceiling invented here shrinks answers the box would
             have served at full size, and a shrunk ask resampled back over a locked surround is the
             same picture, softer: a Change that visibly did nothing. When it happens the plan says
             so (`region.resampled`, in the preview, the landing and the log)
6. CROP      flattened base = visible layers below ∪ active layer (`Document::flatten_over`),
             alpha flattened over a colour sampled from the mask ring (never over plain
             white/black) — `flatten_over_ring(crop, coverage_in_crop)`
7. PREP      a *hole*, not a treatment of the region. Only pixels the coverage reaches are
             altered; every other byte of the crop is uploaded exactly as the document holds it,
             because the surroundings are the instruction. Modes: none | average | blur(gauss,
             r = max(kMinBlurRadiusPx, crop short side / 12)) | pixelate (block = that radius) |
             noise (seeded, so the bytes are reproducible) | white. Default blur: it destroys *what*
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
9. WRITE     PNG encode (these exact bytes are the upload; `blob` is their hash)
```

A document-scope recipe with an aspect chosen enters the same pipeline through step 1: the coverage it
hands over is `ai::frame_for_aspect(canvas, ratio)`, the largest centred rectangle of that ratio. Nothing
downstream differs, which is the point - a frame is coverage, not a second kind of request, and the grid
snap in step 4 belongs to this compiler rather than to the frame (aligning the frame itself to 16 px costs
23 % of a 1000×1000 canvas's 16:9 area to save a snap that happens anyway).

### Field policy

Tiers exist so nothing accidental creeps in. The builder is the only place that can add a
field, and it refuses tier-2 unless the model profile opts in.

**The builder also validates its own values, because nobody downstream does.** `seed:
"not-a-number"` is answered with `200` and a default (measured). A misspelt field name is
therefore silent rather than loud, and the one place that can catch it is here. The seed's
*range* is caught here too, and the two endpoints disagree about it (measured 2026-10-25):
`/images/generations`, a JSON body, answers `200` for `0`, `2147483647`, `2148483648` and
`4294967295`; `/images/edits`, a multipart form, answers `200` for `2147483647` and `400
Invalid value for 'seed': must be an integer` for `3000000000` — a range error wearing a type
error's clothes, in a form-field parser that reads a signed 32-bit integer. The form therefore
sets `ai::kMaxSeed`, since one field carries one limit; the box publishes no schema to read it
from (`/ui-schema` does not exist, `/models` carries no seed bounds). `seed: 0` is accepted by
both, so 0 staying unused is our convention for "no seed yet", not an engine rule.

**Tier 1 — OpenAI-level, used in MVP**

| Endpoint | Content type | Fields |
|---|---|---|
| `POST /v1/images/generations` | JSON | `model`, `prompt`, `size`, `steps`, `cfg_scale`, `seed`, `response_format="b64_json"` |
| `POST /v1/images/edits` | `multipart/form-data` | `model`, one `image` PNG part for a single-image request; for two or more images, one repeated `image[]` PNG part per image, in order; `prompt`, `size`, `steps`, `cfg_scale`, `seed`, `response_format="b64_json"` |
| `POST /v1/images/upscale` | JSON | `image` (b64 PNG), `model`, optional `upscale_factor` (1..model native scale, requires updated Lemonade adapter) |
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
| `upscale`, `upscale_factor` (generation/edit), `upscale_type`, `pixel_upscaler` | in-generate 2× latent refine, or pixel upscaler | "generate big" without a second round trip |
| `film_grain` (0–10), `sharpening` (RCAS 0–1), `qwen_vae_enhance` | post-processing | **`film_grain` is a seam-hider**: grain on the generated patch masks the composite boundary; `qwen_vae_enhance` kills 2 px grid artifacts from Qwen VAEs |
| `lora_specs` | `["style:0.8"]` | style packs — server-side files, so a settings feature, not a per-request one |

Tier 2 is **not implemented**, and there is deliberately no opt-in path for it: `FieldSink` is the only
code that can put a field on a request, it checks every name against the endpoint's tier-1 row, and
`ai.run` refuses a field the table does not list. Adding tier 2 therefore means adding a row to that
table and a way for a recipe to ask for it — which is the point: a knob that works cannot be reached
by accident. When it is added, the gate is intended to be per-server rather than per-model, and a
rejected field dropped with the deduction recorded rather than the job failed.

For `/images/edits`, one image keeps the existing `image` part. With two or more, every image including
the primary uses a repeated `image[]` part; `image` and `image[]` are never mixed. The primary is always
Image 1: the current document composite prepared for the recipe's editing region. Additional references
are appended in the order shown in the References section. They are independent images, never a flattened
substitute for the document and never an instruction to widen local coverage. No reference-count limit is
assumed from model labels; a backend refusal is reported with its response and the request is not retried
with fewer images.

Each recipe's `include_references_default` supplies the starting choice: Change and Fill enable it;
Blend, Heal and Remove leave references configured but out of the request until explicitly enabled in
the References section. `ai.intent`, `ai.preview` and `ai.run` share this choice, and the preview reports
both the configured count and the number actually transmitted. Switching recipes restores the selected
recipe's default unless that operation is explicitly changed again.

The request preview lists each image's order, source name, dimensions, PNG byte count, SHA-256, content
type and multipart filename, along with the actual image field names and total body size. The request
identity hashes every image's bytes in order, so reordering references changes the body boundary and the
recording match. Image payloads and base64 are never written to request diagnostics.

`AiSession` owns these references for the current app run; they are not saved in `.noise` or mixed into
document pixels. File references are decoded from PNG/JPEG/WebP and encoded once as lossless PNG at their
decoded dimensions. A selected layer is isolated from other layers, cropped to pixels with nonzero
effective alpha, and carries its pixel alpha, mask and opacity. Transparent RGB hidden by alpha or mask is
zeroed. Layer references refresh only when that layer's content revision or opacity changes. If the layer
is temporarily absent in the same document, its stable id and cached image are retained so undo can restore
the link; switching documents detaches the reference before another document's ids are considered. A queued
request already owns its image bytes, so a later reorder, removal or layer edit cannot change what it sends.

#### `n`: we do not use it

The adapter implements `n` as a **sequential loop inside one request**, for generations too
(the public doc only promises it for edits). One response body containing N base64 PNGs
means waiting for the *last* image. Our queue issues N separate jobs instead, so variants
appear in the tray as they land and one failure does not take the batch with it.

#### Variations endpoint: not available

`/v1/images/variations` returns `UnsupportedOperationException` on the `thenoise` backend.
Removed from the contract, from the recipes, and from the UI. "Give me variations" = N jobs
with `seed = base + i` against the normal endpoint.

### Server session & model switching (the constraint that shapes the UX)

TheNoise's **image pool can hold one model at a time** on the measured box:
`max_models.image == 1`. Other model types can be loaded concurrently, so `model_loaded`
(the most recently used model, of any type) is not the image resident. The app reads
`all_models_loaded[]` with each model's `type` to determine whether the selected image model
is already present or would evict another image model. The
image checkpoints a box of this class holds are 17–35 GB, the same size class as its memory budget, so a
swap is a real event: the first request that followed an unloaded model on the dev box cost
32.0 s for a 512² image. Tens of seconds, sometimes minutes, on a LAN box. Therefore:

* The app has a **session model** concept, available through `server.state` and `ai.intent`. The UI
  status line shows the active verb, quoted size and estimate; a model name appears when a press would
  switch the server, and the line reports job progress, cancellation, landing and errors.
* Recipes declare a preferred *role* (`generate` / `edit`), not a model. The picker resolves
  role → user's chosen model for that role.
* **Swap is explicit and priced.** When a command's role resolves to a model that isn't
  loaded, the confirm dialog says "this switches the server model (~Xs)" and offers to pin.
* We call `POST /v1/load {model_name, pinned:true}` when the user pins, and
  `GET /v1/health` (`model_loaded`, `all_models_loaded[] {model_name, type, recipe,
  device, pinned, last_use}`) to keep the session state honest. Never auto-swap silently.
* **Warm-up** (a 256×256/1-step request right after a swap, so the first real generation is not the
  one that pays model init): designed, not built. What is built is `server.load {model, pin?}`, so the
  swap is at least something the user chooses a moment for.
* **Queue concurrency 1 per server**, always.
* `GET /v1/stats` is token-oriented, not image-oriented ⇒ we measure latency **client-side**
  per `(model, w×h, steps)` and keep an EMA for ETA display. **A request the server has already
  answered is served from its own cache in ~0.1 s**, so a replay must not feed the EMA — an ETA
  drifting toward the cache's latency would make every job look instant.

### Transport, auth, multi-server

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
  timeout_request   max(600 s, 4 × wait), where the wait is this box's measured time for the exact shape
                    or, when the shape is new, its measured time for the model scaled by pixels × steps
                    ← local diffusion is slow by design, and a compiled region is a shape that has never
                    been run before: a masked edit at the pixel ceiling measured 71.1 s on the dev box,
                    which a 60 s floor reported as a dead server
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

### The Result Compositor

Always runs; this is what makes instruction editing usable at all. Built as `ai/compose` +
`ai/colour_match`, over `core/resample` and `core/blur`; asserted by `test_compose`.

**An option nobody passes is not an option that was declined.** `feather`, `colour_match` and `lock_band`
reach the compositor through `CompositePolicy`, whose values come from the recipe, and the wire
argument helpers leave the caller's variable alone when the key is absent. That sounds like a detail and
was a live defect: the boolean helper used to write `false` for an absent key, so `job.apply` and
`ai.compose` ran with the colour correction switched off while the recipe and the report both said it
was on. The unit tests had been green throughout, because their fixtures are flat fields — see the lesson
in `PLAN.md` about a flat fixture being a degenerate input for a colour operation.

#### The colour match is a drift measurement, not a palette

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
blended onto — an empty one, since rule 1 of [PRODUCT.md](PRODUCT.md) — and `reference` is the document the
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

### Draft → Refine

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

* **No answer cache** (see "The queue lives in memory" below): a re-ask is a second request. The one
  fact that survives from the old design is that a *server-side* fast reply is not a model run — a reply
  inside one second is counted as the box's own answer and never feeds the latency EMA, because an ETA
  drifting toward 0.1 s would promise waits that never happen.
* **Latency EMA** per `(model, w×h, steps)` from client-measured timings → ETA before you
  commit, and it drives `timeout_request`. The engine's published benchmarks are the
  starting point, not the runtime source: every box differs.
* **The queue lives in memory**, one worker per server. `<runtime>/ai/jobs/<id>-{coverage,request,
  result}.png` are scratch for the run that wrote them and **nothing in the runtime directory is read
  back**; startup deletes the previous run's `ai/` and `captures/`. A job is a fact about a socket, not
  about a document — the document is the only thing that persists an answer, by being saved. Crash
  mid-request and the ask is gone; press the verb again.
* **No answer cache, and no `job.retry`.** A cache keyed on `(server, model, prompt, size, steps, cfg,
  seed, upload-hash)` hits only on a byte-identical re-ask, which is an artefact of probing rather than
  of editing: nudge the selection and the upload hash moves. It had no bound and no cleanup, so it grew
  a megabyte per edit forever — 13 MB against 2 jobs in one captured runtime directory. Pressing the
  verb again *is* the retry: it recompiles the region against what the document is now and sends a
  second request, which costs a second image. That is the honest price of an edit, and `test_backend`
  asserts a re-ask is one send more rather than a silent shortcut.
* **Cancel**: the transport polls a flag the job owns (`curl_handle_abort` in flight, an erase from the
  queue while still queued), and cancelling never discards an answer that already arrived — a landed job
  answers `already succeeded; its result stays at <path>` instead of pretending to take it back.

### Prompt Recipes

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

#### Two kinds of operation, and the axis `role` does not cover

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

Four keys belong to the recipe because the interface is a consumer of the table, not of a
hard-coded palette:

* **`label`** is what the control says (`"Remove"`, `"Replace"`). The `id` is a key - greppable,
  quoted in errors, never a word for a human; a button wearing an id is a control nobody wrote.
* **`ask`** names the variables a person is asked to fill, in order (`["target"]`,
  `["target", "replacement"]`). Everything not listed runs on its `variables` default, which is what
  makes a headless `ai.run` and a click compile the *same* prompt - and what keeps `context_desc`
  unwritten until the `enhance` role is switched on to fill it. An `ask` name with no declared default
  is refused at parse: it would be collected from the user and then rejected by the prompt builder,
  which is the worst place in the pipeline for that to surface.
* **`aspects`** lists the frames (`["1:1", "16:9"]`) a person may pick for a recipe whose operation is
  `from_words` and whose region is `document` - the only kind with no source geometry to follow.
  Elsewhere the parser refuses the key: a preset overriding pixels that already exist is a lie about
  the region. A ratio is geometry and never a request field - it picks the rectangle
  (`ai::frame_for_aspect`, `ai/region.h`) that the compiler above then sizes by `size_policy`, and the
  answer is inverse-mapped back onto that rectangle, so the ratio chosen is the ratio that lands.
  Empty means the whole canvas, which is every recipe that is not a generation. The frame is also the
  coverage, and the generate recipes set `feather: none`: `compose` softens a boundary in both
  directions, so an `auto` feather walks the answer past an edge nothing uploaded - which is also why
  `Landing` keeps the mask bounds next to the crop the request was sized from.
* **`variants`** lists recipe ids that stand in for this one at the same key and the same panel row: a
  different `role` and prompt tail, nothing else. Style is therefore recipe data rather than a branch
  in a verb - `new-image` offers `new-image-photo` and `new-image-anime`, and the panel shows the three
  as one "style" dropdown. `load_recipes` refuses a family that is not interchangeable (an
  unresolvable id, a variant with variants of its own, one claimed by two heads, or any difference in
  `operation`, `endpoint`, `ask` or `aspects`), because a stand-in that grew a question would be run by
  a key that never asks it. `RecipeSet::family` and `family_head` are what the surface reads, so a row
  still offers the whole family after a style has been chosen.

Three scopes exist for a region's coverage: `selection` (the marked area), `document` (the frame) and
`layer` (the active layer's own alpha, byte for byte, through `coverage_of` - no threshold, so a soft edge
stays soft and the coverage a paste already has is what gets answered). `ai.verbs` reports which one a row
has through `region_scope_name`, the one place a scope becomes a word, and the panel's intent list is
`selection_intents()` plus `layer_intents()` - the second offered whether or not anything is marked.

The palette of selection operations is therefore `recipes/*.json` filtered to `region: selection` with
an `edits` endpoint (`RecipeSet::selection_intents`): adding an operation means adding a
file, and a recipe that stops being a selection operation leaves the palette by itself.

Variables come from the user's instruction (whatever the recipe's `ask` lists) and the layer name. A
chosen `aspect` is not one of them: it never enters the words, only the size the request asks for.

**Every request takes a seed of its own.** The model explores from the seed, so two asks over the same
pixels with the same seed are the *same request twice* — a second charge for the picture already on the
canvas, and a verb that cannot improve. `ai.preview` and `ai.run` take `seed` optionally; a call that
names none takes the panel's pin (`ai.intent {seed: n}` pins, `{seed: null}` lets go) or a draw from
`AiSession::seed_draw`, which is `platform::random_seed` and nothing else. Reproducibility comes from
*recording*, not from a constant: the number and its source (`named` / `pinned` / `random`) travel with
the compile and the job record, so "what made this" is answered afterwards.

`seed` and `seed_pin` are different arguments because they are different claims. `seed` belongs to the
request the call builds — `ai.change {seed: n}`, `ai.run {seed: n}`, `ai.intent {seed: n, run: true}` all
carry *that* request and nothing after it. `seed_pin` is the hold: `ai.intent {seed_pin: n}` makes every
later ask take that number (how someone iterates on an answer they liked), `{seed_pin: null}` lets go.
They were one argument for an hour, and `ai_job_probe` caught what that meant: a verb press carrying a
seed quietly held it for every press that followed, which is the original defect wearing the fix's clothes.
The panel shows the seed of the ask that produced the current answer, labelled pinned or random — never a
number the next press promises to reuse — and reading the intent draws nothing, because a rule that
forbids a per-frame read from computing an answer forbids it spending entropy too. Variants stay the
explicit case: `seed + i`, asked for by name. The
`enhance` **role** (one `chat/completions` call on a downscaled crop — *"describe the
background surface, lighting and palette in two lines"*) is designed in but switched off in
every MVP recipe: it should buy better lighting/perspective matching for ~1 s of an LLM call
instead of minutes of re-generation, and it is a later tuning step, not a dependency. A
recipe that declares `enhance` must produce a valid request when the role is unresolved.

#### MVP recipes (pre-shipped)

Every row obeys rule 1: the answer arrives on its own layer above the one the region was flattened from,
which is where a strength control lives when one is wanted — the layer's opacity is the blend slider, and
fidelity is not a server knob.

| id | endpoint | role | operation | hole | region | notes |
|---|---|---|---|---|---|---|
| `new-image` | generations | generate | from_words | none | full doc | the head of its family: `aspects` 1:1, 3:4, 4:3, 16:9, `variants` the two rows below |
| `new-image-anime` | generations | generate | from_words | none | full doc | stands in for `new-image` at its key and row: illustration tail, `generate.illustration` |
| `new-image-photo` | generations | generate | from_words | none | full doc | stands in for `new-image` at its key and row: photographic tail, `generate.photo` |
| `inpaint-remove` | edits | edit | reconstruct | blur | mask bbox | the flagship: the only operation whose blur is a feature |
| `inpaint-replace` | edits | edit | in_place | none | mask bbox | one field of words from the person (`instruction`): "Change this picture: …" |
| `blend` | edits | edit | in_place | none | **layer alpha** | the pasted thing painted into its scene, with no words asked: the coverage is the active layer's own alpha and the upload the document flattened up to and including it, so the model is shown the scene the object has to fit. Nothing is destroyed in the upload, because destroying it removes the thing to keep |
| `region-fill` | edits | edit | reconstruct | average | mask bbox | generate into the region from one field of words (`prompt`); the fill takes the surrounding colour, never the hole's own |
| `heal` | edits | edit | reconstruct | average | selection | the in-process Telea/Chan path for a box with no edit model is designed and **not built**, so `heal` refuses when the edit role is unresolved |
| `flatten-colorize` | edits | edit | in_place | none | full doc | e.g. "colorize this line art" — the highest-value raster trick for illustration workflows |
| `reimagine` | edits | edit | in_place | none | full doc | full-frame edit. The whole frame is the region, so there is no hole and nothing to blend at the edge; its answer lands on a layer like any other, and 50 % opacity is a number the user owns |
| `upscale` | upscale | upscale | transform | none | full document | ESRGAN model choice; `upscale_factor` requests 1..native 2x/4x output, changing document size and adding a top layer over hidden scaled originals |
| `free-command` | user's choice | user's choice | user's choice | user's choice | user's choice | escape hatch below |

(`variations` deleted — unsupported endpoint.)

**Answers land themselves.** A succeeded job's answer is composited onto the document as soon as a door
reads the queue (`ai.intent`, `job.list`, `job.get`, `job.wait` all call `pump_ai_landings`), onto its own
new layer above the layer the region was flattened from, named for the recipe's label and the job. Three
facts make that safe and they are all in the job's record: `document_epoch` (a job may land only in the
document that asked for it - a job that came back after the person opened another file stays out of the
new one), `landed` + `landed_layer` (so reopening the app does not stack a second copy, and so `job.apply`
can tell "already there" from "undo took it away"), and `report` (the compose report of that landing, so
the pixel-lock claim is checkable after the fact whoever asked). A landing that refuses is recorded once
with its sentence rather than retried every frame. **Undo is respected**: the beat never re-lands what
undo removed; `job.apply` is the door back.

**`ai.preview` is the only thing that preps, resamples and encodes an upload besides a run**, and both are
on being asked. Nothing on the frame path does: the window's price line is a `RequestQuote` (endpoint, the
size the region compiler settled on, the model, its default steps, the box's measured time), which is
geometry and table lookups. `AppState::upload_encodes` counts the encodes and `ai.intent` reports it, so a
regression that puts an encode back on a read is a red test rather than a slow window. The reason to keep
`ai.preview` at all is that the *upload* is a claim about the document - `tools/prep_proof.py` renders it
for every recipe, and that is how a dark patch and a blurred subject were caught - not that anyone can
preview an answer before the model makes one.

#### What a recipe may ask for

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

**Built today.** `assets/roles.json` + `assets/recipes/*.json` (11 recipes; `free-command` is the one
row of the table not shipped), loaded by `ai/registry` and `ai/recipes` from `$NOISE_EDIT_ASSETS`, the
installed asset directory relative to the executable, `assets` in the working directory, or the runtime
directory — in that order, one lookup, reported by `server.state` as `tables`. `ai.recipes` lists each
recipe with the policy it states and the model its role resolved to, so
"a recipe that behaves oddly" is answerable from the wire. A recipe file whose name and `id` disagree is a
refusal, not two ids for one policy.

#### `free-command`

Raw prompt + model + params, plus **"show exact request"**: the multipart field list, the
resolved `size`, and the actual prepped PNG that will be uploaded. Trust, debuggability, and
our own recipe-tuning workflow all depend on this panel existing early.

### Failure handling

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
| state | `app.state`, `command.list`, `wire.info`, `doc.get`, `layer.list`, `select.get`, `ai.references`, `server.state`, `job.list`, `job.get {id}`, `ai.roles`, `ai.recipes`, `log.tail` | read what the app believes, cheaply |
| drive | `select.rect {rect, mode?}`, `select.ellipse {…}`, `select.lasso {points, mode?}`, `select.all`, `select.clear`, `select.invert`, `select.from_mask`, `select.get`, `mask.from_selection`, `command.invoke`, `app.quit`, `ui.theme {theme?}`, `ui.layer_split {upper_ratio?}`, `ui.ai_workspace {open?, focus?}`, `ui.image_size` (`Ctrl+Alt+I`), `ui.canvas_size` (`Ctrl+Alt+C`), `ui.crop`, `ui.upscale` (`Ctrl+Alt+U`), `doc.new/open/save/export {path, quality?}/get`, `doc.resize {width, height, offset_x?, offset_y?, mark_added?}`, `doc.outpaint.prepare {width, height, offset_x?, offset_y?, prompt?}`, `doc.crop {rect?}`, `doc.rotate {direction: cw|ccw}`, `image.get`, `log.level`, `view.fit/set/pan/zoom`, `edit.undo/redo`, `tool.stroke {points, tool?, size?, hardness?, opacity?, flow?, spacing?, color?}`, `tool.pointer {phase, at?, mode?}`, `tool.set {tool?, size?, hardness?, opacity?, flow?, spacing?, color?}`, `tool.<name>`, `brush.larger/smaller`, `layer.list/add/remove/select/set {index?, visible?, opacity?, mode?}`, `layer.move {index?, to}`, `layer.merge {index?}`, `layer.flatten`, `layer.rename {index?, name}`, `layer.rename.begin {index?}` (`F2`), `layer.scale {index?, factor}`, `layer.import {path, at}`, `mask.from_selection/add/remove/fill {to}/invert/get`, `layer.plane {index?, plane?}`, `view.mask_overlay {show?}`, `mask.edge {shift?, hard?, feather?}`, `mask.edge.state`, `mask.edge.preview {shift?, feather?}`, `mask.edge.finish`, `mask.edge.cancel`, `brush.larger/smaller`, `brush.hardness_up/down`, `brush.opacity_up/down`, `screenshot`, `pixels.compare`, `wire.info` |
 `tool.stroke` drives the *real* stroke path, so painting is testable without a mouse; its brush fields are declared and every one is optional, inheriting what the app is on, so a recorded stroke that sends them replays without knowing what the UI had selected. The fields that belong to `tool.set` are not accepted here and vice versa, and both say so. `tool.pointer` is the same path one event at a time - the UI drives the pointer through this command, so a gesture can be recorded and replayed. Its `cancel` phase closes a provisional selection without committing it; Lasso press may also carry `mode: replace|add|subtract`. `tool.<name>` is generated per row of `core::kTools`, so a tool exists in exactly one table |
| pixels | `image.get {region, base64}`, `screenshot {region, name, base64}`, `pixels.compare {a, b}` | screenshots exist so a vision model can look; `pixels.compare` is the pixel-lock invariant made checkable. It decodes either path through the flat-format table, so a golden may be a PNG, JPEG or WebP — what is compared is pixels, not a container |
| layer editing | `layer.plane {index?,plane?}` (select image/mask thumbnail), `view.mask_overlay {show?}` (red hidden-area preview), `brush.edit_plane`, `stroke.plane` and `layer_cards[*].editing_mask/editing_pixels` (wire-observable targets) | Selection tools always edit the marked area; Brush/Eraser use the thumbnail. The result plane does not change on landing. |
| ai | `verb.bar {}` (mode `actions` or `generating`; no bar until a new selection after landing), `verb.ask {show}`, `ai.verbs` (the verb rows: id, command, key, recipe, label, operation, `ask`, scope, `ready`, `reason`, `model`, `resident`, `swap`), `ai.heal` / `ai.remove` / `ai.fill` / `ai.change` / `ai.generate` / `ai.blend` `{vars?, seed?, confirm?, include_references?}` (one verb row pressed - they call `ai.intent` with their recipe named, so a key and a scripted intent cannot diverge), `ai.references {}`, `ai.references.add {path?, paths?, layer?}`, `ai.references.remove {id}`, `ai.references.move {id, to}`, `ai.compose {image, apply?, feather?, colour_match?, lock_band?}`, `ai.preview {recipe, vars?, seed?, steps?, cfg_scale?, include_references?}` (compiled request + prepped PNG, **no send**), `ai.run {recipe, vars?, seed?, steps?, cfg_scale?, include_references?}`, `ai.intent {recipe?, vars?, aspect?, include_references?, seed?, seed_pin?, job?, run?, confirm?, connect?}` (the selection panel: intents, values, price, job and session - read with no arguments, which costs nothing), `server.connect`, `server.load {model, pinned}`, `server.state`, `ai.roles`, `ai.recipes`, `job.list`, `job.get {id}`, `job.wait {id, timeout?}`, `job.cancel {id}`, `job.apply {id, feather?, colour_match?}` (the idempotent landing door - answers land by themselves, so this one brings one back or overrides the policy, and says `already_landed` otherwise), | `ai.references` reads ordered session references; its add/remove/move commands are the same actions the GUI uses, and `ai.preview` shows every image's order, source, dimensions, hash and encoded size plus the actual request field names and body size. It also reports whether configured references are included for the selected operation. `ai.compose` lands a returned image on the document and reports the effective coverage, so pixel-lock is checkable without a server; `ai.preview` shows the exact request before anything is sent; `server.*` and `job.*` are the session and the queue, so a driver can connect, run, wait, cancel and land without a UI; `ai.intent` is the whole surface of the action, so a scripted run and a key press cannot mean different things - including the handshake, which is `connect` here and a button in the window, never a line of UI code that owns a socket. `seed` is optional on every request-building command; absent, the request takes the panel's pin or a fresh draw, and every report says which (`seed`, `seed_source`) |

Live now: every row. The AI row is driven end to end by `tools/ai_job_probe.py` against
`tests/fixtures/http` (a recorded server), so the whole path - discovery, role resolution, the compiled
request, the queue, the landing, the pixels the report claims - is checked in the default `ctest`
without a socket. `--live` runs the same probe against the dev box and asserts only what holds whatever
model answers; it is registered under the `live` label and skips without `$THENOISE_URL`.
`app.state` reports `ui_theme` (`dark` or `light`), `layer_split.upper_ratio` (the Layers dock's
upper-region fraction, also read and set by `ui.layer_split`) and `ai_workspace` (open,
visible, docked and screen window bounds, controlled through `ui.ai_workspace`), `frame` (frames presented),
`frame_ms` (last presented frame), `canvas`
(origin + size of the canvas content area in framebuffer px) and the view transform
(`scale`, `origin`, `image`, `fit_pending`), the brush and tool the last stroke ran with, the
document's `history` depths, and its `selection` (`active`, `bounds`, `covered`, `area`, `canvas`,
with `canvas` present only while one exists, because a sized mask that covers nothing is a real state
and must not read as no selection). `covered` counts pixels touched, `area` adds the coverage bytes
up: for a rectangle they are equal, for an anti-aliased curve `area` measures partial coverage and
`covered` is the pixels it grazes. While a pointer gesture is down, `stroke` also reports `marquee` —
the box a rectangle or ellipse tool is drawing, live, `marquee_shape`, which gesture is active, `lasso_points` in document coordinates for Lasso, `selection_mode`, and `target`,
which is `selection` for a marquee or Cover and a layer plane for Brush/Eraser: the only way to tell a marquee that tracks the pointer from one that
appears on release, and the only way to see which selection the gesture will commit on release. The mask is reported the same way a selection is - `layer.list`
rows carry `mask` (`covered`, `area`, `bounds`, or `null` for none), `mask.get` answers with the mask
itself, and `brush.edit_plane` names whether Brush/Eraser write pixels or the mask - so a
driver can tell a mask that hides everything from a layer that has no mask, and a stroke that painted
colour from one that painted coverage. The Layers panel reports where it is as well as what it shows:
`layer_cards` rows carry a layer's `index`, `id`, `name`, whether it is `active`, `renaming` or `scaling`
while the card holds the name field or the factor field, and the screen box of every control on the card
(`title`, `visibility`, `picture`, `mask_picture`, `editing_pixels`, `editing_mask`,
`move_up`, `move_down`, `merge`, `opacity`, `mode`, `scale`), while `layer_panel`
carries the boxes of the stack's own row, the `can_flatten` that says whether the row can do anything to
this stack, and the box of the scrolling list the cards live in. A driver presses a control where the app says it is, which is the only way to catch the
failure this found: a widget submitted inside another widget's rect draws, lights up under the pointer and
never fires, so a dead control looks exactly like a button nobody clicked. Enough for a driver to place an image rect on screen, wait
for the frame that drew it, and tell a stroke from an undo. `doc.get` is the call that hashes
the pixels and answers with a checksum; `app.state` deliberately does not, because a driver
polls it every frame. `fit_pending` is true while a fit waits for the docked canvas area to keep the
same size across frames and be wide enough to hold the document: the framebuffer is not the canvas,
and the first DockBuilder frame can still have the pre-split width, so fitting against it latches a
scale that spills past the panel.
It also reports the References section's screen rectangle for file drops and the cached thumbnail rebuild
count; `ai.references` reports the session-only source list, order, dimensions, cache revisions and errors.
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

## Decisions (locked)

| # | Question | Status |
|---|---|---|
| **D1** | Platform layer | **sokol_app** — no pen/tablet in the MVP. SDL3 behind the same shim if that ever changes. |
| **D2** | Colour/bit depth | RGBA8 straight alpha, sRGB-ish, no colour management in MVP. `PixelFormat` stays in `ImageBuffer` so 16-bit is a migration, not a rewrite. |
| **D3** | Canvas ceiling | 8192×8192, warn >4096², tile only when a real document needs it. |
| **D4** | Alpha of the flattened base sent to AI | flatten over a colour sampled from the mask ring, never over white/black. |
| **D5** | One image per document (no artboards) | yes |
| **D6** | Undo across AI ops | always. Today it is free because the answer arrived as a **layer** and one undo step removes that layer and its pixels together; the blob-pointer swap this row originally described is the planned form, for edits to an existing layer's whole payload |
| **D7** | Single-file vs directory project | single `.noise` zip. `--unpack/--repack` dev flags for git-friendliness are **planned**, not built: nothing reads a loose project tree today |
| **D8** | Licence | **Apache-2.0**. One SPDX line per source file. GPL codec libs stay out. |
| **D9** | Content scope | raster layers only — no text/vector/adjustment layers in the MVP. |
| **D10** | Recipes | pre-shipped, read-only JSON in the app resource dir — the format the manifest and the wire already speak, so no second parser. No in-app authoring. |
| **D11** | Backend scope | Lemonade only, `thenoise` recipe only; N server profiles (LAN/localhost), always keyed. |
| **D12** | Naming | extension `.noise`, binary `noise-edit`, config dir `~/.config/noise-edit`, MIME `application/x-noise+zip`. |
| **D13** | Model references | roles only (`generate.photo`, `generate.illustration`, `edit`, `upscale`, `enhance`); one model per role per profile; candidates ranked by quality/speed in `roles.json`. |
| **D14** | Observability | debug wire from M0, dispatched through the UI command table; every feature ships its wire command. |
| **D15** | Ground rules | `AGENTS.md` is normative and overrides prose elsewhere in this repo. |

### From the original scope document

| | Decision |
|---|---|
| Platforms | cross-platform **by design**; Linux build/CI now. The unwritten ports are `NSOpenPanel` (macOS) and `GetOpenFileNameW` (Windows), one file each, to be written on a machine that can test them |
| Input | mouse/keyboard only, **no pen/tablet** in MVP → sokol_app |
| Backend | **Lemonade only**, `recipe: thenoise` only; API keys required; server often remote on LAN; no consent badge (server is user-controlled) |
| Content model | **raster only** |
| Canvas | 8192×8192 ceiling, ~512 MB budget |
| Recipes | pre-shipped, read-only; JSON, the same reader as the manifest — no second format |
| Naming | extension **`.noise`**, binary `noise-edit`, MIME `application/x-noise+zip` |
| Flat formats | **PNG, JPEG and WebP in and out**; read by content, written by extension. A format with no alpha channel composites the export against **white** and reports `discarded_alpha` — it does not refuse, and it does not pretend the transparency survived |
| Interchange | **no `.ora`/OpenRaster import or export, in the MVP or later.** `.noise` is ours; what leaves the app is a flat PNG/WebP/JPEG. A second layer-stack semantics kept true for another application's file costs more than the interop buys, and provenance has nowhere to go in it |
| Mask | never sent to the server; applied locally, always |
| Variations | endpoint not used (unsupported) |
| Model references | **roles, never model ids** (`generate.photo`, `generate.illustration`, `edit`, `upscale`, `enhance`); one model per role per profile; `roles.json` ranks candidates by quality/speed |
| Prompt enhancement | the `enhance` role exists but is **off in every MVP recipe**; later tuning step, never a dependency |
| Licence | **Apache-2.0**, one SPDX line per source file |
| Observability | **debug wire from M0**, dispatched through the UI command table; every feature ships its wire command |
| Testing | four tiers (unit · golden image · recorded backend · `live`); the dev server is used for real when `THENOISE_URL`/`THENOISE_API_KEY` are set |
| Ground rules | **`AGENTS.md` is normative** and outranks the prose in these docs |
