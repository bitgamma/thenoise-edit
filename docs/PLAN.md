# Plan — where M0 ended and what comes next

Working note for the next session. Facts below were observed, not assumed; re-check any
that matter before building on them. Refresh or delete this file as milestones close.

## Handoff for the next context (2026-10-09, written mid-session)

Read `AGENTS.md`, then this section, then `docs/MVP.md` M1/M2 rows. Everything below was
observed, not assumed; the numbers are from one dev box and rotate with it.

### Machine rules for this repo (already true, keep them true)

- Build/gate: `cmake -B build -G Ninja && cmake --build build && xvfb-run -a ctest --test-dir build`
  → currently **16/16 green**. Binary is `build/noise-edit`.
- Commits need `--no-gpg-sign` (`commit.gpgsign=true`, no signing key here).
- **Never write a server URL, hostname or key into the repo** - not in docs, probes, comments or
  commit messages. The dev pool rotates by occupancy, so reference `$THENOISE_URL` /
  `$THENOISE_API_KEY` and say "the dev box". Two example LAN addresses are still in the docs and
  need genericising (see To do #1).
- Live tier runs only with those env vars set; `tools/tn_probe.py` skips loudly otherwise, and it
  caps requests with `--max-images`. Ask before spending more than ~10 images.

### Decisions taken by the human this session

1. **`.noise` format: always bump.** Adding required or optional manifest fields bumps
   `manifest.json:format` and refuses older files naming the version. Do not add optional fields
   under an old version number. (Needed for layer `visible` / `opacity` / `mode` / mask refs.)
2. **Live server is available**; use the **`Flux-2-Klein-4B-TheNoise`** recipe for now (lightest).
   "If you have ANY doubt on how to use the API or if something doesn't work, ping me."
   The box had a huge LLM resident during probing, so first-request load costs measured today are
   outliers, not the normal case.
3. **Write a short user guide** - explicit request: "even I don't know how to use it. I saw the
   layers and the brushes." Nobody has asked for anything else in the UI yet.

### Committed (all green, tree clean at each commit)

| commit | what |
|---|---|
| `ba8befc` | a stroke inherits the tool settings the app is on; present fields still range-checked |
| `f28d72e` | `.noise` v0 container, `doc.save`/`doc.export`, dirty flag, layer name |
| `da2de36` | debug wire: closed clients release their slot (`recv` 0 == EOF, not "idle") |
| `ff828ad` | `core/blend.{h,cpp}` - the blend reference; `Rgba8` moved to `core/image.h` |
| `0fee97d` | the document is a layer stack: `Layer{id,name,pixels}`, per-layer history, `composite()`, `layer.*` commands, container N layers, `core/blend` region blend |

### Uncommitted right now

- `tools/tn_probe.py` - the live-tier probe harness, **works, all checks green**, but is not yet:
  registered in `tests/CMakeLists.txt` (want `LABELS live`, `SKIP_REGULAR_EXPRESSION "SKIP:"`,
  timeout ~1800 s), nor documented. It reads only `$THENOISE_URL` / `$THENOISE_API_KEY`, prints no
  secret, and takes `--model --sizes --max-images --upscale --timeout`.
- Nothing else is dirty. Probe findings below are not yet written into `docs/probes/` (do that,
  without a hostname).

### Live server findings (2026-10-09, one box, Klein-4B) - do not re-spend images to learn these

- Discovery: `GET /health` (status/version/`model_loaded` string/`all_models_models[]` with
  `model_name`,`checkpoint`,`device`,`pinned`; `max_models.image == 1` ⇒ one image model at a
  time). `GET /models` returns the box's short list; `GET /models?show_all=true` returns 187
  entries. Model entries carry `recipe`, `labels` (`image`, `edit`, `upscaling`, `chat`,
  `vision`, `custom`), `downloaded`, `size` (GB) and `image_defaults` (`steps`, `cfg_scale`,
  `width`, `height`) - **`image_defaults` is the authoritative source for role defaults**, and the
  list differs per box, so `roles.toml` must resolve against what is actually downloaded.
  Note `Flux-2-Klein-4B` is `recipe: sd-cpp` (excluded by policy) while
  `Flux-2-Klein-4B-TheNoise` is `thenoise` ✓. `Anima-VAE` / `Krea-2-VAE` also show as
  `thenoise`+`image` with their own defaults - they are VAE-swap aliases, not separate roles;
  no clean machine-readable flag distinguishes them, so preference order has to name candidates.
- Timing (warm model, 4 steps): 64² 0.7 s · 512² 1.6 s · 768×512 2.1 s · 1024² 5.2 s ·
  1536×2048 20.3 s · edit 512² 2.6 s · upscale 512²→2048² 5.0 s. Cold start with a big LLM
  resident: 113 s (outlier, per the human).
- **Sizes snap to a multiple of 16, silently**: asking 500×300 returns 512×304. `size_policy` is
  16 in practice, not 64, and a request is never refused for a bad multiple - so the region
  compiler must expect output dims ≠ requested dims (the resample already handles it) and should
  send multiples of 16 itself.
- **`size` is honoured on edits; omitting it does NOT follow the reference image** - it returns
  the model default (1024²). The old claim in `AI-BACKEND.md` ("the first reference image sets
  output size") is wrong for this server; explicit size stays mandatory. Omitting it also costs
  10.8 s vs 2.6 s at 512², so it is not a free mistake.
- **The server answers an identical request in 0.1 s** - there is a response cache keyed on the
  request. Two consequences: (a) a timing probe must vary the seed or it measures a cache hit,
  and (b) the client-side request cache is still worth having but must not be believed as
  "reproduction of a model run". Whether that cache is intended/configurable is an open question
  for the human.
- Reproducibility: same prompt+params+seed **warm** ⇒ byte-identical ✓ (a cold first request after
  a load did differ once - do not conclude non-determinism from a cold run).
- Failure shapes to branch on: 404 `{"error":{"code":"model_not_found","message":…,"param":"model",
  "requested_model":…}}`; 400 `{"error":{"message":"Missing 'prompt' field in request","type":
  "invalid_request_error"}}`.
- **Bad field values are accepted, not rejected**: `seed: "abc"` returned 200. So the request
  builder must validate its own fields; a typo'd field name will silently mean "default".
- Upscale works via `POST /images/upscale {model, image(b64 PNG)}` with
  `RealESRGAN-4xplus-TheNoise` (0.1 GB, `labels: upscaling,image`) - it was absent from the
  default `/models` list early in the session and present later, so do not gate a role on
  discovery alone: try the endpoint and map the refusal.

### To do, in order

1. Scrub the example LAN address from `docs/AI-BACKEND.md` (~line 165) and the mockup line in
   `docs/MVP.md` (~line 124); replace with `http://<server>:13305/api/v1` style placeholders.
2. Write `docs/probes/2026-10-09-live-server.md` from the findings above (no hostname, no key),
   register `tn_probe.py` as ctest `tn_probe_live` with `LABELS live` + skip regex, and correct
   `docs/AI-BACKEND.md` where the measurements contradict it: size multiple 16 and silent
   snapping, omitted-size behaviour, the response cache and its effect on ETA/EMA, discovery
   being per-box and `show_all=true`, upscaler gating, "bad fields are ignored ⇒ validate our own".
   Record in `AGENTS.md` that server URLs/keys never enter the repo.
3. **User guide** (`docs/USER-GUIDE.md`, linked from `docs/README.md`): how to start it, the
   window layout, mouse/keys that exist today (`F`, wheel zoom-to-cursor, right-drag pan,
   left-drag paint, `B`/`E`, `[`/`]`, `Ctrl+Z`/`Ctrl+Y`, `Ctrl+S`, `Escape`/`Ctrl+Q`), the Brush
   panel, Layers panel (add/select/delete, what "active layer" means, what the composite is),
   `.noise` vs PNG (`doc.save` vs `doc.export`), and the debug wire section for people driving it
   from a terminal (`--debug-wire`, `command.list`, `doc.get`, `layer.list`, `tool.pointer`,
   `screenshot`). State plainly what does not exist yet: no AI commands, no masks, no blend
   modes, no move tool, no reorder in the panel.
4. `.noise` **format 2** (decision 1): `manifest.json:format = 2`, refuse 1 naming the version,
   layer entries gain `visible`, `opacity`, `mode` (and later `mask`). Then implement visibility
   and opacity in the composite (`core/blend` scales source alpha by layer opacity for `normal`),
   the Layers panel eye/opacity row, and tests (round-trip keeps them; hidden layer contributes
   nothing; composite-with-opacity equals a reference; `test_document`'s incremental-vs-scratch
   equivalence must cover them). Keep `mode` to `normal` until the blend-mode set is designed -
   a field with one value is honest, one with five unimplemented is not.
5. Then the remaining tool keys (`V`, `M`, `,`/`.`) and the panel rows they belong to.
6. Later, gated by measurement: dirty-rect texture upload; GPU stamp preview; per-layer GPU
   textures + the shader blend pair checked against `core/blend`.
7. Deferred on purpose: CI until MVP; `enhance` role; blob store (until history or variants must
   outlive the session).

### Things that bit me, so they don't bite twice

- Our own reader accepting our own writer proves nothing: the ZIP end-of-directory size was 12
  bytes wrong, `unzip` tolerated it, Python's `zipfile` refused it. `tools/container_probe.py`
  exists because of this. Any new format surface needs a second implementation reading it.
- A missing measurement is a result: `peak frame_ms 0.0` during a running stroke led to the wire
  slot leak.
- The probe bound caught a 71 ms/event composite regression that looked fine in review.
- `CHECK_EQ` evaluates each operand once (fixed); `nlohmann::json::contains()` is always false for
  arrays; `StrokeScratch::grow_to` once copied coverage as if every rect started at the corner.
- Live pointer strokes must equal the same polyline sent as one `tool.stroke` call; provisional
  edits exist so a drag is one undo step and does not compound opacity per frame.
## State at the M0 handoff

`cmake -B build -G Ninja && cmake --build build && ctest --test-dir build` → 7/7 green,
zero warnings with `-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion`.

| Suite | Covers |
|---|---|
| `test_hash` | sha256, base64 encode |
| `test_geom` | view round-trip, anchor-fixed zoom, reversibility, fit containment, origin clamp reachability, rect clip |
| `test_image` | PNG round-trip incl. alpha, decode rejection, region/blit/fill clipping |
| `test_command` | table registration, dispatch, unknown-id error |
| `test_wire` | request/response, multi-frame writes, malformed input, throw mapping, frame bound, token gate, bind errors, stop |
| `test_app` | command surface vs `ARCHITECTURE.md`, `command.invoke`, arg validation, doc disk round-trip, `image.get` pixels, `pixels.compare` single byte, `screenshot` vs capture, view invariants |
| `test_view_golden` | live app over the wire: fit → zoom → pan → canvas capture vs `tests/golden/view_pan_zoom.png` |

Wire commands live now: `app.state` `app.quit` `command.list` `command.invoke` `doc.new`
`doc.open` `doc.save` `doc.get` `image.get` `screenshot` `pixels.compare` `log.tail`
`log.level` `view.fit` `view.set` `view.pan` `view.zoom` `wire.info`.

Measured on this box (Xvfb + llvmpipe, no GPU): 79 fps at 1200×760 with a 1600×1000
document, median frame 5.7 ms, p95 7.8 ms. The golden is byte-exact across runs
(`differing_bytes=0`); tolerance constants in `tools/wire_drive.py` exist only for
cross-driver rasterization, not for slop.

## Fixed after the handoff (observed, not assumed)

* **A document blanked the canvas.** The transparency checker was drawn as one ImGui rect
  per visible screen square, with the loop unclipped to the viewport — so its cost grew with
  the canvas rect, not the screen. A 2400x1600 document at fit zoom in a 1280x800 window
  demanded ~124k vertices against simgui's 65536-vertex staging buffer, which rejects the
  frame's draw commands: black canvas plus a `BUFFER_OVERFLOW` line per frame. It is now one
  2x2 texel tile drawn repeat-sampled over the canvas rect (`gfx/checker.{h,cpp}`): four
  vertices at any zoom, measured 598-640 for a whole frame. `simgui.max_vertices` comes from
  `app::kUiVertexLimit` rather than the default.
* **A document opened on the command line never fitted.** `doc.open` fitted against
  `state.viewport`, which before the first laid-out frame is the framebuffer (or 1x1 on the
  frame where the dock has not sized the canvas yet): a 1600x1000 document at 900x600 fitted
  to scale 0.542 — 868 px wide inside a 612 px canvas — and one reproduced open landed on
  `scale = DBL_MIN` with non-finite origin. A fit now stays pending (`view.fit_pending`)
  until a frame publishes a canvas area that can hold the document; the host applies it via
  `app::apply_pending_fit`.
* `app.state` gained `ui_vertices`, `ui_vertex_limit` and `view.fit_pending`.
  `tools/wire_drive.py` boots the app **on a document argument** (so the startup path is the
  one under test), asserts the fitted rect lies inside the canvas, keeps the frame under
  75% of the vertex buffer, fails if the app logged `BUFFER_OVERFLOW` at all, and checks
  that x4 zoom leaves the frame's vertex count where it was (612 -> 620 now; per-element
  canvas geometry multiplies). Red-checked by building with `kUiVertexLimit = 512`:
  `frame demanded 612 of 512 UI vertices (120%)` plus `the app logged 11 BUFFER_OVERFLOW
  lines`, driver exit 1. Do not re-run a per-rect checker under llvmpipe to prove this:
  ~250k software-rasterized UI vertices per frame wedged the box for minutes.
* **Mouse pan and zoom never fired.** `draw_canvas` gated both on
  `ImGui::IsItemHovered()`, but the Canvas window submits no items - only draw-list calls -
  so that reads whatever another window submitted last and is effectively always false. Now
  `ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)`. Verified by
  injecting X events (libXtst via ctypes): 3 wheel clicks over a point take scale
  0.7250 -> 0.9650 = fit x 1.1^3 with the world point under the cursor pinned to 0.15 px,
  and a 40,-30 right-drag at scale 2 moves the origin by exactly -dx/scale, +dy/scale. With
  the old predicate both are no-ops, which is what it looked like on screen.
* `tests/golden/view_pan_zoom.png` re-recorded: the checker's rasterization changed (a
  viewport-anchored rect lattice became a canvas-anchored texture). Document pixels were
  unaffected — captured an alpha-255 copy of the fixture with the pre-fix and post-fix
  binaries and diffed them.

