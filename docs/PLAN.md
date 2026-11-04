# PLAN — what is next, and the reasoning the code cannot carry

Last re-read of the document geometry and AI landing path: **2026-10-29**. `doc.resize` and
`doc.crop` now move pixels, masks and marked coverage together, and undo/redo exchange entire
canvas states so older per-pixel history is valid again once the original extent returns. New AI
answers retain the generated crop under a separately editable mask. The prior 2026-10-26 notes
below remain historical. Local image resampling and fast model upscaling are now implemented;
best-quality generative upscaling remains a separate future option.
`AGENTS.md` is normative and outranks this file.

## 2026-11-04 - Long prompts without clipping

**Architecture verdict:** The panel, selection-verb popup, and New Image dialog share the
same word-wrapped, height-adjusting multiline widget, backed by the existing shared verb
buffers and `ai.intent` commands. The shared prompt buffers now use growable strings instead of the former 220-character
limit. The floating popup grows with the text while capping its field height and remaining
inside the canvas viewport; beyond that limit the text field scrolls. Enter still runs
an action in the popup, while Shift+Enter can insert a line break. The normal panel and
New Image dialog use Enter to insert a new line. No changes to the recipe or AI API.

## 2026-11-03 - Undo across resolution version creation

The creation of a new image-size version is now a virtual Undo/Redo step in the
Document, not a stack deletion. Native per-version History continues to undo layer
strokes and ordering before the switch-back boundary is crossed. Version ancestry
is saved as `parent_id` with each stack; this allows Ctrl+Z to return to the
previous resolution, Ctrl+Y to reselect the generated version, and subsequent
Undo/Redo to retain both stacks exactly. Explicit version navigation leaves the
per-version histories intact, but clears pending redo transitions to avoid replaying
an outdated navigation branch. A previously armed deferred upscale is cleared if
Undo/Redo switches versions. The `Entire layer stack` mode is exclusive to Lanczos3:
other model choices are visually disabled, and command entry points reject starting
AI Upscale or deferred AI Upscale until that option is unchecked.

## 2026-11-03 - Resolution versions instead of scaled hidden originals

**Architecture verdict:** A full-image upscale is a new document version, never a hidden
resampled duplicate stack. The active version owns one layer stack and its own undo/redo;
inactive versions retain original native pixels, masks, selection, and ordering. Switching
changes the working version without resampling; pending AI landings compiled for a different
version are invalidated. The `.noise` format is now v5 and persists all version stacks,
active version and native extents. Local Lanczos3 defaults to a composite-only version;
`whole_stack=true` explicitly creates a version whose layers are resampled individually.
Selection and layer coverage use non-ringing linear interpolation, and marching ants use
a half-coverage contour rather than treating tiny resampling tails as fully selected.
A version may not receive a layer dragged from another version until an explicit
cross-resolution layer copy operation is introduced. Undo history remains session-local.

## 2026-11-03 - Generative landing grid and visible errors

**Architecture verdict:** The edit model may return an image aligned to a coarser backend
latent grid than the size it was asked for. Deferred upscale now accepts a small bounded
grid difference (including 752x816 -> 768x832) and resamples to the exact user-selected
document size. A significantly different answer is still refused. A successful model
request whose result cannot be applied is a **landing failure**, not an endlessly
"landing" job; the latest landing error is visible in Tools and on the AI status line,
while `job.get` preserves its reason and the deferred target remains armed for retry.

## 2026-11-03 - Exact deferred document size and upscaler UI

**Architecture verdict:** The user-selected document size and the edit-model output
grid are separate geometry. Deferred upscale records the desired document dimensions;
the edit request rounds up to the model grid while retaining that exact final target
in the landing record. A correctly sized response is resampled once on landing, inside
the existing atomic document/Undo operation. Validation uses the snapped request for
model limits. Pixel upscalers use a dedicated endpoint and do not require confirmation
to unload the resident image-edit model. The single size panel now names the selected
AI Upscaler, wraps its help, and gives operation-specific feedback in place.

## 2026-11-03 - Direct pixel-upscale scale factor

**Architecture verdict:** `upscale_factor` is optional in the provider's upscale field
policy and is sent by the existing request builder as a number. The requested target
controls the factor, so the client no longer receives/stages a native-scale image
or rejects targets based on an unnecessary intermediate 64-Mi-pixel client guard.
TheNoise may still construct a native-resolution intermediate internally.
The landing only permits at most 1px of server rounding,
which keeps the selected document dimensions authoritative. The updated Lemonade server
must forward this field to TheNoise; an older server that ignores it cannot silently
install the wrong result. Deferred generative edits continue using their own `size` field.

## 2026-11-03 - Unified resolution and next-edit upscale

- A single Image size / Upscale target drives local Lanczos3 and explicit native
  2x/4x ESRGAN model choices. The upscaler no longer inherits the region editor's
  2-Mi-pixel input budget. An updated Lemonade endpoint forwards
  `upscale_factor` for intermediate scales directly to TheNoise.
- `image.upscale.defer` records the target for the next selection-scoped `/images/edits`
  request. The model gets the original full image as input and the larger requested
  **output** `size`; this is one generative edit, not a second queued pixel upscaler.
  A matching response replaces the document in one undoable layer/geometry change;
  stale or mis-sized responses are refused. Failure retains the pending target for retry.
- A selection-guided generative upscale is a **full-frame** recomputation. The
  surrounding image is not pixel-locked, and the output must fit the chosen edit
  model's published ceiling. This differs intentionally from ordinary masked edits.

## 2026-11-01 - Image size and pixel upscaling

**Architecture verdict:** Canvas resize only moves image coordinates, while image size
changes their scale. `Document::resample_image` now performs Lanczos3 over every layer's
pixels and coverage, including the independent selection, as one undoable geometry step.
`doc.resample` is the command and UI entry point; geometry epochs invalidate pending
AI edits when dimensions change.

**Dedicated AI upscale:** The existing `upscale` recipe uses `/images/upscale`, but
must not share the inpaint region compiler's grid-aligned request or the landing
compositor that returns an answer to the original canvas. The native composite is
sent unchanged and the returned 2x/4x image is installed at its native dimensions;
underlying layers are resampled and kept hidden below the enhancement. Only this
path bypasses edit-grid snapping. The ordinary model roles, job queue, field-policy
and recorded transport remain in use. The explicit role preferences can resolve
known upscalers whose catalogue entry lacks the optional `upscaling` label; model
recipe and explicit name remain checked.

**Remaining:** A generative `AI upscale (best)` recipe is distinct from fast ESRGAN
pixel enhancement and is not implemented. Verify the normal ImGui build and full
recorded-backend suite in CI; the headless core is independently tested with C++20.

## 2026-10-30 - Explicit selection-edit cycle over the AI result

The marked-area bar now has three queue-backed states: actions, generating and editing. During
execution, the four operations are disabled in the floating bar and by the shared `ai.intent`
readiness guard (which also protects shortcuts and the panel). Only a successfully **landed**
masked result enters `Editor mode active`; errors, cancellations, refused landings and the
blank-first-layer special case do not pretend an editable mask exists. A new `verb.new_edit`
command re-arms marking without changing the result layer, and `verb.bar` exposes mode and
clickable button coordinates for the native probe. Result identity and document epoch prevent
new layers, document replacement or Undo from inheriting an old edit state. The return button
anchors to the resulting mask or its saved job bounds even after clearing a selection/mask.

The AI job queue remains the source of progress and landing; the selection-edit controller only
records which request owns the on-canvas UI cycle. No new AI backend path or document format.

## 2026-10-31 - Outpaint launch and continuous AI-mask editing

**Architecture verdict:** The existing `doc.resize` geometry step and `region-fill` recipe
already cover outpainting; a new backend recipe or layer format would duplicate the same
concept. The missing piece was the user-facing handoff: `doc.outpaint.prepare` composes
one geometry change, the newly exposed selection, the existing Fill phrase box and a suggested
prompt. The model request still requires an explicit **Run**. Geometry-only resizing remains
separate. The command ensures Coverage targets the marked area after an earlier mask edit.

**Seam correctness:** `compose_editable_result` keeps the full generated crop and the editable
coverage mask. At newly transparent source pixels, preserve the original selected coverage
instead of blurring it toward zero at the outer canvas boundary; feather the transition over
existing source pixels only. A whole-image pixel test confirms the expansion is opaque and
original pixels beyond the selected/feathered region are unchanged.

**No extra mask commit:** The original Document provisional mask gesture is reused for edge
sliders: each move recomputes from the gesture's start mask, release writes one history step,
and cancel restores it. Mask gestures now choose the `PaintFence` explicitly, so editing the
result mask is not clipped to an unrelated marked area for the next generation. The panel
removes Apply edge; Undo/Redo remain the reversal mechanism. No project-format migration.

**Verification scope:** core canvas/compose/document tests were run using local C++20
compilation; the app command/UI and full recorded-backend suites still require the normal
CMake dependencies and live verification requires an opt-in configured backend.

## 2026-10-29 - Crop, canvas size, outpainting and editable AI seams

**Landed in this change:** `doc.resize` translates all layer pixel planes and layer masks without
resampling; `doc.crop` keeps the requested rectangle. The Canvas size / Crop panel exposes an
anchor, pixel dimensions, a crop rectangle and crop-to-selection. Resizing selects precisely the
newly exposed pixels by default. That mark is immediately usable with the existing Fill workflow;
selecting a different region or selecting everything lets the editor regenerate only that portion
or the complete enlarged canvas. The actual AI request can still be downsampled by the model's
published pixel cap; the resized **document** has the requested size regardless.

**History and jobs:** `Kind::canvas_replaced` holds the old layer stack **and** the marked area
as a single reversible state. Earlier rectangles remain valid when the old extent is restored.
`geometry_revision` propagates to the document epoch on resize, crop and undo/redo, so a queued
answer cannot be pasted onto a canvas whose coordinates have changed. A large single geometry
snapshot remains undoable even if it exceeds the nominal 128 MiB history budget.

**Editable seam:** A generated edit now keeps the full colour-matched requested crop on its
new layer, with the original coverage/lock-band as an editable layer mask. `mask.edge` applies
harden, pixel growth/shrinkage and optional feathering; painting on the layer mask makes local
corrections. Undo records mask changes. **Only pixels returned in the generated crop can be
revealed**: reaching outside it requires a new AI request. The empty-first-layer special case
still lands directly on that canvas. The existing flattened `ai.compose` preview remains unchanged.

**Still outstanding:** actual image-size resampling (Lanczos, AI upscale) is separate from
canvas size; a dedicated `extend` recipe can refine the outpainting language later. The native
ImGui/runtime and a live model round trip require the normal full dependency-equipped build.
See [CANVAS_OUTPAINT.md](CANVAS_OUTPAINT.md) for the commands and workflow.

## 2026-10-26 - The window stops offering a theme

The theme dropdown left the file row. Nothing about the palette moved: `ui_theme::Palette` is still how
every panel names a colour, `io/config` still persists the choice, `ui.theme` is still the door - the probe
harness sets it for the viewport golden, the file-dialog probe runs the native chooser in both themes, and
the browser build still drives the page chrome from it. What went was the widget, because a dropdown with
two entries is a design decision about light appearance that nobody has made yet, and a half-decided
appearance in the menu is worse than no appearance in the menu: the dark palette is what a person gets
until the light one is designed rather than merely defined.

## 2026-10-26 - The name and the factor land on top of the module split

Two weeks of parallel work met here: this branch wrote inline rename (`layer.rename`) and layer scaling
(`layer.scale`) into the single `ui.cpp`, while `origin/main` emptied that file into `ui_canvas` /
`ui_layers_panel` / `ui_tool_panel` / `ui_references` / `ui_verbs` / `ui_front_door` / `ui_theme` and, in
the same window, put a layer's arrival, departure and mask plane into the timeline. Merging was a port, not
a line merge, and three of the collisions were design questions rather than text.

