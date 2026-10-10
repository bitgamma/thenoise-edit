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
| `25aa0ad` | `CommandTable::add` declares the arguments a command reads, and the dispatcher refuses anything else; `command.list` reports the declaration | a measurement that silently ignored half its arguments is the worst kind, and the rule is enforced by one table rather than by prose: every registered command is driven with an unknown argument in `test_app` |
| `a47a324` | files are classified by content before being refused: a directory, a foreign ZIP and a foreign mimetype now name themselves | the `.ora` decision made "we do not read that" load-bearing; the app had been answering `Not a PNG file` to an OpenRaster and `basic_filebuf::underflow` to a directory |
| `755c6b5` | `,`/`.` and `Shift+,`/`Shift+.` step hardness and opacity from one row table, with a hover hint on the panel sliders and `CommandTable::shortcuts_for` so the hint asks rather than knows | the brush became fully keyboard-reachable, and `wire_input.py` now proves the *whole* key chain - XTest event to command - which is the only way a dropped Shift cannot hide |
| `88e0ede` | three exact fast paths in front of the blend reference (absent pixel, whole-opaque table, per-alpha-pair table) with the probe written up: repair 82.5 -> 14.6 ms, opacity drag 5 fps -> 49 fps | the slider was the reported defect and the general path was 150x a memcpy; the tables are built by calling the reference so they cannot disagree, and the case that got 9 % slower is reported rather than hidden |
| `3506b8c` | eight blend modes in the one table, one composite rule, `layer.set {mode}` and a panel chooser; `tests/test_blend.cpp` restating the definitions; no `format` bump, with the reason written down | the layer model stops being a stack of overrides, and the test suite gained the one thing it did not have: a specification the implementation can be wrong against (it already caught two order bugs it had been mirroring) |
| `4f67f43` | the whole-texture upload measured instead of assumed: 334 fps while painting at 2048^2 with the upload inside it, so the dirty-rect item closes as a real-GPU question | "we think the upload dominates" was the claim; cadence counted through `app.state.frame` says it does not, here, at these sizes - and the report says where the number would change |
| `abf42b5` | the move tool: `V`, whole-document-pixel offsets, one undo step per gesture, live from a press-time snapshot, `move_steps` in the stroke report | the third tool the keys already promised, and the first one the pointer router had to dispatch by `ToolAction` rather than by name - which found a release-routing bug that only a *real* drag could show |
| `29cddfc` | a layer mask: `Layer::mask`, the composite reading a layer through it, `mask.m8` per layer (`.noise` **format 4**), and `mask.from_selection` / `select.from_mask` | the human's "turn any mask into a selection and back" is a statement about types: one `Mask`, two copies, no adapter and therefore no rules to get wrong. It also cost a startup assert that only launching the app could show, and taught the two composite shortcuts (lone-layer, pending region) that a mask is not part of the layer's bytes |
| `990d85d` | `core/mask`: the coverage plane the document owns as its selection, clipping at both write doors, `select.*`, live marquee, ants from the mask's own runs, `.noise` format 3 | every later writer (fill, paste, the AI composite) gets spatial honesty for free instead of re-implementing it, and a layer mask will be the same type, so the two convert by copying |
| `2bbd35c` | PNG, JPEG and WebP read **and** written, as one table (`io/flat`); exports report `format`, `quality`, `discarded_alpha`; system libjpeg/libwebp via pkg-config; `tools/image_interop_probe.py` registered in ctest | the MVP's interchange claim became true rather than planned, and there is now a test for the failure no in-house codec test can catch: a header written wrong by us and read wrong by us, which any self-consistent pair of codec tests passes |

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

### Blend modes, as built (observed, not assumed)

`core/blend` was a `normal` compositor with a table that had one row. It is now the eight the MVP
names - `normal`, `multiply`, `screen`, `overlay`, `darken`, `lighten`, `color-dodge`, `color-burn` -
in that one table, with `blend_separable(mode, backdrop, source)` as the per-channel definition and
one composite rule (`(1-ab)·as·Cs + as·ab·B + (1-as)·ab·Cb`, PDF 1.4 / CSS Compositing) around it.
`blend_over`, the brush's path, stays `normal`: a mode belongs to a layer, not to a dab.

- **No `format` bump.** The version guards the *layout*; a wider vocabulary changes no field's
  meaning, and a build too old for `color-burn` refuses the file naming the mode and the list. The
  reasoning is in `ARCHITECTURE.md` so the next person does not bump on instinct.