Still open, when convenient:

* Log line **length** is unbounded. ImGui culls whole lines outside a panel, so the Log
  panel's cost is (visible rows x longest line) rather than all 400 lines; nothing today
  logs a long line, and the frame sits at 0.5% of the buffer. If an AI response body ever
  goes to the log, cap it in `core/log::record` (one home) instead of growing the buffer.
* Frame time under Xvfb + llvmpipe with a 1600x1000 document at 900x600 measured
  99.5 -> 64.3 ms median across the checker fix. Both are far from the 5.7 ms above, which
  was a different scene; re-measure when the compositor and texture cache land.

* **Keyboard shortcuts are part of a command's registration.** `CommandTable::add` takes the
  bindings and normalizes the text, refuses a malformed shortcut and refuses to bind a key
  twice; `app::shortcut_of` turns a sokol key event into the same canonical text and
  `main.cpp` dispatches through the table; `command.list` reports the keymap. Seeded: `F` fit,
  `Ctrl+S` save, `Ctrl+Z`/`Ctrl+Y` undo and redo, `Escape`/`Ctrl+Q` quit - the rest arrive with
  their tools. Two findings: `simgui_handle_event()` returns `WantCaptureKeyboard ||
  WantCaptureMouse` and the pointer is always over chrome, so the old
  `if (simgui_handle_event(ev)) return;` dropped **every** key event (which is why the
  hardcoded Escape-quit never worked); and on a window-manager-less Xvfb display keys reach
  nobody until `XSetInputFocus` picks the app window, while mouse events do not care.
  `tools/wire_input.py` does both and `test_input_gestures` covers wheel zoom-to-cursor,
  right-drag pan, `F`, a failing `Ctrl+S` reaching the log, injected `Ctrl+Z` undoing a real
  stroke, an unbound key doing nothing, and `Escape` quitting.