**The panel's two fields live in the module now, not in the file they were written in.** `draw_layer_title`
holds the name field where the name was, `draw_layer_preview` holds the factor field where the `Scale`
button was, and `draw_layers_panel` wraps the cards in the `layer-list` child with the stack's verbs pinned
under it. The card colours came out of the merge and into `ui_theme::Palette`: the branch had been pushing
hardcoded `IM_COL32` card backgrounds, upstream had moved exactly those into the semantic palette with a
light theme beside them, and a second home for the same colour is the kind of thing that shows up as a dark
card in a light window. `InlineField` and `typing_in_layer_field` stayed in `state.h`, so the command door
(`layer.rename.begin`, so `F2`, so the wire) and the card's double-click still share one seeding rule.

**A rename step and the deleted `Stacked::name` were the real conflict.** The rename entry stores the name
that is not current in `Stacked::name`; upstream deleted that field on the rule that a layer's name travels
inside `whole`. Both rules were kept, because they answer different edits: a departing layer carries its
whole self, and a rename carries one string — routing a rename through `whole` would carry both pixel planes
of the layer through an operation that must move no pixel and resample no thumbnail (the probe asserts
exactly that). `Kind::layer_renamed` therefore owns a `name` field with that argument on it.

**`Applied::coverage` is gone, and that is what made room for the rename.** The flag meant "no texture was
repainted" only because a moving mark was the single kind that paints nothing; a rename is the second, so
the class is now real and the document derives it - `!structure && rect.empty()` - instead of a flag that
can fall out of step with the kinds. A third kind costs one line in `step_back` and no change in
`Document`. The `Applied` contract in ARCHITECTURE §Undo says it that way now.

**The mask section scrolls with the cards, so a driver asks the list where it is.** Upstream's probe waited
for `layer_panel.mask_add` to be inside the panel window and then pressed it, which worked while the panel
itself scrolled. It is inside the pinned list now, so a row the list clipped still reported a box inside the
window, and a wheel over the pinned row scrolled the panel, which has nothing to scroll: the wait is against
`layer_panel.list` and the wheel is aimed at it, which is the same rule a card's title row was already held
to.

**Running the suite under `xvfb-run` makes the click probes lie.** `wire_drive` starts its own Xvfb at the
size a probe asked for only when `DISPLAY` is unset, so `xvfb-run -a ctest` hands every probe the wrapper's
server instead and every injected press lands somewhere else - three probe tests red on a tree with nothing
wrong with it. Run plain `ctest` on a box with no `DISPLAY`. Both trees were then compared claim by claim:
`ctest` 41/42, with `test_file_dialog_ui` the one failure, identical on `origin/main` before the merge (the
GTK chooser opens but cannot be driven here - no `Xvfb` of its own to move, and `dconf` cannot write
`/run/user/1000`), and `tools/layer_panel_probe.py` fully green including the rename and scale claims, which
upstream does not have.

**What the next feature touches.** A gesture for `layer.scale` goes on the canvas in `ui_canvas.cpp` and
sends the command that exists; a third card field goes in `draw_layer_*` with one `InlineField` in
`AppState`; any entry that changes no pixel is already in the no-repaint class and needs no new flag; and
the panel's scroll region is `layer_panel.list` for every control inside the list, so a driver never has to
know the panel's height.

## 2026-10-25 - Freehand Lasso

Lasso uses the existing `Mask` bytes and `MaskOp` rules. `maskops::rasterize_lasso` makes a patch only for
the clipped path bounds, fills self-crossings with even-odd scan conversion, and integrates sixteen
subscanlines per row for antialiased edges. `tool.pointer` captures the gesture, coverage target and
operation on press; release commits one marked-area or layer-mask step, while `cancel` drops the
provisional transaction. The command `select.lasso` reaches the same rasterizer for marked-area edits.

Architecture verdict: no second selection model or persistent document field was needed. The next crop
and resize milestone still owns the outstanding canvas-extent/history constraint; this feature did not
change it. Core geometry, command results, exact undo bytes, cancellation, both coverage targets, and
the real mouse path at two zoom levels are covered by the existing tests and input probe.

---

## Rules every change has to keep

Not preferences. Each one exists because breaking it was observed as a bug.

1. **Rule A — an answer always lands on a new layer**, above the layer the region was flattened from.
   No preview-into-the-active-layer mode exists. The upload is always the flattened composite of the
   region (`Document::flatten_over(crop, active)`), never the single layer.
2. **A per-frame read computes nothing expensive.** `ai.intent` is read by the status line every frame:
   it quotes (endpoint, size, model, steps, measured ~s) from geometry and tables and preps / resamples /
   PNG-encodes nothing. `AppState::upload_encodes` counts every upload encode and a test fails if a read
   raises it. `ai.preview` is the only pre-flight compile, and it is for drivers and probes, not a frame.
3. **Documents, jobs and app preferences have separate lifetimes.** No answer cache, no job file, no
   `job.retry`, no durable queue. The runtime directory is scratch — startup deletes the previous run's
   `ai/` and `captures/` — and nothing is read back from it. Pressing the verb again *is* the retry: it
   recompiles against what the document is now and costs a second request. The UI theme and recent-file
   list are app preferences in config, not document or job state. Any future durable store states its
   identity scheme and eviction policy before it gets a writer.
4. **The words an operation asks for are typed.** An `ask` field has no default anywhere; a run with a
   blank or whitespace-only asked-for field is refused, naming the recipe and the field.
5. **Every feature is observable on the debug wire, through the same command table the UI uses.** If the
   UI cannot do it via the table, neither can a driver — that is a finding to fix, not a reason for a side
   door. `screenshot` / `image.get` exist so a vision model can look at the result.
6. **One fact, one home; no default arguments; no parallel representations.**

   | fact | home |
   |---|---|
   | commands, titles, key bindings | the command table (`command.list` reports all three) |
   | verb → recipe, key | `app::kVerbs` — a row names no label, words or scope; those are the recipe's |
   | roles, models, size and context policy | `assets/roles.json` ∩ `/v1/models`, resolved in `ai/registry` |
   | fields a request may carry | `ai/provider`'s field policy — the only place a field can be added |
   | prompts, region/prep/composite policy | `assets/recipes/*.json` |
   | document model and its defaults | `core/document` |
   | blend maths | `core/blend`'s reference, mirrored only in `tests/test_blend` |
   | app preferences | `io/config` (`ui_theme`, recent files), the environment |
   | document state | `core/document`, `manifest.json` |
7. **The default `ctest` run is hermetic and opens no sockets.** `$NOISE_AI_FIXTURES` selects the recorded
   transport. Live-tier tests are labelled `live`, registered only with `-DNOISE_LIVE_TESTS=ON`, skip
   loudly without `$THENOISE_URL` / `$THENOISE_API_KEY`, assert contract and invariants and never model
   output, and put the resident model back (`server.load`'s argument is `pinned`). No URL, host or key
   ever enters the repo.

## 2026-10-23 — UI module split and theme preference

The original Dear ImGui editor layout, default font, tool list, welcome dialog and dark
palette remain unchanged. UI implementation is split by panel responsibility (canvas,
front door, layers, tools, verbs and theme), but this is not a visual redesign.
The optional light/dark switch uses the `ui.theme` command, with its persisted value
owned by `io/config` alongside recent files. The dark-mode viewport golden explicitly
selects `--theme dark`; GUI probes use an isolated `NOISE_CONFIG_DIR`.

The module split changes no document, AI request, recipe or pixel-write interface.
Installed asset lookup resolves the role and recipe files relative to the executable.
The browser build reuses these UI modules and its own platform-specific file handling.
The remaining product roadmap is unchanged.

## What is next, in order

Ordered by what makes the thing usable rather than by what is interesting. Each slice is demoable by a
human without a terminal, ships the wire command that makes it observable, and ends with a step of
`tools/flow_probe.py` walking [PRODUCT.md](PRODUCT.md)'s scenario. The primary flows in slices 1–4 are
landed; slice 3 still owes local Heal. Slices 5 and 6 have landed their core UI, image-input and ordered AI
reference behavior, with remaining work listed below. The selected next milestone is slice 7, document geometry.

**Slices 1 and 2 landed 2026-10-15** — files, and a new image from an idea. The file surface now includes
New, Open, Save, Save As, Export, Insert, drop, recent files and the quit guard. The New dialog's price
quote remains deliberately cut. Slice 3's coverage and verb bar are in place; local Heal remains owed.

1. **Files.** Landed: Start dialog (New / Open + recent), Open, Save, Save As, Export (png / jpg / webp),
   Insert and drag-and-drop onto a new layer, dirty title, and quit guard. Native pickers live in
   `src/platform/`; the browser picker, import and download path lives in `src/platform/*_web.cpp` and
   `web/bridge.js`.
2. **New image from an idea.** The New dialog's prompt, kind (a recipe family's `variants`) and size -
   preset ratios plus explicit pixels - and the dialog **quoting the price** the way the status line does.
   OK generates into the document's first layer. Progress, coarse or indefinite, plus Cancel.
3. **One coverage, verbs at the marked area.** Done: `coverage.target {to: marked|mask}` and `cover`
   (`K`) make one editor over the two lifetimes, and the marquee follows the same switch; the bar is on
   the canvas, holding a separate button for each verb whose recipe reads the marked area - **Heal**,
   **Remove**, **Fill**, **Change** - with the panel keeping the full controls beside it.
   Settled since: the labels stay the recipe labels (**Heal / Remove / Fill / Change**); a bar button
   that wants words opens a phrase box over the mark (`verb.ask`) instead of reading the panel;
   `un-cover` (`Shift+K`) takes coverage back; and the tool panel dropped the marked-area verbs and
   everything else another surface already says, which retired `panel_words_probe` - the bar probe now
   makes those claims with real keys. The wash over the marked area landed the same week, below.
   **Still owed**: local `heal` (in-process Telea/Chan, below), because *Heal* on a small area should
   not pay for a round trip.

   **Landed 2026-10-19, below**: coverage edits enter the history. The rule holds — *one gesture that
   changes coverage is one undo step*: marquee press to release, `cover` and `un-cover`, All / None /
   Invert, and `select.from_mask` — and a pixel write carries no coverage with it, because it did not
   move any. The other half landed in the note after it: a layer mask **appearing or going** (`mask.add`,
   `mask.remove`, `mask.from_selection` onto a layer that has none) is `Kind::mask_replaced`, so the rule
   now holds for every coverage change. A layer's own arrival and deletion landed with the note after that
   one, which also deleted the invalidation `History` used to keep; what is left outside the timeline is a
   layer's visibility, opacity and blend mode.
4. **The pointer says which tool it is.** Landed 2026-10-16, below: the host cursor takes the tool's
   shape and the canvas draws the dab's footprint, measured against the size the app reports.
5. **Layers you can see.** Landed since 2026-10-16: thumbnails of the layer and its mask, one bordered card
   per layer holding its own controls, visibility, opacity, blend mode, reorder (`Up`/`Down` on the card,
   `layer.move`), **Merge down** (`layer.merge`) and **Flatten** of the whole stack (`layer.flatten`, whose
   history entry is the replaced stack itself - see *The stack becomes one layer* below), the panel's own
   row of buttons pinned clear of the scrolling card list, a name typed on the card itself (`F2`,
   `layer.rename` - see *A layer's name is typed where the layer is shown* below), and a factor typed in the
   place of the card's `Scale` button (`layer.scale` - see *A layer's picture is resized by a number, typed
   on the card* below). What the slice owes: a gesture for that scale - a handle on the layer's picture
   driving the command that already exists. The panel is always visible: it is where a person goes to
   understand what an edit did.
6. **Blend, and images as input.** Landed: Insert/drop places an image on its own layer; no-prompt *Blend*
   uses `region: layer`, whose source is the layer's pixels and whose coverage is its alpha. *Blend* with a
   prompt and control images (openpose) remain future work through the same input path.
7. **Document geometry.** Canvas `doc.crop` and `doc.resize` **landed 2026-10-29**.
   `History::canvas_replaced` exchanges the entire document extent, layers and marked selection;
   new exposure is selected for Fill. *Image size*, which **resamples** rather than enlarges an
   empty border, is still future work: Lanczos, `AI upscale (fast)` (ESRGAN) and
   `AI upscale (best)` (an edit model with a recipe and a role).

   **Outpaint, the mechanism (landed via Fill):** Grow `Extent`, mark newly exposed coverage,
   compile the crop with the original frame as context, and land on a new mask-clipped layer.
   Existing document pixels remain untouched. The present path deliberately reuses the existing
   selection-scoped recipe; a dedicated `operation: extend` may be worth adding when its prompt
   or backend needs to behave differently, not as a prerequisite for outpainting.
