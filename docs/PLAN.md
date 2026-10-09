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
  `$THENOISE_API_KEY` and say "the dev box". The two example LAN addresses that were in the docs are
  gone; nothing to genericise left.
- Live tier runs only with those env vars set; `tools/tn_probe.py` skips loudly otherwise, and it
  caps requests with `--max-images`. Ask before spending more than ~10 images.

### Decisions taken by the human this session

0. **Drop `.ora` import/export from the MVP.** We do not need OpenRaster in: no `stack.xml`, no
   `.ora` read or write, no second set of layer-stack semantics to keep true. Done - and it made a
   behaviour load-bearing rather than academic, because "we do not read that" now has to be said
   correctly (see `a47a324`).

0b. **PNG, JPEG and WebP open and save are in the MVP** (human, same session): implemented
   (`io/jpeg`, `io/webp`, `io/flat`). Codecs come from the system `libjpeg` (libjpeg-turbo 3.2) and
   `libwebp` via pkg-config, the same way `libpng16` already arrives - a feature cannot exist without
   a JPEG and a WebP decoder, and vendoring two more trees behind the build was the heavier option.
   Flagged because `AGENTS.md` asks before a new dependency: **this one was implied by the request.**

1. **`.noise` format: always bump.** Adding required or optional manifest fields bumps
   `manifest.json:format` and refuses older files naming the version. Do not add optional fields
   under an old version number. (Needed for layer `visible` / `opacity` / `mode` / mask refs.)
2. **Live server is available**; ask when a change needs it, and say how many images it will spend.
   "If you have ANY doubt on how to use the API or if something doesn't work, ping me." The box had a
   huge LLM resident during probing, so first-request load costs measured that day are outliers.
   Which model answers the `edit` role **rotates with what the box has downloaded** - the probe
   resolved Klein-4B early in the session and Klein-9B later - so nothing names one: `edit` resolves
   by recipe + labels + downloaded, smallest download first, `--model` to override.
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
| `a735462` | LAN addresses out of the docs; the no-secrets rule into `AGENTS.md` |
| `c59d6bb` | `tn_probe.py` resolves the role instead of naming a model; reports a spent budget as SKIP |
| `5b8bc33` | live-server probe report, `tn_probe_live` registered, `AI-BACKEND.md` corrected to the measurements |
| `ade3067` | `docs/USER-GUIDE.md`: the panel, the keys that exist, `.noise` vs PNG, driving the wire |
| `e858b12` | `.noise` **format 2**, and what a pointer event and a panel click cost - see `probes/2026-10-09-paint-and-panel-cost.md` |
| `0f87f93` | the probe measures the brush it claims: read settings back from `app.state`, and the corrected cost tables |

### What the last commit changed, and why it was one commit

Three facts, entangled in `document.cpp` and `command_args.cpp`, each verified on its own:

1. **`.noise` format 2.** `Layer` gained `visible`, `opacity`, `mode`; `core::kBlendModes` is the one
   table of modes and manifest names (`normal` only, until there is arithmetic to add); the
   compositor applies them, so a layer shown at 30 % is still painted on at full strength; the eye and
   the slider are in the panel; `format` is 2 and any other version is refused by number. Visibility
   and opacity enter no undo step, like adding and removing a layer.
2. **An interactive command no longer hashes the document.** `layer.select` and `layer.set` answered
   with `layer.list`'s row builder, which SHA-256s every layer: ~500 ms per click and per slider step.
   Rows come from `layers_json`, checksums only from `layers_json_with_checksums` (`layer.list`).
3. **A pointer event costs its own dabs, not the stroke's history.** Repaint scope, geometric buffer
   growth, row `memcpy`. Before: 2.9 ms rising to 108 ms inside one 40-event drag at brush 24; after:
   0.7-1.6 ms flat, and brush size stopped being the variable (spanning strokes 26 ms -> 1.3 ms median
   at every size measured). `stroke_probe` now fails if a drag's last third costs more than 4x its
   first.

Splitting these into three commits would have meant hand-constructing intermediate versions of
`document.cpp` that no test had ever seen green; one commit with a green tree beat a tidier history
with a broken one.

### Flat formats, as built (observed, not assumed)

`io/flat.{h,cpp}` is the table: extension, whether the format holds alpha, whether `quality` means
anything, and the writer. `doc.open` sniffs content (PNG magic, `FF D8 FF`, `RIFF....WEBP`) and
`doc.export` writes by extension, defaulting quality to `io::kDefaultExportQuality`.

- A JPEG has no alpha: the export composites against white and answers `discarded_alpha`, so nothing
  goes missing quietly. `quality` on a PNG export is refused ("it is lossless") rather than ignored,
  which is the same rule the command table now enforces on unknown arguments.