* **The paint path exists and undoes exactly.** `core/document.{h,cpp}` owns the document
  model (it lived in `app/state.h`, where any command could poke it), `core/history.{h,cpp}`
  undoes and redoes **rectangle deltas** budgeted by stored bytes, `core/brush.{h,cpp}` is the
  CPU stamp path, and `tool.stroke` / `edit.undo` / `edit.redo` are on the wire with
  `Ctrl+Z`/`Ctrl+Y` bound. Measured numbers and what they ruled in and out:
  `docs/probes/2026-10-09-paint-core.md`. Two findings: a one-pixel brush painted *nothing*
  while the antialiasing feather lived inside the circle, and a command that checksums the
  pixels it just painted costs more than the paint did (166 ms -> 10.2 ms per 2048² stroke).

* **The pointer paints, one drag is one step, and the tool table is the only list of tools.**
  `app/tools/brush.{h,cpp}` is the press/drag/release state machine, the UI drives it through
  `tool.pointer` (never around it), `Document` grew a provisional edit (hold pixels aside,
  repaint from them, commit as one step) and `core::kTools` generates `tool.<name>` and its key
  per row, so `B`/`E`/`[`/`]` and the Brush panel are front-ends for commands a wire client can
  send too. `app.state.stroke` reports the gesture in progress. Numbers in
  `docs/probes/2026-10-09-paint-core.md`. Four findings, all of them found by looking rather
  than reasoning:
  - `StrokeScratch::grow_to` copied the accumulated coverage iterating `0..w`/`0..h` as if the
    scratch rect always started at the corner - out-of-bounds reads, and every repaint after a
    growth kept only the newest dabs. A 640-canvas drag left **one dab** while the dab counter
    said 23. Caught by `test_a_live_stroke_keeps_every_dab_it_ever_stamped` (samples columns
    across the stroke) and `test_a_scratch_stamped_in_pieces_holds_the_same_coverage_as_one_stamp`.
  - `CHECK_EQ` in `tests/test.h` evaluated both operands twice, so `CHECK_EQ(h.value("edit.undo",
    ...)["undo_depth"], 0)` ran the command twice and reported a failure that was not there.
    The macro now compares through a function, which evaluates once; a side-effecting argument
    is safe by construction instead of by comment.
  - `nlohmann::json::contains()` is key containment and is **always false for arrays**: three
    keymap assertions passed as failures until `has_shortcut()` replaced them.
  - The incremental-versus-one-call equivalence was vacuous at first: the test painted a
    different polyline with the tool stroke than with the pointer, and both ends agreed on an
    unpainted document. An equivalence test has to be read for whether it can fail.