- **`tests/test_blend.cpp` is the specification restated**, not the implementation reused: the
  published formulas are written out again in the test and compared byte for byte against
  `blend_layer` over a 20x15 fixture - five backdrop levels x three backdrop alphas x five source
  levels x four source alphas, asymmetric hues so a channel reading its neighbour cannot pass. 22 992
  checks. Order properties (multiply can only darken, dodge cannot darken, `overlay` is `hard-light`
  with the operands swapped), the ends of the range, region bounds, per-channel independence and the
  brush dab's fractional coverage all hold.
- **The red-checks found two real bugs, and the mirror-image test that hid them.** Swapping the
  order of the two end-of-range conditions in `color-dodge` and `color-burn` survived the first
  version of the suite, because the test had copied the order from the code. The specifications
  evaluate the conditions in order and the corners disagree: `dodge(0, 1)` is black and `burn(1, 0)`
  is white, so black stays black under a dodge and white stays white under a burn. Both orders are
  now pinned by name as well as by the fixture. A third finding: `Document`'s lone-layer shortcut
  had no test on its `opacity` condition at all - "ignores opacity" survived everything - so
  `test_a_lone_layer_is_shown_whole_only_at_full_strength` exists now and turns red for it.
- **The shortcut now covers every mode.** A layer over an empty canvas has no second operand, so the
  composite collapses to the source whatever the arithmetic said; `blends_to_itself` dropped its
  `mode == normal` condition and `test_document` pins the shortcut against the blend path's answer
  per mode. Coverage everywhere, colour wherever covered - the two paths may hold different bytes
  under zero alpha and no renderer can show them, which is why that test compares pixels and not a
  checksum, and why "is the shortcut taken at all" is left to measurement rather than asserted.
- **No GPU blend exists to check against, and nothing was invented to have one.** The renderer
  uploads the flattened composite; `test_blend` is the reference a shader pair will be checked
  against if and when the compositor moves to the GPU (a measurement-gated item below). `AGENTS.md`
  §4 said "the shader pair, checked by a test"; that claim was ahead of the tree and is corrected.
- On the wire and in the panel, the same numbers as in the app: 3072 px document, one painted layer,
  mean RGB of the composite by mode - `multiply` 111.7, `darken` 114.3, `normal` 134.2, `lighten`
  166.5, `screen` 169.2, `overlay` 143.0, `color-dodge` 178.0, `color-burn` 102.5, and exactly the
  same 1620 pixels of stroke footprint changed in every mode, which is the coverage invariant visible
  from outside. Panel screenshot shows the dropdown per row (`color-burn`, `normal`) and the canvas
  repainted from the mode change alone.
- **Cost of the generalisation, measured at the time** (one `blend_layer` pass over 2048², layer at
  78 % coverage, `-O2`): `multiply` / `screen` / `darken` / `lighten` 74-77 ms, `overlay` 77-78 ms,
  `color-dodge` / `color-burn` 81-86 ms - the division and the corner tests cost about 10 % over
  `multiply`, and the first mode timed always measures ~100 ms whoever it is, which is cold pages,
  not arithmetic. Debug (`-O0`) was 245-300 ms, same shape. Two claims from that paragraph were wrong
  and are corrected in `probes/2026-10-09-blend-cost.md`: the loop is **not** memory bound (16 ns per
  pixel against 0.1 ns for a `memcpy` of the same buffer), and the "pre-existing 75 ms baseline" was
  the general-path cost of one pass, not a floor - the same pass is now 2.5 ms whole-opaque and
  8 ms at partial strength, with the arithmetic untouched and byte tables in front of it.

### Rectangular selection, as built (observed, not assumed)

`M` and `Ctrl+D`/`Ctrl+I` were a row of todos; the row that mattered was "selection is
document-level state". Built as: **`core/mask` is the coverage plane** - one straight byte per pixel -
and `Document` holds `std::optional<Mask>` as *the* selection. `restrict_to(mask, ...)` runs inside
`paint` and `repaint_provisional`, so clipping is a property of the document's write doors rather than
etiquette each tool has to remember: the brush, the move tool and every writer that arrives later
(including the AI composite, which is what this feature exists for) get it for free, and a writer
cannot forget it.

