# PLAN — what is next, and the reasoning the code cannot carry

Last re-read of the tree against the docs: **2026-10-15**, when the product was rewritten around what a
person does ([PRODUCT.md](PRODUCT.md)) and the other design documents were merged into this one,
[ARCHITECTURE.md](ARCHITECTURE.md) and [TESTING.md](TESTING.md). `AGENTS.md` is normative and outranks this
file.

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
3. **Nothing persists except the document.** No answer cache, no job file, no `job.retry`, no durable
   queue. The runtime directory is scratch — startup deletes the previous run's `ai/` and `captures/` —
   and nothing is read back from it. Pressing the verb again *is* the retry: it recompiles against what
   the document is now and costs a second request. Any future store states its identity scheme and its
   eviction policy before it gets a writer.
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
   | prefs, session state | `io/config` (planned), `manifest.json`, the environment |
7. **The default `ctest` run is hermetic and opens no sockets.** `$NOISE_AI_FIXTURES` selects the recorded
   transport. Live-tier tests are labelled `live`, registered only with `-DNOISE_LIVE_TESTS=ON`, skip
   loudly without `$THENOISE_URL` / `$THENOISE_API_KEY`, assert contract and invariants and never model
   output, and put the resident model back (`server.load`'s argument is `pinned`). No URL, host or key
   ever enters the repo.

## What is next, in order

Ordered by what makes the thing usable rather than by what is interesting. Each slice is demoable by a
human without a terminal, ships the wire command that makes it observable, and ends with a step of
`tools/flow_probe.py` walking [PRODUCT.md](PRODUCT.md)'s scenario. Until slice 3 the scenario is walked
partly over the wire; after it, by clicking.

**Slices 1 and 2 landed 2026-10-15** — files, and a new image from an idea — with two deliberate gaps: the
New dialog quotes no price, because that was cut by decision, and *Blend* belongs to slice 5. Slice 3, one
coverage with verbs at the marked area, is next.

1. **Files.** Start dialog (New / Open + recent), Open, Save, Save As, Export (png / jpg / webp),
   drag-and-drop onto a new layer, dirty title, quit guard. Native picker through the desktop portal or
   `zenity`, one file in `src/platform/`, with the in-app browser as the fallback. `doc.new`, `doc.open`,
   `doc.save` and `doc.export` already exist as commands; what does not exist is a way to reach them.
2. **New image from an idea.** The New dialog's prompt, kind (a recipe family's `variants`) and size -
   preset ratios plus explicit pixels - and the dialog **quoting the price** the way the status line does.
   OK generates into the document's first layer. Progress, coarse or indefinite, plus Cancel.
3. **One coverage, verbs at the marked area.** Selection and layer mask share the coverage editor, the
   shape language and the maths, with the target switch saying which lifetime the brush is painting.
   **Fix**, **Insert**, **Edit**, **Erase** as three-plus separate buttons (decided: not one button with a
   mode) in a bar attached to the marked area, the panel keeping the full controls. Local `heal` lands
   here, because *Fix* on a small area should not pay for a round trip.
4. **Layers you can see.** Thumbnails and mask thumbnails, reorder, opacity, visibility, move and scale,
   **Flatten** (`layer.merge`). The panel is always visible: it is where a person goes to understand what
   an edit did.
5. **Blend, and images as input.** Drop an image → its own layer → *Blend* with no prompt repaints it into
   the scene on that layer. Needs the third recipe scope, `region: layer`, whose source is the layer's
   pixels and whose coverage is its alpha. *Blend* with a prompt, then control images (openpose), come in
   through the same door.
6. **Document geometry.** `doc.crop`, `doc.resize` for the canvas - with *AI fill* on the grown strip, the
   outpaint mechanism below - and *Image size* with a method: Lanczos, `AI upscale (fast)` (ESRGAN),
   `AI upscale (best)` (an edit model, so one `roles.json` row and one recipe). The seam to think about
   first is history: `Document` fixes its `Extent` at creation today, and the journal holds full-size
   snapshots, so a size change needs a decision about what undo means across it.

   **Outpaint, the mechanism.** Grow the `Extent`, mark the grown border as the coverage, compile the
   region with the original frame *inside* it as context, and let the answer land on a new layer clipped to
   the grown strip, so the original pixels stay byte-untouched. It is `reconstruct` with a different mask
   story, which is why it wants its own `operation: extend` rather than a weakened call.
7. **The tuning pass and the parked niceties.** Recipe wording and `prep.radius_px` per verb against hole
   size (specified at the end of this file, needs the box), the `enhance` prompt-improver behind its own
   button, the keymap file, autosave.

Seed is not a user-facing concept: **Retry** re-asks the same request with a different seed and lands
another layer. Roles and recipes are ours to pin for now - the sets are in `assets/roles.json` and
`assets/recipes/`, loaded from disk, and the plan is that a person edits those files, not a dialog, so
nothing here needs a settings UI to be foreseen later.

## The seams, and how you see them