* **`.noise` v0 is a file, and a stranger can read it.** `io/zip.{h,cpp}` writes and reads ZIP -
  stored members with fixed DOS timestamps, so the same content gives byte-identical archives -
  `io/project.{h,cpp}` lays out `mimetype`, `manifest.json`, `layers/0/layer.png`,
  `mergedimage.png` and carries every other member through unchanged, and `io/file.{h,cpp}`
  writes atomically (`<path>.noise-tmp` then rename). `doc.open` reads `.noise` and PNG,
  `doc.save` writes only `.noise`, and `doc.export` writes a flat PNG without taking the
  document's path or clearing its dirty flag. `doc.get` reports `kind` and `dirty`, derived from
  the path and the pixel history rather than stored alongside them. Costs and what they ruled out:
  `docs/probes/2026-10-09-container-v0.md`. Findings:
  - The writer overstated the central-directory size in the end record by the 12 bytes of the end
    record itself. `unzip` reads such a file; Python's `zipfile` refuses it. Our own reader read
    it happily, because it never compared the directory's extent to where the end record sits -
    which is the general trap: our reader agreeing with our own writer proves nothing about the
    bytes. `test_zip` now parses the end record by hand, and `tools/container_probe.py` has
    `zipfile` read a project the app just saved and decode its layer PNG.
  - Refusals are by version and by count, and both name the thing found: `format` newer than this
    build says which version it met, a manifest declaring two layers is refused rather than opened
    as one (opening it as one throws a layer away), and a `size` that disagrees with the PNG names
    both.
  - `zlib` was already in the build through `libpng`; the ZIP reader uses its `crc32` and raw
    inflate rather than vendoring a ZIP library.