- **`nullopt` and a zero-filled mask are different states and both are reachable.** No selection means
  the whole canvas is writable; a selection that covers nothing blocks every write. Saving them
  identically would be a data-loss bug in the reader's favour, so `selection.m8` is written only when a
  selection exists and the manifest's `selection` row decides (`test_project` checks a file with no row
  against a file whose plane covers nothing).
- **Partial coverage mixes in premultiplied space.** Scaling straight RGBA scales the colour too, so a
  half-selected stroke would paint the right alpha in the wrong colour. `test_mask` compares against an
  independent double-precision "scale the amount painted" reference: coverage 0 and 255 exact, in
  between alpha within 1 LSB and colour within 2 - the blend's rounding plus this one's. The comment in
  `restrict_to` carries that measurement so nobody "simplifies" it back to straight alpha.
- **An edit that changes nothing is not an edit.** `paint` and `commit_provisional` compare what they
  were about to write with what they wrote and, if equal, record no step, set no dirty flag, bump no
  revision. Otherwise a stray click outside a small selection spends an undo step that restores
  identical bytes and `undo_depth` stops meaning "edits".
- **`MaskFacts` (covered count, tight bbox, boundary runs) is cached by the mask that owns the bytes**
  and dropped by every write to them, so the overlay, the status bar and the panel read the same runs
  the brush is clipped by. There is no `selection_revision` counter, which is what a second home for
  "did this change" looks like.
- **The overlay is a dashed line on the boundary, drawn in both directions**, each line on the outer
  edge of its boundary pixel, so the ants fall between the selected pixel and the unselected one -
  both directions because a boundary can only be dashed along its own length; the story of that is in
  "The ants that blinked" below. Measured on a screenshot pair (same document, selection up then
  `select.clear`): 2 871 differing device pixels, bbox `(171,189)-(388,402)` against a selection rect
  at `(172,190)-(388,402)` - the 1 px overhang each side is the antialiasing on a line centred on the
  pixel edge. `ImGui::GetTime()` drives the march and nothing else: the phase is in no command reply
  and no document byte.
- **`tool.pointer` is the select tool's path** and `app.state.stroke.marquee` reports the rectangle
  while the button is down, which is the only way for a driver to tell a marquee that tracks the
  pointer from one that appears on release. `marquee()` rounds outward (`floor` of the min, `ceil` of
  the max): dropping the half-covered pixel under the cursor would misreport what was dragged.
- **`tool.stroke` now refuses when the *app's* tool cannot paint.** It validated only the tool named in
  the call, so with `M` selected a wire stroke painted anyway - the class of bug where a command checks
  its argument instead of the state it acts on.
- **`app.state.document.selection` answers with no `canvas` key when there is no selection**, rather
  than `[0,0]`, so "sized but empty" and "absent" stay distinguishable on the wire.
- `.noise` is **format 3**: a plane member plus its manifest row is a layout change, so 2 is refused by
  number. `tools/container_probe.py` grew a third pass where a third-party `zipfile` reader finds the
  plane, its bytes are exactly the rectangle, and the app reopens it with the selection intact - a
  format only our reader can read is not a container.
- Live checks: `test_mask` 4 646, `test_document` 1 720, `test_project` 157, `test_app` 54 937, 19/19
  hermetic, and `tools/wire_input.py` drives the whole thing through real X11 events - `M`, the marquee
  drag (552 of 3072 px), a stroke outside the selection changing no byte and costing no step, a stroke
  through it changing the document and undoing exactly, `Ctrl+I` twice coming back round, `Ctrl+D`
  letting the held-back stroke through. Red-checking (`/tmp/redcheck_selection.py`, 18 mutations over
  the clip, the door, the marquee rule, the live report, the refusals): **0 survived**, after four
  survived the first pass and named what the tests did not yet pin.

**What this deliberately did not build**: layer masks, and the two conversions. They are one field on
`Layer` plus the conversion commands, now that `Mask` exists - and doing them *before* mask painting is
the order that keeps `Mask` the one representation instead of a selection-shaped type with a mask
bolted on afterwards. Feather, lasso, grow/shrink and snap-to-colour all attach to the same `Mask`.

### The ants that blinked: a bug report, and what it turned out to be

The human's report after the selection landed: *"the selection marker is strange, it blinks very fast
and while horizontal lines are stroked, vertical ones are solid or don't appear."* Both halves were
real, and the second one was an architecture error in `MaskFacts`, not a drawing typo.