8. **The tuning pass and the parked niceties.** Recipe wording and `prep.radius_px` per verb against hole
   size (specified at the end of this file, needs the box), the `enhance` prompt-improver behind its own
   button, the keymap file, autosave.

Seed is not a user-facing concept: **Retry** re-asks the same request with a different seed and lands
another layer — which is only true because every request draws one of its own (see 2026-10-23 below); with
the constant the code had before, a re-ask was a restatement. Roles and recipes are ours to pin for now - the sets are in `assets/roles.json` and
`assets/recipes/`, loaded from disk, and the plan is that a person edits those files, not a dialog, so
nothing here needs a settings UI to be foreseen later.

## 2026-10-23 - A re-ask that could not differ, and a Change that only softened

Two reported defects, one seam each, and both invisible to the recorded tier because a recording answers
whatever you send it.

**Every press sent `seed: 0`.** Two `ai.preview` calls on one mark produced byte-identical requests, so
pressing Change twice was one request twice — a second GPU charge for the picture already on the canvas.
`kSessionSeed = 0` was the default and nothing re-rolled it, and the doc claim that *Retry re-asks with a
different seed* had no code behind it. A request now takes a seed at compile time: the panel's pin, or a
draw through `AiSession::seed_draw` (`platform::random_seed` — the only place entropy enters the app, and
the seam a test replaces with a fixed sequence, since nothing a test reaches may read entropy for itself).
The number and its source (`named` / `pinned` / `random`) travel with the compile, the job and the panel:
reproducibility comes from recording the seed, not from freezing it, and a per-frame read draws nothing —
rule A forbids a read from computing an answer, and spending entropy is the same offence.

**Then the probe found the fix had a second bug.** `seed` was one argument doing two jobs, so a verb press
that carried a number *held* it: `ai.remove {seed: n}` quietly became `seed_pin`, and every ask after it
was the same request again — the defect I had set out to remove, reintroduced by the press that removed it.
The recorded tier passed, because the probe's own `ai.preview` pair happened to run before any press. Now
`seed` belongs to the request the call builds and `seed_pin` is the hold, and the probe asserts both halves
per verb: two asks draw two seeds, a named seed dies with its press, a held seed survives every ask until
it is released.

**The engine refused a wide seed, and said the wrong thing about it — twice.** The first live press after
that change answered `400 Invalid value for 'seed': must be an integer`. The first fix assumed a 32-bit
unsigned field and masked to `0xFFFFFFFF`, which fixed nothing: the box's *multipart* form parses `seed` as
a signed 32-bit integer, so the second live press failed the same way with roughly half the draws. Measured
by asking, at 256², one step: `/images/edits` takes `2147483647` and refuses `3000000000` and `4294967295`
with that message, while `/images/generations` — a JSON body — takes all of them and `0` as well. The
upload path therefore sets `ai::kMaxSeed` (`ai/provider.h`, the module that owns the field): the draw is
masked to it, a named seed above it is refused by name, and `ai_job_probe` asserts the bound on every ask.
Two lessons, both mine: an inferred bound is a guess, and a guess written into a comment with the word
"measured" in it is worse than no comment. There is no schema to read here — `/ui-schema` does not exist on
this box and `/models` carries no seed limits — so the bound has to be probed, which costs nothing because a
400 runs no model.

**Bigger asks outran the request timeout, and a live server was reported dead.** Raising the pixel ceiling
made the first job the user then ran fail as `timeout - /images/edits: timeout`. The ETA is keyed on the
exact `(model, size, steps)`, so a mark never asked before has no sample and its timeout fell to the 60 s
floor — while a masked edit at the new ceiling measures 71.1 s on this box (1024² at 8 steps masked: 33.5 s;
the same shape at 4 steps unmasked: 20.2 s, against 29.7 s at 1440x1088). The floor is now 600 s, enough for
a model load (measured 72–198 s) inside the first request, and the timeout — which is a margin, not the
promise the panel shows — falls back to scaling the model's best-sampled run by pixels × steps. The ETA
itself still refuses to guess: a shape nobody has run here has no estimate, which is what its test and its
thread-safety test both defend.

**A Change over a large mark came back as the same picture, softer.** A 1440x1080 document with a
1000x700 mark compiled `crop [0,0,1440,1080]` to `requested [1184,896], scale 0.8211`: `roles.json` capped
a region at 1 MP, a number nobody had measured, so the upload was Lanczos-downscaled and the answer
Lanczos-upscaled back over a surround that pixel-lock keeps byte-identical — softness everywhere, change
nowhere. Small marks were unaffected (their grown crop fits 1 MP), which is why it looked intermittent.
`GET /models` publishes a per-model `soft_pixel_cap` — 2097152 on `Flux-2-Klein-9B-TheNoise`, 4194304 on
`builtin.Qwen-Image-2.1-Turbo-INT8` — and nothing parsed it. The ceiling is that number when the model
states one, and the table's only as a stand-in for a model that says nothing, raised to 2 MP: the smallest
cap this box publishes, so the stand-in does not shrink answers the box would have served. A scale below 1
is now a fact in the report (`region.resampled`, plus the size it shrank to in the landing log) rather than
something a person infers by squinting.

**Two of my own fixes were wrong, and only measuring caught them.** Both were in the rescale, and both
survived a green hermetic suite because the tests asserted the *shape* of the answer rather than the
promise it carries.

- The shrunken ask was snapped *up* to the size multiple, which put it back over the ceiling it had just
  been shrunk for: a 2400x1600 mark under a 2097152 cap asked `1776x1184` — 2102784 pixels. The grid is
  snapped *inside* the ceiling now (down when a shrink is in force, up when there was room, never below
  one multiple), and `test_region` sweeps 1040 shapes against the invariant `asked ≤ cap`, because the
  failure needs a particular remainder and the two assertions that used to cover it allowed a factor of
  two and a slack term.
- `resampled` never reached the sentence it was supposed to produce. The landing rebuilds its plan from
  the job record — `plan_of_landing` copies the crop, the asked size and the mask bounds — and a plan
  rebuilt that way had `resampled == false`, so the automatic landing, which is the path answers actually
  take, logged a rescaled answer as a plain one. It is a fact the compile decides, carried on `Landing`
  and restored with the rectangles, which is what the live line now reads: `rescaled to 1760x1168 for the
  model's pixel ceiling, answered at a different size than requested`.

The general shape: a derived flag travelling beside the numbers it was derived from is a second home, and
it goes stale the moment one of them is rebuilt from storage. Either carry it or recompute it from the
same bytes the compositor maps by — do not leave both half-done.

What the box added for free: the engine snaps the size it *answers* (`1760x1168` asked, `1760x1184`
answered), so `answered_as_requested` is a separate fact from `resampled` and the landing keeps saying
them apart. Pixel-lock held byte-exact outside the coverage in every case above, at 1:1 and both ways
around a rescale.

**What the tests talked me out of.** The first cut asked for the crop's exact size, which is what "output
size equals input size" sounds like. Two `test_region` assertions went red for a reason: the 16-grid exists
because the engine snaps an off-grid ask itself (measured: asking 500x300 answers 512x304), so an off-grid
ask trades a snap this side planned for one the engine invented, and the aspect-frame contract — a 16:9
frame arrives 16:9 — depends on `requested` being on the grid. The grid costs at most 15 pixels; the
invented ceiling cost 18%. The ceiling was the defect, not the grid.

Also settled, from a claim in an older session that a `style` request field was half-built: it is not
needed and it would have been a field-policy violation. `style` is a recipe *variable*
(`assets/recipes/change.local.json`) fed by the panel's style dropdown, and `style` stays among the fields
never sent ([ARCHITECTURE.md](ARCHITECTURE.md) §field policy).

Hermetic: 40/40 green (including `ai_job_probe`, which now asserts the seed and resample claims per
verb), `sweep_procs.py` clean. **Owed, not yet spent:** live verification of both fixes —
two asks over one mark differing in the answer, and a full-HD Change landing 1:1 sharp — four images on
the dev box, waiting on a yes.

## 2026-10-19 - The live tier ran for the first time, and it found two things the recordings cannot

`$THENOISE_URL` / `$THENOISE_API_KEY` are exported, so `cmake -B build -DNOISE_LIVE_TESTS=ON` and
`ctest -L live` are available. First run: `tn_probe_live` passed and taught the box's contract again
(`2026.42.1`, one model per class, explicit size honoured, 512² in 25.6 s cold and 1024² in 8.6 s warm);
`ai_job_probe_live` and its `generate` sibling **failed**, and both failures were real.

**The expensive refusal was winning the cheap one.** A bare *Generate* with no words answered
`'new-image' needs Flux-2-Klein-9B-TheNoise and the server is running Flux-…` — a model-swap
confirmation, for a request that could never have been built. In the recordings the resident model always
matches the role, so the swap branch is never reached and the blank-field refusal inside `build_prompt`
was the only one anyone ever saw. `start_intent` now renders the prompt *before* pricing a swap: a
request with no words in it must not ask a shared box to load 32 GB first, and `confirm` cannot buy its
way past a blank field either. The hermetic test is the ordering itself — swap pending *and*
`instruction` blank → the error names `'instruction'` and does **not** contain `confirm` — because that
is the half the fixtures are structurally unable to show.

**A live probe that refuses is a green run of nothing.** Every verb then went through the same swap
refusal, which is a guard for a hand on a key, not for a driver that intends the work: the probe now
loads the model the role resolves to, says it did (`Flux-2-Klein-4B-TheNoise → Qwen-Image-2.1-INT8`), and
the existing `finally` puts back what was resident (`the model the box was running is back`). After that
the remove cell passed against a real answer: 2 images, the answer landed itself as `Remove j0001`,
`0 pixels beyond [134, 88, 152, 136]`, colour ratio 1.04 against the band, and undo byte-for-byte.

**What was not a bug, and what both of them were.** The first run resolved `edit` to
`Flux-2-Klein-9B-TheNoise`, the second to `Qwen-Image-2.1-INT8`, and the obvious reading — a resolution
that moves on its own — was wrong: `assets/roles.json` was edited in the working tree *between the two
runs*, by a hand that is not mine, and `git status` still carries it uncommitted. The preference list
moved, so the answer moved. Two things follow. The live tier reads `assets/roles.json` from the working
tree, so on a shared box a run's meaning can change under it while it is in flight — which is why the
`ai_job_probe` output prints the resolved model rather than assuming it, and why that print stayed. And
`tools/tn_probe.py` answers "which model?" with its own "smallest edit-capable" rule instead of asking
the registry, so the same question has two answers in this repo; the probe's is a cost rule about the
box and the registry's is the app's assignment, and anyone reading one report needs to know whose answer
they are looking at.

Not fixed here: the duplicated rule in `tn_probe.py`, because the probe deliberately spends the cheap
model while probing a shared box, and making it agree with the registry would make it swap models to
probe a contract. The fix is to label its answer, which is a docs-and-print change, not a rule change.

## 2026-10-19 - A layer's arrival and departure are steps, and the timeline stops invalidating things

`layer.remove` used to call `History::drop_layer`, and `layer.add` recorded nothing at all. Both are steps
now: the departure files the **whole** layer under the entry `layer.merge` already had, and the arrival files
one `layer_created` - nothing beside it, for the reason below.

**What a step that takes a layer away actually carries.** The header said a creation "comes back empty and
gets its pixels from the pixel entries of the same step", so the first version of this change recorded the
`fill` beside the arrival — a whole canvas of bytes per Add, and wrong twice over. Undoing a creation moves
the **live layer** into the entry, and `record_layer_removed` files one the same way, so every
`layer_absent` re-inserts the layer it was handed: `layer.add` with `fill: [9,8,7,255]` undone and redone
returns the opaque layer with nothing recorded but the arrival. `add_layer_written` still records the answer
beside its arrival, because an answer is a *write* whose `before` has to exist for the step to be undone in
order — not because the shell needs filling. The command-level test reads the bytes back after the redo,
because "the layer came back" is a claim an empty shell would also satisfy, and it is that test which said
the comment was wrong rather than the code.