* **The document is a stack now, and the composite is a derived value with one owner.**
  `core/layer.{h,cpp}` is `Layer{id, name, pixels}` (bottom-first, `id` never written to a file),
  `Document` holds `std::vector<Layer>` plus the active index, `core/blend.{h,cpp}` became the
  blend reference the docs had been promising - source-over and erase on straight RGBA8, with the
  one quantisation - and `brush.cpp` now calls it instead of carrying its own arithmetic
  (`test_brush` passing unchanged at 1820 checks is what proves the move was a move). History
  entries name the layer they belong to, so a step stays undoable after the selection moves, and
  `provisional_` is keyed by layer id. `layer.list` / `layer.add` / `layer.remove` /
  `layer.select` are on the wire and the Layers panel is a front-end for them; `composite()` is
  what the renderer uploads, what `doc.export` writes and what `mergedimage.png` holds.
  Costs: `docs/probes/2026-10-09-layers-composite.md`. Three findings:
  - The first `composite()` rebuilt the whole flattened stack on every revision bump, which put a
    canvas-sized blend on every frame of a live stroke: **71 ms per pointer event** on a
    three-layer 2048² document against 10.9 ms single-layer. The probe's own bound caught it, not
    a review. Fixed by two rules in one place - a stack of one is returned as that layer, and a
    repair covers only the rectangles that changed - down to 33.7 ms for the canvas-spanning
    gesture and 9.4 ms for a local one. `test_stroke_probe` now runs the three-layer gesture too,
    so the regression cannot come back quietly, and `test_document` holds the correctness side
    (incremental composite == composite built from scratch, through add, paint, select, undo,
    redo and delete).
  - `platform::read_some` returned 0 for both "nothing readable" and "peer closed" (`recv`
    returns 0 at end of stream), so a hung-up debug client kept its slot and once `max_clients`
    were held **no new connection was ever serviced** - every Python driver that reconnects would
    eventually hang. The symptom was a probe printing `peak frame_ms 0.0` beside events of 34 ms:
    a missing measurement, which is a result. `test_a_hung_up_client_gives_its_slot_back` covers
    it (red before the fix, green after).
  - Structural commands are not undoable yet, and that is stated rather than papered over:
    adding or deleting a layer does not enter the timeline, and deleting a layer drops the
    history entries naming it (`History::drop_layer`) because a change to a layer that is gone
    cannot be put back. Layers also have no visibility, opacity, blend mode or offset yet - the
    manifest has no field for them, which is a format decision to ask for rather than invent.