- **The boundary was only grouped along rows.** A vertical edge is then one run per row, each one
  pixel long, and dashes placed along x on a 1-pixel run make that edge *solid or absent* - and it
  flips as the pattern moves, because whether the single pixel lands inside a dash depends on the
  phase. The first repair grouped the *pixels* both ways (`rows` + `columns`). That was still wrong,
  and the report came back: *"it is basically a barely visible blinking dashed frame."* Grouping
  pixels on a boundary by row and again by column double-reports every corner and reports a diagonal
  staircase as dozens of 1-pixel runs; what the overlay actually wants is neither.
- **The mask now reports boundary edges, once each, labelled with the side they face.**
  `MaskSide {up, down, left, right}` and `MaskRun {side, fixed, from, to}` in `core/geom.h`;
  `Mask::boundary()` in place of `rows()`/`columns()`. An edge is a covered pixel's neighbour step in
  one direction, so a horizontal edge is one run along x, a vertical edge is one run along y, a
  rectangle yields exactly four runs, and a hole yields four more - all asserted by walking the plane
  and requiring each edge to appear once and each maximal stretch to appear as exactly one run. The
  second shape family in that test - two rectangles with a gap on one line - is what kills a run that
  runs on through the gap. This is also the first representation that a future lasso or wand can
  feed: they produce coverage, and coverage produces edges.
- **"Blinks very fast" was the march speed**: 32 px/s over an 8 px light-dark pair is four pattern
  cycles a second. It is now 8 px/s over a 6 px pair - one and a third cycles - and dash length is
  measured in **screen** pixels, so zooming changes how much selection a dash covers and never how
  crisp the ants are.
- **Different sides moved at different rates, and the reason was the parametrization.** Placing the
  pattern by screen coordinate, anchored per run, makes a side that the outline traverses backwards
  march the other way; sampling the colour *on* a cut makes neighbouring dashes the same colour; and
  rounding cuts in screen space at a non-integer zoom makes them collapse onto one pixel. So the dash
  pattern is now a function of one coordinate shared by the whole outline: distance travelled along
  the bounding-box ring, `AntRing::along(side, axis)` - up from the left, right down the right side,
  down from the right, left up the left side, all with unit slope. `ant_dashes()` solves for the cuts
  on that coordinate (walking the multiples in the run's own direction, because the `up`/`left` sides
  run backwards against the axis) and returns `{from, to, light}` in screen pixels. Every side, hole
  and disjoint piece therefore inherits one phase, one speed and one direction.
- **The pattern logic left `ui.cpp`.** It is `app/ants.{h,cpp}` - headless, no Sokol, no ImGui -
  because none of the above is testable through a frame and all of it was previously invisible.
  `test_ants` (14 467 checks) covers: dashes tile a run with no gap and no overlap; colours alternate;
  dash length is exactly 3 screen px at any zoom, including the worst case where one doc px is
  0.025 screen px; and marching a phase by the speed for one second moves a given screen point by
  exactly one pattern period (measured on *every* doc pixel of a selection, not sampled).
  `app.state.ants` reports `phase_px`, `time_s`, `dash_px`, `speed_px_per_second`, `frozen_phases`,
  and `ants.freeze {"phases":[p,...]}` draws bands at fixed phases side by side so a screenshot can
  be measured against arithmetic.
- **Two arithmetic bugs the tests found**, worth naming because neither was visible in a screenshot:
  the colour of a dash was taken from its end point, which is a cut, where the wrap puts it in
  whichever band it likes - so two neighbouring dashes came out the same colour (fixed by sampling the
  middle); and the multiplier walk had to follow the run's own direction (fixed by `step`). Both are
  mutation-checked.
- **One thing that looked wrong and was not**: at a stride of 4 device px, light coverage measured on
  the ant line came out at 43 %, which reads as "43 % of the boundary is missing". It is not -
  light and dark dashes tile the boundary with no gap, so every boundary pixel is marked in every
  frame, which is what an editor's ants do; the *colour* is what marches. Probing every device pixel
  across the line over-samples the antialiased one and under-counts the dark half against the fixture's
  own bright colours. A still frame of correct ants cannot look dashed; the movement is the feature.