| seam | what it guarantees | how it is seen |
|---|---|---|
| `ai/region` | compile → inverse-map is exact for off-canvas and tiny crops; output dims always satisfy `size_policy`; prep claims (`keep`/`destroy`) measured, not asserted; `frame_for_aspect` gives the largest centred rectangle of a ratio with one axis full, and leaves the 16 px grid to `plan_region` | `test_region` |
| `core/blend` | one separable per-channel rule; CPU reference == GPU numbers over a golden pixel set | `test_blend`, `tests/golden/` |
| `ai/compose` | **pixel-lock**: every byte outside the feathered coverage is identical to the source; paired colour drift (context band vs document band, pixel against pixel) taken off the whole answer | `test_compose`, `ai_compose_probe`, live drift |
| `ai/provider` | the field policy is the only place a field can be added; tier-2 and unknown fields refused by name; `image_url` / `mask_url` are never sent | `test_provider` |
| `ai/queue` | one worker, cancellable, in-memory; a job's timeout is predicted when it is queued so it cannot change under it; **a re-ask is another request** | `test_backend` |
| `ai/server` | latency EMA per `(model, w×h, steps)` → ETA and `timeout_request`; a reply inside 1 s is the box's own answer and never a sample | `test_backend`, `docs/probes/` |
| `app::kVerbs` + `ai.verbs` | four verbs bound to recipes; a row whose recipe contradicts it — wrong operation, an `ask` that grew, two verbs on one recipe — makes the row not-ready at session open | `test_app`, `ai.verbs` |
| `ai.intent` | the whole action surface (intents, values, price, aspect, style, seed, job, session) in one command; a refusal is recorded so the next read shows it; the handshake lives here too | `test_app`, the status line, `panel_aspect_probe` |
| `ai/recipes` (`aspects`, `variants`) | a frame is offered only by a document-scope `from_words` recipe, and a variant is interchangeable with its head at the key it shares (operation, endpoint, `ask`, `aspects`) or the table does not load | `test_recipes` |
| `pump_ai_landings` | a succeeded job lands itself at the first door that reads the queue; it never re-lands what undo removed; a job lands only in the `document_epoch` it was queued under | `test_app`, `wire_input` |
| `tools/ai_job_probe.py` | every marked-area verb on the recordings in the order a person has it: bare press refused naming recipe and field → preview carries the typed words in the tier-1 field set → the row's command presses → the answer lands itself on a layer named for the verb and the job → no pixel outside the coverage claimed → undo byte-for-byte | `test_ai_job_probe` |
| `tools/panel_words_probe.py` | the panel's own door: injected keys into a box found by sweeping the dock, then the verb's real key → a job whose prompt carries the typed marker → the layer named for it. A bare key with nothing typed starts nothing | `test_panel_words` |
| `tools/panel_aspect_probe.py` | the aspect and style dropdowns commit: a click pair changes `aspect` and the quoted size follows it (ratio held, one axis still full), a second pair changes which recipe the intent holds and what `ai.verbs` calls `active`, then the key runs that recipe and it lands | `test_panel_aspect_probe` |

## Traps the code cannot show

* **A words box that has focus keeps every shortcut.** In this ImGui build `Tab` does *not* move focus
  out of an `InputText` — measured, text keeps appending — so a verb key pressed with a box active is
  correctly swallowed until `Enter` / `Escape` / a click elsewhere. `WantCaptureKeyboard` being true
  looks like a bug in the gate and is not one; prove it with injected keys before changing the gate.
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
  panel has a dropdown per layer, `layer.set {mode}` sets one, and the composite applies it. Rename,
  duplicate, drag-reorder and the M5 modes are not
- merge down, flatten, crop-to-selection, new-layer-via-copy/clone
- **live**: a layer mask (`Layer::mask`) the composite honours, saved with the project, converting
  both ways with the selection (`mask.from_selection`, `select.from_mask`) - buttons for both in the
  Brush panel - and **paintable**: the Layers panel puts one on (`mask.add`), takes it off
  (`mask.remove`), fills it (`mask.fill {to: show|hide}`), inverts it (`mask.invert`) and aims strokes
  at it (`mask.target {plane: pixels|mask}`), so the brush reveals coverage and the eraser takes it
  back, one drag per undo step. Still missing: enable/disable without deleting it, apply
  (destructive), alt-click to view the mask, and a mask thumbnail in the row

**Selections**
- rect, ellipse, polygon/lasso (path → scanline fill), magic wand (tolerance,
  contiguous/global, anti-aliased) on composite or active layer
- new/add/subtract/intersect modifiers; invert; feather; expand/shrink; border
- **live**: marching ants, `Ctrl+D` clear, invert, save selection ↔ layer mask (and the reverse), and
  two shapes — rectangle (`M`) and the anti-aliased ellipse inscribed in the drag (`Shift+M`), plus
  `select.rect` / `select.ellipse` with `replace`/`add`/`subtract`. Missing: lasso, wand, feather,
  expand/shrink and border

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