## Next steps, in order

1. **CI waits until MVP** (decided 2026-10-09): the workflow is not going to run on a runner
   before MVP, so nothing here should spend effort on remotes or runners. The local gate
   stands in for it and is the thing to keep green - `cmake -B build -G Ninja && cmake --build
   build && xvfb-run -a ctest --output-on-failure`. `.github/workflows/ci.yml` stays as
   written and unverified; when it finally runs, expect the golden to be what differs under
   Ubuntu's llvmpipe: read `differing_bytes` / `max_channel_delta`, then either widen the two
   constants in `tools/wire_drive.py` deliberately or re-record there with `--update-golden`.
   Never widen a tolerance to hide a structural diff.
2. **Blend modes, opacity, per-layer visibility and masks.** `core/blend` is the reference for
   `over` and `erase` and nothing else; the mode set, layer opacity and masks extend it, and the
   docs' "CPU reference matched by a GPU shader pair" test only becomes meaningful when the GPU
   blends something - today it uploads the flattened composite. Before any of it lands, ask the
   human about the `.noise` manifest (`AGENTS.md` §7 puts a format change behind that question):
   `visible`, `opacity`, `mode` and a mask reference are new required-or-optional fields, and the
   answer decides whether `format` stays 1.
3. **The rest of the tool keys** (`V` move, `M` rect select, `,`/`.` hardness and opacity) and
   the panel rows they belong to. `B`/`E`/`[`/`]` landed with the brush.