- **What the previous verification had missed**, and why: the earlier check was a whole-image diff with
  a bounding box. It proved the ants were *on the selection* and proved nothing about their pattern,
  because a bounding box is insensitive to whether the four sides are drawn the same way. The
  replacement asks per edge. Measured on the live app at 3.67 px/doc px, dark coverage per side of a
  60x40 rectangle (expected 180 device px each at 3 of every 6): top 90, bottom 87, left 93,
  right 91 - each side within one dash of its neighbour, and the whole outline at 366 dark against
  367 computed. Same probe for an inverted selection, a 1-px-wide selection, two overlapping
  rectangles and a selection in the canvas corner: all within ~1 % of the arithmetic, including the
  canvas-edge case where the band is nudged one pixel inside so it cannot be clipped away. A test that
  summarizes cannot catch a defect that is different per side - which is what both reports were.
- **No test may hang, and one could.** `ant_dashes()` walked its multiples in a `while` over floats;
  it is now a counted walk bounded by the run's own length in dash widths, with a non-finite guard,
  and the overlay only walks the part of a run inside the viewport (with the phase interpolated to the
  visible start, so clipping does not move the dashes).

### Layer masks and the two conversions, as built (observed, not assumed)

The human's requirement was *"easy to turn any mask into a selection and selection into a mask"*,
which is a statement about types rather than about features: it is why the selection landed as
`core::Mask` one commit earlier and why this slice is small. `Layer` gained
`std::optional<Mask> mask`, the composite reads a layer through it with `scale_by_coverage` -
`restrict_to`'s premultiplied mix with nothing underneath - and the commands are
`mask.from_selection` and `select.from_mask`, both copies, both outside history.

- **A mask hides, it does not clip writes.** Painting under a masked-out region is allowed and real;
  `test_a_selection_and_a_mask_fence_different_things` pins both halves, including taking the mask off
  and finding the pixels that were painted beneath it. Selections fence the write doors, masks fence
  the canvas; conflating them would make a mask destructive on the next brush stroke.
- **Two shortcuts had to learn about it.** A lone visible layer at full strength used to composite by
  returning the layer's own bytes - legal, and wrong once the layer has a mask, because that path
  never touches coverage. And a mask change widens the pending region to the **whole canvas**: what a
  mask hides is outside its bounds too, so a region taken from either mask's `bounds()` leaves the
  composite showing the mask that *used* to be. Both were found by the existing incremental-vs-scratch
  property test once masks were added to its journey - the test that was written for a different bug
  and caught this one.
- **`nullopt` and a covered-nothing mask are different documents in the file as well as in memory**,
  so `layers/<n>/mask.m8` is written only when there is a mask and the row decides, exactly as the
  selection plane does. `tools/container_probe.py` now reads that plane with a third-party `zipfile`
  reader and reopens the file: the byte set equals the rectangle, and opening does not mark the
  document dirty.
- **The empty-document crash the tests could not see**: the Brush panel read
  `doc.layer(active())` unconditionally, and a fresh app has no document - so the app asserted on
  startup, while every test creates a document first. Found by launching the app for the screenshot,
  which is the argument for launching it.
- Live checks: `test_mask` 4 962, `test_document` 1 862, `test_project` 191, `test_app` 55 964,
  19/19 hermetic in Debug and Release. `wire_input.py` converts a pointer-drawn marquee into a mask on
  the running app: composite `31b14367 -> 362ff014`, layer checksum unchanged, undo depth unchanged,
  and `select.from_mask` returning the same bounds. Red-check: 21 mutations over the scale, the
  composite, the door, the manifest row and the two refusals - **0 survived**, after one
  ("a mask change also records history") initially survived and forced the test to say *what it means*
  (`undo_depth` before and after a mask change around a real stroke) instead of only that undo is
  unavailable on a document that never had a step.

**Not here**: painting into a mask, enable/disable/apply/view, mask management UI (there is no way to
draw a mask except by selecting first), and per-layer content rectangles - which a mask makes worth
doing, since a masked layer's visible box is `min(pixels.bounds, mask.bounds())`.

### To do, in order

1. ~~**Blend modes**~~ - done; see "Blend modes, as built" above for the arithmetic, the
   specification-restated test, the two order bugs the red-checks found, and why the `format` number
   did not move.
2. ~~**Rectangular selection (`M`)**~~ - done as the section above: `core::Mask` as the document's
   selection, clipping at the write doors, `select.*`, live marquee, `.noise` format 3.