What fell out of it: `step_structure`'s empty-shell branch, `Stacked::name` and `record_layer_created`'s
`name` argument. Every entry that brings a layer back carries the layer, so a name stored beside it was a
second representation of a fact already in the thing — and a branch that no road reaches is not a safety
net, it is a place where a wrong layer can be manufactured quietly.

**What recording the departure made unnecessary.** `History::drop_layer`, `drop_entries`, `drop_where` and
`names_layer` are **gone** — four functions and the rule they carried, that a step naming a layer which is
not there must be thrown away with the unrelated steps around it. They were only ever right because a
departure was irreversible. Recorded, a departure is a step *above* the layer's own paint, so undoing it
first hands that paint a layer to land on: `test_deleting_a_layer_is_one_step_that_gives_it_back_whole`
walks five steps back and five forward through a deletion and lands every one of them, and the panel probe
clicks `Delete layer`, presses `Ctrl+Z`, and finds the card in the same pixels holding the same picture —
the thumbnail is keyed to the layer's id, which the deletion released and the undo re-used.

`step_back`'s validation stays, and its meaning changed: it is no longer a workflow (`delete a layer, then
try to undo its paint`) but the fence under the timeline — a layer that went missing *without* a recorded
departure drops the history whole rather than writing bytes into a layer that is not there. Same for a
plane of another canvas.

**Half a rule is worse than none, which is why `layer.add` is in this change and not a later one.** With
the departure recorded and the arrival not, `Ctrl+Z` after Add would silently undo the *previous* edit —
the surprise is worse than the missing step, and it is the same surprise the coverage rule was written to
remove.

**Observed, first run with a display.** `Xvfb` is installed, so the whole suite ran including the
app-over-the-wire tier: **40 tests, 0 failed, 0 skipped** (322 s), and the two coverage commits landed
earlier today passed over the wire for the first time. The panel probe's new claim was itself wrong on its
first run and taught something: a card's `picture` box is clipped by the panel's viewport, so a thumbnail
grabbed after the wheel moved the list is a *different region* (44×29 against 44×25) and comparing the two
pixels is comparing nothing. The claim now pins the reported box before and after, so the measurement fails
loudly if the layout moved instead of quietly comparing two different pictures.

**Owed from here**: nothing in the history rule except a layer's visibility, opacity and blend mode — and
that is a decision about whether those belong in a timeline at all, not a missing entry. The next slice is
6, *Blend*, whose seam is `region: layer`.

## 2026-10-19 - A mask plane is a step, and the coverage rule has no exception left

The note below left one hole: `mask.add`, `mask.remove` and `mask.from_selection` onto a bare layer moved
coverage and entered nothing. They are in the timeline now as `Kind::mask_replaced`, which holds the layer's
mask that is *not* current — `nullopt` being one of the two states it carries — and applies it by swapping,
the third entry built that way after `stack_replaced` and `coverage`. The rule it was checked against is
not "does undo move the pixels back" but "does undo bring back **the** mask, with the coverage it was
hiding": `mask.add` over a layer whose mask had a corner hidden, then undo, returns that corner, which an
entry recording "a mask exists" cannot promise.

**Two things the entry does not need, and one it must not lose.** It wants no `layer_created`/
`layer_absent` twin: those two exist because a *layer* has a position in a stack and the frames of a
removal and an insert disagree (see the flatten note). A plane has no position — it is a field of one
layer — so one kind swapping `optional<Mask>` covers both directions and the pair would be two encodings
of one fact. And `History::drop_mask` is **gone**, not kept: it existed because a plane that could only go
forever made every coverage step underneath it unrestorable. Once the removal is a step, those steps are
reachable again by undoing the removal first, and `test_taking_the_mask_off_ends_the_gesture_and_is_itself_undoable`
walks the removal, the colour step and the plane's arrival in one timeline to show nothing was thrown away.

**What it catches by construction.** `fits` asks two questions of a plane entry — the layer is there, and
the plane is the size of that layer's picture — and `step_back` validates in the same order it applies, so
a mask of another canvas clears the timeline instead of draping a layer with coverage that does not fit it.
The interaction that was worth the walk is the one between kinds: `layer.merge` puts a layer back and the
step *underneath* it names that layer's mask, so undo hands the mask off to a layer the merge had just
re-inserted, and redo takes it back off. `test_merging_down_shows_what_the_canvas_already_showed` and
`test_flattening_leaves_one_layer_that_shows_what_the_stack_showed` now walk through it, both directions.

**`Applied::structure` is the right flag here, and a second flag would not have been.** A mask change moves
no pixel of any layer, but it changes what the composite shows everywhere the layer reaches — which is what
`structure` already means, and what makes `Document::undo` widen the pending region to the whole canvas. The
kinds that need saying the other way round - a mark moving, a name going back - are read from the absence:
`structure` false with an empty `rect`, so nothing has to be kept in step as entries are added.

**What is left outside the timeline is now one thing, not a family.** A layer's arrival and deletion landed
in the note above this one; visibility, opacity and blend mode are the remainder — composite state the stack
carries, which undo restores only as part of a layer that arrives, goes or is replaced. ARCHITECTURE §Undo
states the boundary that way: the *coverage* rule and the *stack* rule are closed, the properties are the
open half.

**On this box**: as the note below — no `Xvfb`, no root, so the wire tier skipped and `wire_input`'s
`mask.from_selection` claim was rewritten to the new rule without being run. `ctest --test-dir build`: 40
tests, 0 failed, 15 skipped.

## 2026-10-19 - One gesture that changes coverage is one undo step

The marked area is a document property — it is saved with the project — and it had no undo. It does now.
`Kind::coverage` in `core/history` holds the coverage that is **not** current and applying it *swaps* it,
exactly as a replaced stack does, and `History::undo` is handed the document's plane next to the layer
stack. `Document::set_selection` is the one write door: outside a gesture it records what it replaces,
inside one the gesture owns the step. `begin_marked` / `repaint_marked` / `commit_marked` /
`cancel_marked` are the bracket the marquee and `cover` press open and shut around the coverage the press
found.

**What the change found, rather than what it needed.** Four things, all in the drag path:

* A marquee aimed at a *layer mask* called `paint_mask` once per pointer event, so a 20-event drag pushed
  20 undo steps and repainted the canvas with each. It runs through the layer's provisional door now —
  the one the `cover` stroke has always used — so the drag is one step committed at release.
* The same marquee wrote its shape into the live mask instead of drawing it onto what the press found, so
  a drag that changed its mind left the earlier box's coverage behind: 256 px marked where the person
  described 48. Both lifetimes now replace the coverage they are aimed at, from the held bytes.
* `cover` aimed at the marked area stamped its accumulated scratch onto the *current* selection every
  event, so the coat compounded and a held pointer darkened the dab once per frame. It restores what the
  press found inside the event's rectangle first, which is what the colour path does; the new test pins
  it — three overlapping events equal one coat, byte for byte.
* A marquee took its lifetime from `coverage.target` on every event *and* on release, so a target switch
  mid-drag wrote the box into one door and shut the other: the marked gesture was never closed, and from
  then on every selection command wrote coverage without recording it. `PointerSelection` now carries the
  lifetime the press opened, as it already carried the shape, and `stroke.marquee_coverage` says which one
  a running drag holds.

**The step moves coverage and nothing else**, which is a fact the renderer needs: `structure` is not set and
the entry's `rect` is empty, so `Document::undo` neither widens the pending region nor calls
`changed_all_layers`. Bumping the stack's revision for a mark would re-upload a texture nobody repainted
and resample every layer card — invisible, and a per-frame cost — so the tests hold `revision` and every
layer's `content_revision` unchanged across a mark's undo and redo.

**A marked gesture and a layer gesture are different presses**: `begin_marked` refuses when a provisional
edit already holds a layer's plane and `begin_provisional` refuses the other way round, naming what is
held. One gesture, one set of held-aside bytes; the alternative is a commit that restores neither.

**Owed, and landed the same day below.** A mask appearing or going was still coverage that could not be
undone, because the entry it wants is the pair restated for a *plane*. That is the next note.

**What it changes for the next slice.** Slice 6 gives a recipe a third scope, `region: layer`, whose
coverage is the layer's own alpha. Coverage that a person *edits* is now undoable per gesture; coverage
the layer *has* by being painted is the same plane, so the seam to watch is a verb whose answer arrives as
a layer whose alpha is the coverage: landing it writes pixels and no coverage step, and the rule that a
step prices only what it touched is what keeps that landing from snapshotting the mark along with it.

**On this box**: no `Xvfb` and no root, so the app-over-the-wire tier *skips* and none of this was seen
through a window — the claims are unit and command-table level (`test_document`, `test_history`,
`test_app`), and `coverage_view_probe`, `verb_bar_probe`, `layer_panel_probe` and `wire_input` have not
run since. `ctest --test-dir build`: 40 tests, 0 failed, 15 skipped. Getting there needed the harness
fixed first — see the note below on `prep_proof` and `coverage_view_probe` aborting instead of skipping.

## 2026-11-01 - Separate the AI selection from layer-mask painting

The earlier `coverage.target` design (described in the historical milestones below) has been
replaced. It made Cover/Un-cover unexpectedly mutate a result mask and required a manual switch to
mark the next edit. The single source of truth is now the selected layer's `LayerPlane` (`layer.plane`),
which affects **only Brush/Eraser**. Cover, Un-cover and selection gestures always edit the transient,
blue AI selection. Clicking the image or mask thumbnail chooses pixel or mask painting; a selected
mask's hidden region is shown in translucent red and can be toggled off. The same Brush/Eraser
controls reveal/hide the result without applying or committing it.

After a successful AI edit, the new result layer's mask is selected for immediate adjustment.
`verb.new_edit` selects Cover and the image plane as a convenience, **not** a prerequisite for running
another AI operation on a marked selection. The bar remains reachable over the result; selection
verbs are available whenever a valid mark exists. Tool targeting, result handoff, the two overlay
colours and the thumbnail hitboxes are observable on the debug wire; the mask and pixel editing
routes retain one undo step per stroke. Regression tests and probe scripts now use `layer.plane`.

## 2026-10-16 - The stack becomes one layer, and the history entry that can say so

`layer.flatten`, with a `Flatten` button on the panel's own row: PRODUCT's beat 11, *"I am lost in layers"*.
The layer holds `Document::composite()`, so the canvas does not move - and that claim is exact rather than
bounded, unlike merge-down's one-step rounding, because nothing is re-blended: 0 px of 447 700 in the probe.
It is named `Background` and comes out neutral (visible, opacity 1, `normal`, no mask) because with those
the composite *is* the layer; give it the active layer's 0.35 and the canvas quietly halves. A stack of one
refuses, and refusing records no step. On the wire it is `layer.flatten`, and `app.state.layer_panel`
reports the box of each of the panel's three buttons and whether Flatten can apply.

**The obvious encoding put the stack back reversed, and only the second time.** One `layer_created` plus a
`layer_absent` per removed layer looked right, and the first undo did give all four layers back. Redo, then
undo again, gave `ghost, haze, bird, sky`. The index is the whole of it: a removal records the position it
saw in a stack that is **shrinking** (once `sky` is gone, `bird` is at 0), and the insert pass reads that
number as a position in a stack that is **growing**. With one removal per step the two frames coincide,
which is why `layer.merge` was never wrong and why this needed four layers to show. Making the frames agree
in general means inventing an ordering rule for the indices within a step, to describe a case that is not
"layers moved about" but "the stack was replaced".

**So a replaced stack is one entry that holds the stack.** `Kind::stack_replaced` carries the layers that
are not current and applying it *swaps* them: undo and redo are the same operation, no position is asserted
anywhere, and the entry says what the command did. It is the only entry that names no layer, which is why
`drop_layer` skips it - a layer deleted after the swap belongs to a later step, undone before this one is
reached - and it carries every pixel of both sides, which is why such a step records nothing else. It is
also the whole answer to the properties question the note below raises: name, opacity, mode and mask
come back because the layers themselves do.

**The budget is where it actually hurt.** A structural entry's cost was read *after* `step_structure` had
moved it into the other stack - zero for the kinds whose payload had just been moved out, and wrong for a
swap, whose other side is the entire live stack. `bytes_` underflowed to 1.8e19 and
`drop_oldest_until_within_budget` did what it is told: dropped every step, oldest first, leaving `Ctrl+Z`
dead in a document that looked untouched and saying nothing. The price is read where it landed
(`held(into.back())`), and `test_history` walks a flatten three times holding the byte count, the ids and
the depth on each lap.

**A control below the cards is below the panel's fold.** The panel's own row (`Add layer`, `Delete layer`,
`Flatten`) and the mask section are submitted after the cards *in the panel's own scroll region*, so on three
layers that row starts past the window's bottom edge - `app.state` reported it at y=926 in a window ending
at 800: drawn nowhere, unreachable until scrolled. The probe wheels to it and then presses where the app
says it is, the rule the last card is already held to. The next note is the decision: the row is pinned.

**Owed from here**: moving and scaling a layer's picture, inline rename, and `layer.remove` in the timeline
(`layer_absent` is the entry deletion always needed; the decision is whether deleting belongs in the
timeline at all). The coverage-undo decision further up is still the bigger of the two.

## 2026-10-17 - A layer's name is typed where the layer is shown

Two things slice 5 owed that turned out to be about the panel rather than the pixels: the row of
stack-wide controls moved out of the scroll, and a layer's name became something a person types.

**The stack's own buttons are pinned, not scrolled.** The cards now live in a `layer-list` child sized to
leave room under it, and `Add layer` / `Delete layer` / `Flatten` are submitted after that child, so the
buttons that act on the whole stack are where they always are instead of at the bottom of a scroll. What it
bought in observability is the important half: `app.state.layer_panel.list` is the scroll region, so a probe
wheels *inside the list* rather than over the buttons, and the claims are that three cards deep the row is
already on screen, that the three buttons claim no pixel twice, and that every card sits inside the list.

**A name is document data that moves no pixel.** `F2`, or a double-click on the card, puts a field in the
card's title row; `Enter` commits it through `layer.rename`, `Escape` throws it away. The card says which
state it is in (`layer_cards[].renaming`), and the host's existing shortcut gate is what makes `Escape` the
field's rather than the app's quit key - which the probe checks with a real key, because on the wire the
question does not exist. A rename marks the document dirty and enters `checksum()` - it is saved, so it is
data - while bumping neither the document revision nor any layer's `content_revision`: the probe asserts 0
canvas px moved and 24 thumbnails still 24 resamples.

**`Kind::layer_renamed` holds the name that is not current**, and applying it swaps: the same shape as the
stack swap above, one field instead of a stack, and the only property of a layer with a step of its own.
Hiding, fading or re-modding a layer stays out of the timeline deliberately - those are states checked by
looking, not mistakes undone.

**What a layer may be called is one fact in one file.** The rule was being restated in three places: the
document trimmed and capped, `layer.rename` re-typed the same sentence in its error (with the limit spelled
out a second time), and the container reader accepted whatever the file said - a 4 kB name or none. They are
`normalize_layer_name` and `layer_name_rule` in `core/layer`, asked by all three doors. The reader now
refuses a name a layer cannot carry, quoting the row and the offending text and listing the limit, and
defaults only where the file has no `name` at all, which is a different fact rather than the same failure.

**A double-click cannot be driven on this box, and a probe that claimed to would be lying.** Measured: two
injected presses reach ImGui ~0.5 s apart (presses 200 frames apart) however the driver spaces them - the
app renders ~340 fps idle and a wire call costs 3 ms, so it is XTEST-to-app delivery, not the app being
slow. ImGui counts a pair inside `io.MouseDoubleClickTime` (0.3 s), so every variant of the burst failed the
same way. `layer.rename.begin` is the door the key and the double-click share, so the probe drives that and
`F2`, and the double-click stays for the hand that produces it - unclaimed, because the claim would not be
true of the thing the probe did.

**Verified**: `test_document` 3760 checks, `test_history` 254, `test_project` 216 (the new reader cases are
13 of them), `ctest` 39/39, `tools/layer_panel_probe.py` 65 claims green, `sweep_procs.py` clean.

**Owed from here**: `layer.remove` in the timeline (`layer_absent` is the entry deletion always needed;
the decision is whether deleting belongs in the timeline at all), and the coverage-undo decision further
up, which is still the bigger of the two.

## 2026-10-17 - A layer's picture is resized by a number, typed on the card

The half of slice 5's "move or scale a layer's picture" that was missing was scaling: the move tool had
long since been dragging a layer's *content* around, because `src/app/tools/move.h` insists that whole-pixel
movement and resampling are different features with different undo semantics. Scaling is now a command with
those semantics of its own, and a field on the card to type it into.

**What is scaled is the picture, and the picture is `Image::content_bounds`.** A layer stays the size of the
canvas - every blend, mask, thumbnail and `Document::checksum` assumes a stack of congruent rectangles - so
scaling means resampling what the layer *holds* and leaving the rest transparent. `content_bounds` used to be
a static inside the move tool; it is on `Image` now, so a drag and a scale cannot disagree about which part
of a layer "it" is. The anchor is the middle of those bounds, not the canvas origin and not the layer's
top-left: on the app a stroke spanning x 68..168 halved to x 93..143, middle 118 before and after.

**The coverage plane goes through the same map, because it is a claim about the same pixels.** Scale the
colour and not the mask and the layer shows parts it was hiding and hides parts it was showing. So both
planes take one factor, about one anchor, in one history step - and they take it through `core/resample`
twice rather than through a filter twice: a coverage plane has no colour to premultiply and must never be
renormalised by weights that sum to less than one, or an in-outside boundary starts claiming coverage it was
asserting the opposite of (`resample_coverage_lanczos3`, clamped at the edge, never renormalised). Two cases
are kept out of the filter on purpose: a plane that covers everything is the same plane at any size, so
filtering it would spend a pass putting ringing where there was none; and a plane that reaches a canvas edge
*limits how far the picture may grow*, which is why the fit test is taken over the union of the two bounds.

**A factor that will not fit is refused with the largest one that will**, because someone who meant "bigger"
wants that number rather than a shrug: `LayerScale::max_factor` comes back on the refusal, in the command's
error and in the log, and the refusal writes nothing - no pixel, no revision, no step. The same is true of a
layer that holds nothing and of a factor that is not a finite positive number, and `factor == 1` is refused
as the no-op it is rather than recorded as a change. The probe's own instance: a layer whose stroke spans the
canvas is resized at 1.00147 at best, so `layer.scale {factor: 2}` on it says so.

**A factor is a number, so the door is a field, and it stands where the button was.** The card's `Scale`
button opens it in the button's own box (`layer_cards[].scale`, with `scaling` saying which of the two the
box currently is) so a card cannot grow wider than the panel; `Enter` sends `layer.scale`, `Escape` puts the
button back, and nothing reaches the document except through the command - so a half-typed factor cannot move
a pixel, cost a step, or be saved.

**Two ImGui facts that cost the debugging time, both of them found by looking at the canvas.** A single-line
`InputText` gives up focus when Enter is pressed whether or not `EnterReturnsTrue` is set, so a refused value
closed the field - and the next key a person reaches for after a refusal is `Escape`, which with no field
holding the keyboard is the app's quit key. Its save prompt then dimmed the canvas, and the probe's next four
claims were about a dialog rather than a scale. The field now re-arms itself on a refusal; and because
`io.WantCaptureKeyboard` is one frame stale on the frame a field is re-armed on, `main.cpp`'s shortcut gate
asks `typing_in_layer_field(state)` next to ImGui's flag, since either one alone lets the quit key through.
The observation that gave this away was a canvas diff with a bbox covering the whole image, which is what
`pixels.compare` and a capture are for: an assertion about the field alone would have gone green.

**Verified**: `test_document` 3815 checks, `test_resample` 766 (the coverage resampler's own invariants:
1:1 byte-identical, all-in stays all-in, all-out stays all-out, area held within the filter's tolerance),
`test_history` 254, `ctest` 39/39, `tools/layer_panel_probe.py` 79 claims green - including that a word where
a factor belongs is not sent and the field keeps it, that `Escape` was the field's key and not the app's,
that half the factor visibly shrinks the picture and costs one thumbnail resample (27 to 28), that one undo
gives the canvas back byte for byte, and that a refusal moves nothing.
`sweep_procs.py` clean.

**Owed from here**: a gesture for the same command - a handle on the selected layer's picture driving
`layer.scale`, which is what the command existing on its own was for - `layer.remove` in the timeline, and
the coverage-undo decision further up.

## 2026-10-16 - Moving a layer, and what a change to the stack actually carries

A card holds the stack's own controls now: `Up` and `Down` on its title row, `Merge down` beside its
pictures. Both are commands before they are buttons — `layer.move {index,to}`, `layer.merge {index}` — and
both go through the same `Document` calls a click makes.

**A reorder is one history entry holding no pixels.** `Kind::layer_order` carries the stack's ids in order;
applying it puts the layers into that order and keeps the order it displaced, which is the same "an entry
holds what is not current" rule a rectangle of pixels follows. A merge is the other direction of the same
problem — a layer leaves the stack — so `layer_absent` carries the **whole** `Layer` rather than a name and
a size, and undo gives back a layer with its pixels, mask, mode and opacity instead of an empty one wearing
the right name. It is also the entry `remove_layer` never had: deleting a layer is now one call from being
undoable the same way, and is not, because whether *that* belongs to the timeline is a separate decision.

**Merge-down is canvas-invariant to within one step**, measured, not asserted: 12 px of 447 700 moved, each
by exactly one step of 255, where the two strokes' soft edges meet. That is arithmetic rather than a bug —
`blend_layer` rounds every channel to 8 bits, so compositing the pair once is not bit-identical to
compositing each layer once — so the probe asserts a bound (no pixel by more than one step, fewer than one
in a thousand moved) where a merge that dropped the mask, the mode or the opacity would move thousands by
tens.

**A thumbnail is a picture *of a layer*, so its cache key is the layer.** Cards were indexed by stack
position, which forced two fictions: a row could only notice it now held a different layer by watching a
counter, so `content_revision` had to move whenever any layer arrived, went or moved, and
`Document::changed_stack` existed to bump every one at once. Reorder made the fiction visible — a card went
on drawing the layer that had sat in that row, because two layers can hold the same revision number, and
only the card *below the fold* was checked, so the probe had to reach it with the wheel to see it. So
`LayerThumbs` is a `map` keyed by layer id, `keep(layers)` releases the pictures of layers that have gone
(and releases the textures, which `card = Card{}` used to leak), and the three kinds of change are named for
what they are: `changed_composite` (what the canvas shows), `changed_layer` (one layer's content),
`changed_all_layers` (undo and redo, whose steps report rectangles rather than layers). A reorder therefore
resamples nothing, which `app.state.layer_thumbs.rebuilds` says as a number and the probe reads: unchanged
across an opacity drag and across a reorder, and up the moment a stroke writes.

**Every control of a card reports its box, and the probe checks them against each other** — the invisible
failure mode is in *Traps* below, and it is what made `Up` look dead for an afternoon. The title row now
reserves the arrows' measured width instead of overlaying them, the blend dropdown measures what is left
inside the card rather than a slice of the panel (its arrow sat past the border), and controls that cannot
apply go through `BeginDisabled` rather than a hand-picked alpha plus a guard that threw the click away.

**Owed, and why it is owed**: `layer.flatten`. Merging two layers keeps the survivor's identity, so the
history entry only has to restore *pixels*; flattening replaces the stack, so the survivor's name, opacity,
mode and mask change with it too. What landed removed the question rather than answering it as posed — see
*The stack becomes one layer* above. Moving and scaling a layer's picture, and inline rename, are the rest
of the slice.

## 2026-10-16 - A surface per layer, and its own picture on it

The panel put a checkbox, a name, a slider and a dropdown in a row and moved on to the next layer, so
which layer any of them belonged to was a guess from proximity. Each layer is now a bordered card - its
own visibility, name, colour thumbnail, mask thumbnail, opacity and blend mode inside one frame, the
active card's border in the accent, so ownership is a fact about the frame rather than about where the eye
landed.

The thumbnails needed a cache key and the document had one revision for the whole stack. So
`Layer::content_revision`: bumped by every write to that layer's pixels or mask, and by any change to the
stack - deliberately **not** by visibility, opacity or blend mode, because those change what the composite
is made of and not what the layer holds, and a slider that invalidated every card would resample the whole
stack on every frame it is dragged. `Document` funnels every change through `changed_stack` /
`changed_layer` / `changed_composite`, so the two facts cannot drift apart, and `test_document` holds both
directions: a counter that misses a write shows pixels that are not there, and one that over-fires is a
cliff nobody notices until someone opens a 2048² document.

A stroke moves the counter on every dab, so a card is rebuilt when the hand lifts (`live` in
`LayerThumbs::sync`) rather than per dab. Sampling is a box average in **premultiplied** space with the
backdrop composited in afterwards: averaging unpremultiplied colours drags every semi-transparent edge
towards whatever happened to be under it.

`app.state.layer_cards` reports each card's box, its checkbox's box and where its two pictures were drawn,
so `layer_panel_probe` presses and measures what the app reports instead of remembering a layout. It checks
the three things a checkbox cannot: that a picture is the layer's own content (two layers' thumbnails
differ), that an **opacity change leaves it byte-identical** - the caching rule, proven with pixels - and
that painting into the layer does change it.

## 2026-10-16 - Coverage you can see

Marked coverage was a boundary and nothing else: `cover` and `un-cover` looked identical while you did
them, and a soft edge from a brush was invisible until something was painted into it. Coverage now draws
twice - the ants say where the edge is, the wash says how much of the area is claimed - with
`view.coverage {mode: ants|overlay|both}` choosing, defaulting to `both`, reported in `app.state.view`.

The wash is one RGBA texture whose **alpha is the coverage byte**, so a half-covered edge washes half as
hard, and it is sized to the coverage's own bounds rather than to the canvas. `gfx::Texture::sync`
recreates when the revision moves, so the cache key is a stamp: FNV over the covered box's bytes, with
the box in the seed, one pass, and an upload only when they differ. That is what keeps a live `cover`
stroke inside budget - the pass is microseconds at these sizes, an upload per dab would be a megabyte.

`coverage_view_probe` is the claim only a capture can make, and it earned its keep: **108 676 of
109 350 px changed inside the mark, 0 px outside it**. Its first run failed for its own reason - it was
comparing framebuffer coordinates against capture-local pixels, the mistake the ant probe was already
written not to make - and the failure was worth having, because "0 outside" is the half that says the
wash is coverage rather than a haze over the viewport.

Owed here: the wash follows the *marked area* only. With `coverage.target` on `mask` the coverage being
painted is a layer mask, which the composite already half-shows; the question to settle later is whether
a mask being painted should wash in a different hue, which is a second `CoverageTint` instance and not a
new mechanism.

## 2026-10-16 - The pointer says which tool it is

The cursor was the desktop arrow whatever was selected, so a brush's width was unreadable and `cover` was
indistinguishable from the tool that moves layers. Two mechanisms, because the two halves need different
ones.

**Shape** comes from the host: `sapp_set_mouse_cursor` has a crosshair (`XC_crosshair` on X11,
`crosshairCursor` on macOS) and a four-way move glyph, and the tool's row in `kTools` names its
`PointerShape`, so a new tool does not build until it names a cursor. **ImGui cannot ask for this**: its
cursor enum has no crosshair, and `sokol_imgui` applies whatever ImGui wants at `simgui_new_frame` -
*before* the canvas has decided anything. So the UI records the frame's decision in `AppState::pointer`
and `main.cpp` sets the cursor after `draw_ui`, which is the frame's last word. Only over the canvas:
off it, the host is left alone and a panel's text field keeps its I-beam, a button its hand.

**Size** cannot come from the host at all. A dab's footprint moves with the brush, the zoom and the
hardness, so the canvas draws it: `brush.size * view.scale` (`Brush::size` is a diameter in document
pixels, so the zoom enters in exactly one place), a light ring at the rim and a dark circle *inside* it -
inside, because the rim is what a person aims with and the overlay's outer edge has to be that rim. The
OS cursor stands down only while that ring is on screen; with the ring forced off a paint tool gets a
crosshair instead, because a hidden cursor and no ring is a tool you cannot see at all. Hiding it is the
app's call and **re-showing it is too**: sokol_imgui re-applies ImGui's shape each frame and never touches
visibility, so the host states `sapp_show_mouse` on every frame rather than only when the ring appears -
the first version left the cursor invisible over the whole window the moment a brush had been under it.

`pointer.footprint {show: true|false|null}` exists because a wire client has no pointer: forcing is how a
capture gets the ring where no hand is, and how it proves the tool alone would not have drawn it.
`app.state.pointer` reports `shape`, `cursor`, `over_canvas`, `at`, `footprint_px`, `core_px` and
`forced`, so a report can name what a frame drew.

`pointer_probe` measures the ring instead of trusting that number: **45.3 px against 43.2 px expected at
2.16x, and 166.9 against 164.8 with a wider brush** - the same +2 px at both sizes, which is the one-pixel
line straddling the rim plus the antialiasing that keeps it visible, and not drift. Its first version
failed at +4 px, because the dark outline was drawn *outside* the rim, which put the overlay's outer edge
two pixels past the size it was claiming; that is what the measurement was for.

*Trap*: `AppState` is `noise::AppState`, not `noise::app::AppState`. A forward declaration of it inside
`noise::app` - the obvious thing to write in a new app-level header - shadows the real type in every
`noise::app` translation unit, and the errors are "invalid use of incomplete type" in files that compiled
fine a minute before.



| seam | what it guarantees | how it is seen |
|---|---|---|
| `ai/region` | compile → inverse-map is exact for off-canvas and tiny crops; output dims always satisfy `size_policy`; prep claims (`keep`/`destroy`) measured, not asserted; `frame_for_aspect` gives the largest centred rectangle of a ratio with one axis full, and leaves the 16 px grid to `plan_region` | `test_region` |
| `core/blend` | one separable per-channel rule; CPU reference == GPU numbers over a golden pixel set | `test_blend`, `tests/golden/` |
| `ai/compose` | **pixel-lock**: every byte outside the feathered coverage is identical to the source; paired colour drift (context band vs document band, pixel against pixel) taken off the whole answer | `test_compose`, `ai_compose_probe`, live drift |
| `ai/provider` | the field policy is the only place a field can be added; tier-2 and unknown fields refused by name; `image_url` / `mask_url` are never sent | `test_provider` |
| `ai/queue` | one worker, cancellable, in-memory; a job's timeout is predicted when it is queued so it cannot change under it; **a re-ask is another request** | `test_backend` |
| `ai/server` | latency EMA per `(model, w×h, steps)` → ETA and `timeout_request`; a reply inside 1 s is the box's own answer and never a sample | `test_backend`, `docs/probes/` |
| `app::kVerbs` + `ai.verbs` | five verbs bound to recipes; a row whose recipe contradicts it — wrong operation, an `ask` that grew, two verbs on one recipe — makes the row not-ready at session open | `test_app`, `ai.verbs` |
| `app/layer_commands.cpp` (`layer.plane`) | the image/mask thumbnails select which layer plane Brush/Eraser paint; Cover/Un-cover and marquee/lasso never switch away from the document selection; mask paint is independent of the selection fence | `test_app`, `wire_input`, `mask_speed_probe` |
| `Document::set_layer_mask` + `Kind::mask_replaced` | a layer's mask plane has one write door: putting one on, taking one off and replacing one are each one step that *swaps* the plane, so the coverage the layer was hidden by comes back rather than a fresh mask, the writes underneath it stay reachable, and `History` needs no `drop_mask` | `test_document`, `test_history`, `test_app` |
| `Document::set_selection` + `begin_marked` | the marked area is one write door and one step per gesture: the marquee from press to release and a `cover` stroke commit once, in one coat, and undoing a mark puts coverage back without moving a pixel, a revision or a thumbnail | `test_document`, `test_history`, `test_app` |
| `app/verb_bar.cpp` (`verb.bar`) | the bar offers exactly the verbs whose recipe has `scope: selection`, hangs off the mark and clear of it, and presses the panel's command; where it drew itself is read back rather than remembered by the caller | `test_app`, `verb_bar_probe` |
| `ai.intent` | the whole action surface (intents, values, price, aspect, style, seed, job, session) in one command; a refusal is recorded so the next read shows it; the handshake lives here too | `test_app`, the status line, `panel_aspect_probe` |
| `ai/recipes` (`aspects`, `variants`) | a frame is offered only by a document-scope `from_words` recipe, and a variant is interchangeable with its head at the key it shares (operation, endpoint, `ask`, `aspects`) or the table does not load | `test_recipes` |
| `layer.add`, `layer.move`, `layer.merge`, `layer.remove` (`core/document` + `core/history`) | a reorder is one step that writes no pixel and repaints everything; a merge leaves the canvas as the blend already showed it, to within one 8-bit step at soft edges; a layer arriving and a layer departing are each one step, and undo returns a whole layer - pixels, mask, mode, opacity, name - not an empty one; nothing in `History` invalidates steps by layer id | `test_document`, `test_history`, `test_app`, `layer_panel_probe` |
| `app/ui.cpp` layer cards | every control reports the box it was drawn in, no control lies outside its card and no two claim the same pixel, and a control that cannot apply does nothing at all when pressed | `layer_panel_probe` |
| `app/layer_thumbs` | a picture is keyed to the layer it is a picture *of*: opacity, order and a layer arriving resample nothing, a write to the layer resamples one, and the count is on the wire | `layer_panel_probe` through `app.state.layer_thumbs.rebuilds` |
| `pump_ai_landings` | a succeeded job lands itself at the first door that reads the queue; it never re-lands what undo removed; a job lands only in the `document_epoch` it was queued under | `test_app`, `wire_input` |
| `tools/ai_job_probe.py` | every marked-area verb on the recordings in the order a person has it: bare press refused naming recipe and field → preview carries the typed words in the tier-1 field set → the row's command presses → the answer lands itself on a layer named for the verb and the job → no pixel outside the coverage claimed → undo byte-for-byte | `test_ai_job_probe` |
| `tools/panel_aspect_probe.py` | the aspect and style dropdowns commit: a click pair changes `aspect` and the quoted size follows it (ratio held, one axis still full), a second pair changes which recipe the intent holds and what `ai.verbs` calls `active`, then the key runs that recipe and it lands | `test_panel_aspect_probe` |

## 2026-10-16 - One coverage, and the verbs that come to it

Slice 3, first two steps. **Coverage**: `select`/`Mask` were already one type, so `coverage.target
{to: marked|mask}` and the `cover` tool (`K`) moved the *editing* onto that fact — brush scratch and
fence shared, marquee following the switch, `PointerStroke` keeping the lifetime it was pressed on.
Colour stopped being a target: the Layers panel's `paint: layer | paint: mask` radios are gone and the
brush paints colour whichever way the switch is aimed, which is what makes `cover` aimed at a missing
mask a refusal rather than a surprise.

**The bar**: `verb.bar` reports the verbs whose recipe has `scope: selection` and where the bar drew
itself, and the window presses the `ai.<verb>` commands it names — `Heal` is a new verb row (`Ctrl+H`)
over the existing `heal` recipe, so *Fix* exists without inventing a mode. Two measured notes: the
panel is a fixed fraction and the coverage block pushed the generate row's **style** dropdown under the
fold at 900 px, so the coverage measures moved to the status line, which already had them; and a window
submitted after the canvas is still drawn under it, so the bar calls
`BringWindowToDisplayFront` — `verb_bar_probe` presses the bar at the coordinates the app reported,
which is the only way either fact could have been found.

`docs/PLAN.md` above and `docs/ARCHITECTURE.md` (seam table, source tree, the `coverage.target`
invariant) carry the rest. Suite: 37/37 with `test_verb_bar_probe` added.

## Traps the code cannot show
* **A resampled picture carries its filter's throw.** Lanczos reaches about three pixels past an edge and
  the throw has alpha, so a result left at the filter's natural size holds a *picture* bigger than the thing
  in it - and `content_bounds`, which is what the next scale, move or crop is aimed at, grows by the reach
  every time round. Cut the output at the box that was asked for; `resample_margin_px` is about the input
  side, where the window must not run off the data.
* **A field that refuses a value has to take the keyboard back, and ImGui will not warn you that it let
  go.** A single-line `InputText` deactivates when Enter is pressed whether or not `EnterReturnsTrue` is
  set, so rejecting an input closes the thing the person is typing into - and the key they reach for next
  is `Escape`, which with nothing holding the keyboard is the app's quit key. The symptom is not a broken
  field but a save prompt over the canvas and every screenshot claim after it describing a dialog. Re-arm
  the field on a refusal, and remember that `io.WantCaptureKeyboard` describes the frame that just ended:
  on the frame a field is re-armed it is stale, which is why the host's shortcut gate asks
  `typing_in_layer_field` beside it rather than trusting either one alone.
* **A position recorded while a stack shrinks is not a position for putting it back.** Undo re-inserts the
  layers a step removed in the order it removed them, each into a stack that is *growing*, while the index
  each carries was read off a stack that was *shrinking*. One removal per step and the two frames coincide;
  several, and a four-layer stack comes back reversed — a plausible-looking stack, which is why no test of a
  single undo sees it, and why the claim has to be walked twice. A step that replaces the stack holds the
  stack and swaps it; only a step that removes exactly one layer may be trusted with a position.
* **Price a history entry where it lands, not where it was written.** `step_structure` moves the entry into
  the other stack, so `held(entry)` after that call prices a moved-from object: nothing for the kinds whose
  payload had just left, and a whole stack for a swap. `bytes_` underflowing does not assert — the budget
  simply believes the history is enormous and drops steps from the bottom, so undo dies quietly in a document
  that looks untouched. Read `held(into.back())`.

* **ImGui gives a point to the item submitted *first*, not the one drawn on top.** A widget whose rect lies
  inside an earlier, wider one - a 22 px button over a 300 px `Selectable` - draws, lights up under the
  cursor and never fires, because `ItemHoverable` refuses the later item while `HoveredId` belongs to
  another. `SetItemAllowOverlap` is not the fix: it lets *both* have the click, so the row selects and
  presses. Reserve the width instead (a button is `CalcTextSize(label).x + FramePadding.x * 2`, which is
  `ButtonEx`'s own formula, so the reservation is exact), report every control's box, and have a probe
  demand that they are disjoint and inside their frame. Overlapping reported rects are a dead control, and
  no screenshot shows it - the widget looks perfectly healthy.
* **A counter that identifies one layer cannot key a cache of many.** `content_revision` is per layer, so
  two layers reach the same number, and anything mapping *rows* - panel positions, which a reorder shifts -
  onto layers by that number serves the wrong picture. Key such a cache by the thing it is a picture *of*;
  the document is then free to say what a change really moved - this layer's content, or only how the
  layers stack.
* **An injected double-click is not a double click on this box.** Two injected presses reach ImGui ~0.5 s
  apart - 200 frames apart at the ~340 fps this app idles at - however tightly the driver sends them, and
  ImGui counts a pair only inside `io.MouseDoubleClickTime` (0.3 s), so the second press starts a new click.
  Raising the threshold would tune the app to the harness. Drive the command the gesture uses, and give the
  feature the key it deserves anyway (`F2`): `layer.rename.begin` opens the field the double-click opens, so
  the probe checks the door instead of claiming a gesture it never performed.
* **`xvfb-run` clips pointer injection; the probes are run without it.** `wd.ensure_display` raises an
  Xvfb at the size the probe asks for and is what ctest uses, so motion reaches the whole window. Running
  a probe by hand under `xvfb-run -a` instead reuses *its* screen, and XTEST motion then stops arriving
  around x≈640 - which looks exactly like a dead widget in the right-hand panels. Two claims were nearly
  chased as app bugs over it and one passed vacuously because the click had never landed. Run by hand as
  `env -u DISPLAY python3 tools/<probe>.py`, and check that a click changed some state before believing
  that it could.
* **A hidden mouse cursor is yours until you show it again.** `sapp_show_mouse(false)` is sticky:
  sokol_imgui re-applies the cursor *shape* from ImGui at new-frame and never re-asserts visibility. A
  canvas that hides the cursor to draw a brush footprint in its place must state visibility every frame,
  not toggle it on entry, or the cursor is gone over the entire window for the rest of the session.
  `pointer_probe` holds this: leaving the canvas asks for a visible cursor again.

* **A held quit has to be cancelled with the host.** `SAPP_EVENTTYPE_QUIT_REQUESTED` only asks: sokol
  quits unless the app calls `sapp_cancel_quit()` (in the Linux loop, the `quit_requested &&
  !quit_ordered` block). `app.quit` answers the ask and the host cancels when the answer is
  `quitting:false`; setting `quit_pending` alone closed the window with its own dialog still unsaid,
  which is exactly what it did the first time. Covered through the key path by `wire_input`; the
  close-button path is read from sokol, not probed - a synthetic `WM_DELETE_WINDOW` probe hung in X11
  window traversal and was removed rather than committed half-finished, so pressing the close button
  on unsaved work is still a human observation waiting to happen.
* **A dialog that answers a command must be routed by the command layer.** While the pick's routing was
  a static in the widget, an answer to a picker started over the wire was applied by nobody - the path
  reached `file.pick.state` and stopped. `pump_picked_files` is the host's per-frame door, and a refusal
  it produces lands in `pick_note`, on the status line where the person is, not only in the log panel
  they have not opened.

* **A words box that has focus keeps every shortcut.** In this ImGui build `Tab` does *not* move focus
  out of an `InputText` — measured, text keeps appending — so a verb key pressed with a box active is
  correctly swallowed until `Enter` / `Escape` / a click elsewhere. `WantCaptureKeyboard` being true
  looks like a bug in the gate and is not one; prove it with injected keys before changing the gate.
* **A live gesture must write onto the bytes it found, not onto what it has already written.** Both halves
  of coverage failed this way in one pass: the marquee aimed at a mask wrote its shape into the live mask,
  so the leftovers of every earlier box in the sweep stayed marked, and `cover` aimed at the marked area
  re-stamped its accumulated scratch onto the current selection, so a still pointer darkened the dab once
  per frame. Restore the held-aside region first, then paint what the event touched — the rule the colour
  path has followed since the paint-cost probe, and the reason the marked coverage was given the same door
  instead of a shortcut.
* **Undo takes the marked area with it.** A probe that undoes an answer must mark the area again before
  asking whether a verb could run, or it reads its own bookkeeping instead of the verb.
* **`nlohmann` two-step**: an object literal whose key is a *variable* can deduce an array — build the
  nested object as a named local first; and ranging over a subobject of a function's temporary return
  walks freed memory — bind the value, then range. Both are silent.
* **Never clean up with a broad `pkill -f`**: it matches the shell running the cleanup. Kill by pid from
  `pgrep -x`, and after any probe run `ps -eo args | grep -E 'noise-edi|Xvfb'` must come back empty.
* **A doc claim that cites a tool means the tool is committed and registered as a test.** Otherwise the
  claim is a wish.
* **Re-capture goldens with `wire_drive.py --update-golden` and look at them.** A golden nobody looked at
  is a lie with a checksum.
* **A test that is never called must be a build error** — the anonymous namespace closes before `main()`.
* Verify a claim about the tree with `grep` before it goes into a doc or a commit message. Prefer the
  edit tool over blind `str.replace`.

## The tuning pass, specified before more images go out

Two recipe questions are open. They are measurement, not code, so they get a probe with a cap and a
written method — and **nothing they measure is ever asserted**, because a diffusion answer is not a golden
file. What the probe asserts stays the invariant set `ai_job_probe` already asserts per image; the output
is numbers into `docs/probes/`, and a wording or radius then changes in `assets/recipes/*.json` *because
of them*. Budget: 7 images, 2 held to re-run whichever cell looks like noise. One scene, one model, fixed
seed, resident model put back unpinned.

* **A — Remove's wording, 3 images.** A plain `Remove the marked content` answer once cloned nearby
  texture instead of continuing it. Three candidate `prompt`s, same scene, same seed, 256×256: the current
  sentence; one adding `continue the existing texture and lighting; do not duplicate objects`; and a
  minimal `Remove the marked object.` Per answer: **seam contrast** (mean gradient magnitude on the
  coverage boundary ring), **repetition** (off-centre autocorrelation peak inside, against the same
  statistic on the context ring — a clone spikes, a continuation does not), and the **drift ratio**.
  Lowest seam-and-repetition wins, *only if* the high-frequency ratio below stays up, so "smear it flat"
  cannot win.
* **B — context radius against hole size, 4 images.** `prep.radius_px` {12, 24} × marked area {90×90,
  150×150}. A radius may recover context and must not soften the answer: **high-frequency ratio** =
  interior gradient energy / context gradient energy, near 1 required (`region_json`'s flat-region
  tolerance is 0.06, so under ~0.7 is a visible soft patch). If the big hole wants a different radius, the
  recipe grows a rule rather than a second constant.

## Owed from the original scope list

The checklist the retired scope document carried, kept because some of it is still genuinely owed
(`heal`, autosave, the keymap file, the role-assignment surface). It has **not** been re-verified against
the tree and several items have landed since - so it is a source of candidates, not a task list. Where it
disagrees with [PRODUCT.md](PRODUCT.md) or the code, it is the older text.

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
- import PNG/WebP/JPEG; export PNG/WebP/JPEG + `.noise`; document ceiling 8192² —
  **implemented**: `doc.open` reads all three by their bytes, `doc.export` writes all three by
  extension, and a format with no alpha channel says it flattened (`discarded_alpha`)
- `Upscale document` (ESRGAN via `/v1/images/upscale`)

**Layers** (raster only)
- add/delete/duplicate/reorder (drag), visibility, inline rename, opacity, blend modes
  (normal, multiply, screen, overlay, darken, lighten, color-dodge, color-burn for MVP;
  soft/hard-light, difference, exclusion, add in M5) —
  **visibility, opacity and the eight modes are implemented**: `kBlendModes` is the one table, the
  panel has a dropdown per layer, `layer.set {mode}` sets one, and the composite applies it. **Reorder is
  implemented** as `Up`/`Down` on a card and `layer.move {index,to}`: one step, no pixel written, undone in
  one. Dragging a card is not, nor are rename, duplicate and the M5 modes
- **merge down implemented** (`layer.merge {index}`): the upper layer's mask, mode and opacity are applied
  on the way down, the survivor keeps its identity, the canvas does not move to within one 8-bit step, and
  one undo separates them again. Flatten (under its own id), crop-to-selection and
  new-layer-via-copy/clone are not
- **live**: a layer mask (`Layer::mask`) the composite honours, saved with the project, converting
  both ways with the selection (`mask.from_selection`, `select.from_mask`) - buttons for both in the
  Brush panel - and **paintable**: the Layers panel puts one on (`mask.add`), takes it off
  (`mask.remove`), fills it (`mask.fill {to: show|hide}`) and inverts it (`mask.invert`), and `cover`
  paints whichever coverage `coverage.target {to: marked|mask}` names - one editor over the marked area
  and the saved mask, one drag per undo step. Still missing: enable/disable without deleting it, apply
  (destructive), alt-click to view the mask, and a mask thumbnail in the row

**Selections**
- rect, ellipse, polygon/lasso (path → scanline fill), magic wand (tolerance,
  contiguous/global, anti-aliased) on composite or active layer
- new/add/subtract/intersect modifiers; invert; feather; expand/shrink; border
- **live**: marching ants, `Ctrl+D` clear, invert, save selection ↔ layer mask (and the reverse), and
  three tools — rectangle (`M`), the anti-aliased ellipse inscribed in the drag (`Shift+M`), and
  freehand Lasso (`L`) with even-odd filling and `Shift`/`Alt` add/subtract. `select.rect`,
  `select.ellipse` and `select.lasso` expose replace/add/subtract to the command table. Missing: wand,
  feather, expand/shrink and border

**AI**
- `New image from prompt` → new layer, aspect presets at the model's native scale,
  presets steered toward the model's strength (`-anime` on Anima, `-photo` on Krea 2)
- `Remove object` (flagship): paint → blur-prep → remove prompt → local composite
  **implemented**, as a **verb**: mark an area, `Ctrl+Backspace`, and the answer arrives on its own new
  layer. No words, no preview press, no scope choice, no destination. `ai.verbs` and `ai.intent` are the
  same surface the key goes through, the compiled request is priced in the status bar before it costs
  anything, and a model swap asks first.
- `Change` (in-place edit), `Fill`, `New image` — same plumbing, different recipe, one key each
  (`Ctrl+Shift+C`, `Ctrl+Shift+F`, `Ctrl+G`). The verbs are rows of `ai_verbs.cpp` over rows of
  `recipes/*.json`; the panel lists them from `ai.verbs`, so neither the buttons nor the words they ask
  for are written twice
- `Heal` — in-process Telea/Chan fusion when there is no server or no edit model; backend
  when available ("no network, no problem")
- `Upscale region / document`
- `Reimagine` — full-frame edit dropped in as a new layer at 50 % opacity
- `Free command` with exact-request preview (fields + the actual prepped PNG)
- draft (≤512 px) → refine → commit flow; ETA from client-measured latency EMA — the EMA is built
  and read (`server.state.latency`, `~X s` on the status bar when the box has answered that size before);
  the draft/refine *stages* are not
- variants = N queued jobs with `seed+i` (not the `n` param), landing one-by-one in a tray;
  A/B wipe; Accept/Discard; every result landed as a layer
- ~~request cache~~ **out of P0**: it hits only on a byte-identical re-ask (rare in editing, the
  normal case in probing), and it had no bound and no cleanup. A re-ask costs a second request,
  which is the true price. Revisit only with an eviction policy and a size cap written down first.
- journal/provenance panel + "Regenerate with same params"

**Robustness**
- undo/redo including AI ops (free via blob indirection), autosave every 2 min to a recovery
  bundle, crash-recovery prompt at launch
- ~~durable job queue~~ **out by decision**: a request in flight belongs to a document revision a
  restart cannot guarantee, so its answer would land somewhere that no longer means the same thing.
  Losing one at a crash costs a re-ask; landing it wrongly costs an edit nobody asked for
- `ai.log` of every request/response (redacted, image bytes → blob hash)

---

## 2026-10-15 - Generate answers at a chosen frame, in a chosen style, and it is checked against the box

`ai.generate` took its size from the whole document, so an aspect was whatever the canvas happened to be,
and a style was a recipe nobody could reach: the row bound one verb to one recipe, so a variant was a
second key over the same operation. Both are table data now. `core::AspectRatio` parses and prints
`"16:9"`, `ai::frame_for_aspect` picks the largest centred rectangle of that ratio, and `Recipe` carries
`aspects` (only a document-scope `from_words` recipe may) and `variants` (which must be interchangeable -
same operation, endpoint, questions and frames - because standing in at the same key means answering the
same questions). The panel gets `Aspect` and `Style` dropdowns under `Generate`, `ai.verbs` reports
`active`, and the status line prices the frame before anything is sent.

Verified against the dev box: `tools/ai_job_probe.py --live --only generate --generate`, five asks at
420x300, every frame the recipe offers, recorded in
`docs/probes/2026-10-15-generate-frame-and-style.md`. Three findings went back into the code rather than
into a note:

- `new-image` inherited `feather: auto`, and `compose` softens both ways, so the answer bled roughly the
  feather past the frame into canvas that was never uploaded. The generate recipes say `feather: none` and
  a table test refuses a document-scope `from_words` recipe that feathers.
- `Landing` stored only the crop, so the landing report and the saved mask drew a box nobody asked for
  whenever the frame was not already on the policy's grid. It carries the mask bounds now.
- The probe's wire socket had a shorter read timeout than its own `job.wait`, so a cold model load killed
  a live run before it could put the box's model back.

Verdict on the architecture: the seam held, and the telling part is what did not change - field policy,
`RoleTable`, `RegionPlan`, `JobQueue`, `AnswerLanding` and the composite all took the frame untouched,
because a frame is a mask and masks were already the currency. `Landing` grew one field, which is the
report telling the truth about coverage rather than a new concept. What this area wants next is a frame
*inside* a marked-area verb (a 3:4 inpaint within a 4:3 selection): the geometry supports it, no recipe
asks for it, so it stays unmade rather than speculative.

## 2026-10-15 - Slice 1 begins: files, and the dialog a launch opens

`platform::pick_path` asks the desktop for one path and answers with a path, a cancel or a named failure;
Linux implements it as a `zenity` subprocess, `$NOISE_PICKER_PROGRAM` stands in for it under test, and
every other target links a file that says the seam is unwritten rather than reporting a cancel nobody
pressed. `file.pick` runs it on a worker so the frame loop and the wire keep answering with a dialog open,
`file.pick.state` carries the answer, and `io/config` was born holding the recent files that `doc.open`
and `doc.save` now write. `front_door` / `file.new` / `new_document` make the New dialog a state the wire
can open, shut and fill; a launch with no file and no `--no-start-dialog` shows it, Escape closes it
instead of quitting, and the title bar says which file is open and whether it has unsaved work.

**Blocked, and named where it bites:** `ImGui::BeginMainMenuBar()` asserts (`Missing End()` in
`ErrorRecoveryTryToRecoverState`) in this frame — ImGui 1.93 WIP with sokol_imgui and a fullscreen
dockspace; an *empty* bar reproduces it, so it is not a widget-nesting mistake in my menu. The File
actions therefore live in a row at the top of the tool panel, and the keys stay in the command table
(`Ctrl+N`, `Ctrl+O`, `Ctrl+S`). Revisit when sokol_imgui/ImGui is next pinned: a menu bar is what a person
expects, and the row is a workaround, not the design.

**After that, same day.** `layer.import {path, at}` lands an image file on a layer of its own with the
middle of the picture at the point it was named for, sokol's XDND handling feeds it on a file drop, and a
drop that fell whole off the canvas is refused with the file's name in the error. The ImGui pin moved to
`v1.93.0-docking` — the main menu bar still asserts, so the version was not the fault and the tool-panel
file row stays. And rule 1 of PRODUCT acquired its exception: an answer arriving on a canvas that holds
nothing fills the empty layer `New` made instead of stacking a picture above an invisible one, which is
what the front door's prompt was always promising.

**Later the same day.** `layer.import {path, at}` lands an image file on a layer of its own with the middle
of the picture at the point it was named for; sokol's XDND handling feeds it when a file is dropped on the
window, and a drop that fell whole off the canvas is refused with the file's name in the error. The ImGui
pin moved to `v1.93.0-docking` and the main menu bar still asserts, so the version was not the fault and
the tool-panel file row stays. Rule 1 of PRODUCT gained its exception: an answer arriving on a canvas that
holds nothing fills the empty layer `New` made instead of stacking a picture above an invisible one, which
is what the front door's prompt was always promising.

**Drops, and where they cannot land.** The person is on Wayland, which means the editor runs as an X11
client under XWayland and a file dragged from a native file manager has to cross XWayland's XDND bridge to
reach it. sokol implements the X11 target side, and the window now advertises `XdndAware = 5` (measured, not
assumed), yet the pointer still shows refusal over the window: what is refused is XWayland's side of the
handshake, which nothing in this tree owns, and sokol master has not moved (one commit since the pin, about
Vulkan). So *Insert* sits beside Export and runs the same `layer.import` a drop runs, centred on the canvas rather than under a
pointer: `file.answer {for, path}` is the routing table for every picked path — `open`, `project`, `flat`,
`image` — because the frame that opens a picker and the frame that gets the answer are minutes apart, and a
button a person can click has to be a command a probe can call. Writing that table caught a live bug: a
ternary on the set name gave Insert a **save** dialog.

**Settled, with the desktop as the witness.** A file dropped on GIMP 3.2 (a native Wayland client) lands;
the same drop on `xterm` (X11) is refused exactly as it is here. So the rule on this compositor is
*XWayland windows receive no drops*, and it is not a bug this tree can fix: file drops need a Wayland
backend, which sokol does not have, or an X11 session. `Insert` is the operation until then, and the XDND
code stays because it is the same `layer.import` either way.

## 2026-10-15 - Slice 2: the New dialog makes the document and the idea in it

The dialog offers **kind** now — `no style / photo / illustration`, read from `RecipeSet::family` through
the `new_document` command and validated against it, so adding a style to `assets/recipes/` is one more
radio button and nothing in the code knows the word "photo". Those three labels stopped being
sentence-case descriptions of recipes because the same field is read by the panel's style row and the
dialog's kind row, and a person picking a style is not choosing a recipe.

**Create** is the command `front_door.create`: it makes the canvas of the size typed and, if the box holds
words, points the intent at the chosen kind and runs `ai.generate`. The button is one line, a driver walks
the identical path, and a refusal leaves the door open — the words are usually what is wrong, and a dialog
that shut on the way out would throw them away with it. A running job reads `working: <recipe>  <n> s`
with a **Cancel** that calls `job.cancel`; the id returns for a finished job, where it is the one thing a
log line and a bug report have in common. `AppState.now` is published by the host rather than read from a
clock inside a widget, so the elapsed number is something a test could pin rather than a wall-clock read
 smuggled into a frame.

The prompt box was a lesson in where truth lives. It held a `char` buffer filled when the dialog opened, so
a prompt set over the wire stored correctly and rendered empty forever: the buffer is a *view* of
`new_document` and is refilled from the form whenever the keyboard is not holding it — the rule the verb
fields already used, discovered by looking at the working case instead of inventing a second one.

## 2026-10-25 — Native ImGuiFileDialog (superseded)

The external Linux-only picker is replaced with the native ImGuiFileDialog v0.6.8
modal. `file.pick` queues `PickRequest`, the frame renders the browser, and the
existing `pump_picked_files()` routes `PickResult` through `file.answer`.
The native picker shares the app's Light/Dark theme and uses Sokol thumbnails;
Open, Save As, Export, Insert and multi-image references share one implementation.
WASM retained the browser file bridge. This picker was replaced on 2026-10-26 to meet the movable-window
requirement; see the newer file-dialog entry below.

## 2026-10-26 — Movable native file picker

Native File Dialog Extended v1.4.1 replaces the embedded browser. The `file.pick` command still queues
the same request, then the host consumes it on the main thread, passes the Sokol parent-window handle,
and waits for the OS dialog to return. The native modal pauses the host frame loop and debug-wire polling;
after it closes, `file.pick.state` reports the result and `file.answer` still owns file operations. The
WASM browser bridge is unchanged. Linux builds use NFD's
GTK 3 backend; the native host remains Linux-first, while the dependency also supplies Windows and macOS
backends for the host ports.

Architecture verdict: file ownership is now on the platform side instead of being drawn into the one
Sokol window. A native modal window solves detachment without adding secondary Sokol contexts, changing
the app command protocol, or moving file routing into the platform adapter. The test seam still separates
the queued request/result from file operations, and the existing selection order is deterministic after
the NFD path set is collected. The crop/resize milestone and the document/history constraint it needs are
unchanged. See `docs/FILE_BROWSER.md`.

## 2026-11-02 - Remove forced edit handoff; add 90-degree rotation

**Architecture verdict:** The blue AI selection and each result layer's mask already have
independent editing tools. The `New AI edit` UI and automatic mask selection duplicated a
mode switch the user did not need and obstructed the canvas in compact windows. A landed
answer now keeps the image plane active; the floating AI verb bar stays hidden until the
selection changes. The result mask stays independently accessible through its thumbnail,
without a new dialog or additional Apply action. Selection revision, not a guessed timeout
or a costly mask copy, determines when the existing verb bar is relevant again.

`doc.rotate` uses the same whole-canvas replacement history as resize and crop: pixels,
all layer masks and the marked selection rotate together. It exchanges width and height,
bumps the geometry epoch to invalidate pending AI work and keeps one Undo step. The default
Outpaint Fill suggestion is now `extended the background` based on user feedback; no
new recipe, model dependency or upscaling code was introduced.

## 2026-11-02 - Mask-thumbnail visual feedback

**Architecture verdict:** A mask thumbnail is an editing target, not a second coverage mode.
Selecting it now activates Brush only when the current tool cannot paint the mask; this makes
the existing red mask preview visible immediately while preserving Eraser and the separation
of Cover/selection (blue) from mask painting (red). No new overlay state or UI command is needed.
The wire and the thumbnail continue to share the `layer.plane` command.