4. **Dirty-rect texture upload**, when a measurement says the whole-texture upload dominates. The
   composite already tracks the rectangles that changed, so half the bookkeeping exists; what is
   missing is a backend that accepts a partial update, or tiles. Three layers at 2048² currently
   cost 33.7 ms per pointer event for a canvas-spanning gesture and 9.4 ms for a local one, and
   the paint plus composite repair are the larger terms - so this is not forced yet.
5. **M0.5 probe harness** (`tools/tn_probe.py`, `docs/probes/`) — blocked on the dev server.
   Ask the human to export `THENOISE_URL` / `THENOISE_API_KEY`; it feeds `size_policy`, role
   default order, ETA seeds and recipe wording, so it should land before M3 designs hard-code
   any of those.

## Architecture review, and what it said

`AGENTS.md` §1 makes this mandatory at a milestone boundary and `AGENTS.md` §1 makes refactoring
always in scope; recorded here so the next session can disagree with it instead of guessing.

**Boundary: the M1 painting column (pointer stroke, undo as steps, tool table).** The design is
no harder to extend than before it, and two places got cheaper:

* `PaintMode`/`kTools` in `core/brush.h` is now the only place a tool is named: a row yields the
  command id, the paint mode and the key. The `tool == "eraser"` comparisons that had grown in
  two files are gone, so adding a tool (a clone stamp, a smudge) is one row plus its handler.
* The stroke's coverage buffer and its dab walk are shared by the one-call and the event-fed
  paths, which is what makes the layers work below cheap: a layer is another `Image` to
  composite onto, not a second gesture implementation.

Places that had to move, and where they went (done in the layer pass that followed this review):

* `Document` held one `Image`. It now holds `std::vector<Layer>` with an active index, and
  `composite()` is the derived flattening the renderer uploads. Because nothing outside
  `core/document` had been reading pixels for editing, the change stayed inside `document.{h,cpp}`
  plus the command and container callers - the property the review was betting on held.
* `provisional_` was per-document; it is now keyed by the layer id the gesture started on, still
  inside `Document`, as this review predicted.
* `scratch_composite` and the over/erase arithmetic lived in `core/brush.cpp`, and `docs/`
  referred to a `core/blend` reference that did not exist. The compositor needed the same maths,
  so `core/blend.{h,cpp}` is now that home, taken verbatim (`test_brush` green without edits is
  the evidence), with `blend_over(dst, src, region)` added for the partial composite. The shader
  half of the doc's claim is still only true once the GPU blends layers; the doc now says so.

New in the review after the layer pass: the composite cache is one value with one owner and one
invalidation path (the pending rectangle), which is what `AGENTS.md` §4 allows a cache to be; and
`History` now knows about `Layer`, which is the one place the model grew a dependency for
convenience - it is core-to-core, and the alternative was a callback per step for no gain.

Deliberately not done: GPU stamp preview during the drag, tile cache, dirty-rect uploads, a
`BlendMode` enum with nothing to blend between layers, and layer visibility/opacity (a manifest
field, so a question before it is code). Each is one measurement away from being
the right call and none of them is forced by the next step.

## Environment notes for this machine

* No `DISPLAY`; a Wayland var may be set with no compositor. Render through `xvfb-run -a` —
  `tools/wire_drive.py` does this itself and skips with a loud reason if neither is available.
* `XDG_RUNTIME_DIR=/run/user/1000` is **not writable** here (owned by `nobody`, mode `------`).
  Anything writing runtime files needs `NOISE_RUNTIME_DIR` pointed somewhere writable;
  `test_app` injects its own dir and `wire_drive.py` uses a temp dir, so the suites are immune.
* Killed runs can leave Xvfb processes holding a ctest output pipe, which looks like a test
  hang. `pkill -x Xvfb` (the `-x` matters: `pkill -f Xvfb` matches your own shell).
* Useful one-liners:
  ```
  cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
  ./build/tests/test_app                       # windowless suites run standalone
  NOISE_RUNTIME_DIR=/tmp/nr ./build/noise-edit --width=1000 --height=620 some.png   # under xvfb-run
  ```