3. ~~**Layer masks, and the two conversions**~~ - done as the section above. What is left of the idea
   is mask *painting* (item 5) and a mask affordance in the Layers panel: today a mask can only come
   from a selection, and can only be removed by converting a covered-nothing selection.
4. **The rectangle is still the only shape.** `Mask` holds coverage, so an ellipse, a lasso or a wand
   is a new way to produce runs of it rather than a new type; grow/shrink/feather are `MaskOp` rows or
   one pass over the plane. Neither the selection nor a layer mask has to change when they arrive.
5. **`core/maskops`**: rasterize shapes, flood fill, feather, morphology, colour-match - the pieces
   mask painting and the region compiler both want, in the order the first of them needs them.
6. **The rest of the `edit` group** (`cut`, `copy`, `paste`) and the first AI command with a region
   compile behind it, in that order. (Undo/redo have been live since the gesture fix; `crop` and
   `flatten` wait on tool UI.) A masked layer is the natural source of an inpaint region: subtracting
   the selection from the layer's mask and taking `Mask::bounds` is already the crop a recipe wants,
   and `restrict_to` is already the way back in.
7. ~~Exact fast paths in `blend_layer`~~ - done, and it is what the opacity slider was waiting for:
   a whole-canvas change at 3 x 2048^2 went 100 ms -> 22 ms on the running app (peak frame), dragging
   the opacity went 200 ms -> 20.4 ms per frame counted, so 5 fps -> 49 fps, and the repair alone
   82.5 ms -> 14.6 ms Release / 806 ms -> 269 ms Debug. Three exact paths in front of the
   reference, tables built by calling the reference, checked against it byte for byte; the one case
   that got slower (9 %) is a layer whose alpha differs every pixel, written up with the rest in
   `probes/2026-10-09-blend-cost.md`. **Dirty-rect texture upload is closed by measurement, not by
   implementation**: cadence counted while painting is 334 fps at 2048^2 and 375 fps with a 300 px
   brush, against 2.3 ms idle, and those frames already carry the whole-texture upload - ~0.7 ms at
   2048^2, ~7 ms at 4096^2, where the repair is 60 ms. It is not a defect on this box; it is a
   real-GPU question and the probe says where the number comes from. Still open, in cost order:
   per-layer content rectangles (the repair at 4096^2 is 14 fps and is arithmetic), GPU stamp
   preview, per-layer GPU textures + the shader blend pair checked against `core/blend`.
8. ~~**The move tool** (`V`)~~ - the active layer slides under the cursor: whole document pixels (no
   resampling, so nothing blurs), the button-down point as the anchor, one undo step per gesture, and
   a tremor under half a pixel is not an edit. The layer is redrawn from a snapshot taken at press, so
   a gesture fed event by event lands on the same bytes as one offset applied at once - which is what
   let `test_app` check it against the *specification* of a translate (`after(x,y) == before(x-dx,y-dy)`,
   transparent off-canvas) rather than against the implementation, over four offsets chosen because the
   interesting cases are not symmetric: a rightward drag can never expose a vacated pixel that held
   content, so the leftward ones are the only proof that what leaves is gone. There is no ghost overlay:
   the provisional edit shows the real pixels live, which is better than a preview and needs no second
   renderer. `V` and the drag are proven through injected events in `wire_input.py`.
9. ~~**Rect selection** (`M`)~~ - done; see "Rectangular selection, as built" above. What it assumed
   and what stayed true: the selection is document-level state (a `Mask`, not a rectangle, so a lasso or
   a painted mask needs no new type), the tools honour it because the *document* applies it, and the
   composite and region paths needed nothing - clipping happens on the way in, so the flattened image
   never learns that a selection existed. `selection.{h,cpp}` was not created: a second coverage type
   next to `Mask` is the thing the design now forbids.
10. Deferred on purpose: CI until MVP; `enhance` role; blob store (until history or variants must
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
- The host's event routing is a seam tests cannot see from the command side. Gating the mouse release
  on `stroke.active` meant a wire-driven move gesture committed fine and a real drag never ended - the
  probe that found it moves a real pointer (`wire_input.py`), which is the only harness that can. Every
  gesture-shaped feature needs that check, not just a `tool.pointer` test.
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

*(Superseded - kept as the M0 record. The live list is "To do, in order" in the handoff section
above; items 2-4 here have since been built, and `.noise` is format 3.)*

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