- Decoders cap a side at `io::kMaxDecodedSide` (16384) so a hostile header cannot ask for gigabytes.
- `tools/image_interop_probe.py` (ctest `test_image_interop`, 17/17 green) is the part worth
  keeping: our exports read by Pillow, and Pillow's files read by us, both through the running app.
  Measured here: PNG byte-exact both ways; our JPEG 0.91 per channel from the white-flattened
  composite; a Pillow JPEG read back 3.28; WebP 0.00 where opaque, transparent pixels coming back at
  0.0 alpha, which is the independent confirmation that alpha crosses **straight**, not premultiplied.
- Two things the probe found about measuring, both now in its comments: computing the white flatten
  in int16 overflows (`255 * 255`) and invents a black reference, and a lossy bound measured against
  per-pixel noise is meaningless - the same encode is 0.5 per channel off a gradient and 110 off
  noise, so the lossy assertions run against a smooth pattern and the alpha assertions against the
  noisy one.

### To do, in order

1. **Blend modes**, the increment `kBlendModes` was shaped for: arithmetic in `core/blend` +
   `tests/test_blend` against the reference, the manifest name list (format bump), the panel's mode
   column becoming a chooser, `layer.set {mode}`.
2. **The remaining tool keys** (`V`, `M`, `,`/`.`) and the panel rows they belong to.
3. Later, gated by measurement, in the order the probe numbers say:
   exact fast paths in `blend_layer` and per-layer content rectangles (a uniform layer change is
   75 ms at 3 x 2048^2 in Release - the pass the GPU is supposed to eat); dirty-rect texture upload
   (the floor under a pointer event); GPU stamp preview; per-layer GPU textures + the shader blend
   pair checked against `core/blend`.
4. Deferred on purpose: CI until MVP; `enhance` role; blob store (until history or variants must
   outlive the session).

**Answered and implemented since:** the table now refuses an argument its command does not declare
(`CommandTable::add` takes the accepted names, `invoke` rejects the rest naming the key, the command
and what would have worked; `command.list` reports `args`). Asked of the human because it changes the
wire contract; the answer was yes. `test_app` drives the whole registered surface with a bogus key and
requires every command to refuse it, which is 101 red checks if the rule is removed.

### Things that bit me, so they don't bite twice

- Our own reader accepting our own writer proves nothing: the ZIP end-of-directory size was 12
  bytes wrong, `unzip` tolerated it, Python's `zipfile` refused it. `tools/container_probe.py`
  exists because of this. Any new format surface needs a second implementation reading it.
- A missing measurement is a result: `peak frame_ms 0.0` during a running stroke led to the wire
  slot leak.
- The probe bound caught a 71 ms/event composite regression that looked fine in review. The one it
  did not catch is worse-shape-same-level: an event costing what the stroke had covered looked fine
  at 1 ms and took 108 ms by the end of a long drag. Bounds on medians are not bounds on a drag.
- `app.state.frame_ms` is `sapp_frame_duration()`, which includes the swap wait: a whole-document
  command in the sampled frame reads ~100 ms on this box whether the paint path is slow or not.
  Read the per-command wire timings for work, `frame_ms` for cadence.
- A row builder that "always includes the interesting fields" puts a whole-document hash on every
  interactive command. Cost belongs in the caller's hands, not the formatter's.
- **The wire ignores an argument its command does not read.** `tool.brush` selects a tool and takes
  no fields at all, so `tool.brush {size: 48}` returns ok and changes nothing - which made a brush
  sweep measure one default-sized brush five times under five labels, and made `stroke_probe` print
  "brush 40 px" for a gesture at 24. Probes now read the setting back from `app.state` and fail on
  disagreement, which is the only defence a harness has. Whether the command table itself should
  reject undeclared arguments (it knows them) is an open question for the human: it changes the wire
  contract. Brush fields belong to `tool.set`; `tool.stroke` takes them per call.
- `CHECK_EQ` evaluates each operand once (fixed); `nlohmann::json::contains()` is always false for
  arrays; `StrokeScratch::grow_to` once copied coverage as if every rect started at the corner, and
  `held_aside` copied it again as if the snapshot were positioned at the origin.
- Live pointer strokes must equal the same polyline sent as one `tool.stroke` call; provisional
  edits exist so a drag is one undo step and does not compound opacity per frame. That equivalence
  is what made the repaint-scope change safe to make at all.

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

Deliberately not done then: GPU stamp preview during the drag, tile cache, dirty-rect uploads.
`BlendMode` and layer visibility/opacity have been done since - `kBlendModes` with one entry, the
eye and the slider, and format 2 to carry them - and the measurement that now argues for the rest is
`probes/2026-10-09-paint-and-panel-cost.md`: a uniform layer change still costs a whole-canvas CPU
re-blend (75 ms at 3 x 2048^2 in Release), which is the pass the GPU composite was always meant to
take.

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
