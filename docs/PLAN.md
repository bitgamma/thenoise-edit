# PLAN — what is next, and the reasoning the code cannot carry

Last re-read of the tree against the docs: **2026-10-11**, after Fill and Change landed as typed
sequences and the panel's words box was fixed. Read [TOOLS.md](TOOLS.md) first: it is the
editing-concept re-alignment (verbs, selections, what an operation is) and it is where the UI work is
tracked. [ARCHITECTURE.md](ARCHITECTURE.md) is the module map, with planned modules marked as such,
[AI-BACKEND.md](AI-BACKEND.md) the server contract, [MVP.md](MVP.md) the scope and the decision log,
[USER-GUIDE.md](USER-GUIDE.md) what a person sees. `AGENTS.md` is normative and outranks this file.

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

1. **Extend (outpaint).** The canvas mechanism nothing else in the app has — and it is a *document*
   mechanism, not a window one. Measured 2026-10-15: the window is already resizable by the window
   manager, and the framebuffer, the layout and the canvas rect all follow (900x620 → 1300x900, canvas
   970x620 → 1012x777). What does not exist is `Document` owning an `Extent` that a command can change:
   `doc.resize`, undoable, with the view following and the pixel-lock rule holding for the grown area.
   `platform::resize_window` is only automation convenience on top of that (the `--width`/`--height`
   flags set `sapp_desc` once), and the size is already readable as `app.state:framebuffer`. Then the
   original frame is cut out of the answer and a new `operation: extend` describes it — not a weakened `reconstruct`, because the hole is a
   property of the operation. The answer lands on a new layer with the existing pixels byte-untouched,
   which is Rule A and the pixel-lock assertion doing the same work.
2. **Upscale / resize.** `ai.run {recipe: upscale}` works through the `/images/upscale` shape today; what
   does not exist is a document that can change size — `doc.resize` — so the result has nowhere to go
   except a layer that disagrees with its canvas. Then the selection tools P0 still owes: lasso, wand,
   feather, expand/shrink.
3. **The jobs tray**: variants as `seed+i` jobs landing one by one, A/B wipe, Accept/Discard. It reads the
   in-memory queue, and the latency EMA it needs for progress already exists.
4. **The tuning pass**, specified at the end of this file: recipe wording and `prep.radius_px` against
   hole size. It needs the dev box and a written-down approval, and it changes tables, not code.

Beyond the queue, MVP's P0 still owed: local `heal`, autosave and crash recovery, the keymap file and
prefs, Linux packaging. Those are milestones M5's rows, not a fifth thing to start.

*Generate's aspect presets and style variants landed on 2026-10-12; `git log` has the story.* What they
changed architecturally is worth the four lines: an aspect is `core::AspectRatio` (a name, `W:H`, parsed
strictly) and nothing more — it picks a rectangle through `ai::frame_for_aspect` and enters the pipeline as
coverage, so `size_policy` still owns the pixel grid, the field policy still owns the fields (there is no
`aspect` field and never will be on the wire), and landing needed no new rule: the coverage *is* the frame,
so everything outside it is byte-untouched by construction and `test_app` asserts exactly that. A style is
`Recipe::variants` — recipes standing in for one another at one key — validated by `load_recipes` rather
than by a caller hoping the table is sane, which is why the verb, the row and the wire cannot disagree: all
three read `RecipeSet::family`. The door stays `ai.intent`, now carrying `aspect`, and
`tools/panel_aspect_probe.py` is the claim that the two new dropdowns commit through it — a command-level
test cannot see a widget, and the probe found the panel needing a taller window before both rows sat above
the fold.

*Fill and Change landed on 2026-10-11; `git log` has the story.* What has to stay true is on the seam:
`app::kVerbs` generates the commands, `ai.intent` holds the words and the refusal, each asking verb row
carries its own box, and the answers land named for the verb and the job. `test_panel_words` drives the
window itself and is what keeps the panel honest — a command-level test cannot see a widget.

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

## Environment

* Build and gate: `cmake -B build -G Ninja && cmake --build build && ctest --test-dir build`. Release
  tree is `build-release`. `-Werror` with `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`.
  Probes raise their own X display when there is no `$DISPLAY`, so `xvfb-run` is not needed (see
  TOOLS.md's house rule); under a real `$DISPLAY` they use it.
* Live tier: `cmake -B build -DNOISE_LIVE_TESTS=ON && ctest --test-dir build -L live`, with
  `$THENOISE_URL` / `$THENOISE_API_KEY` exported (the dev box). Ask before spending more than ~10 images
  and name the cost.
* Commits are unsigned here: `commit.gpgsign=true` with a key that is not in this box's keyring, so
  `git -c commit.gpgsign=false commit` matches the existing history.
* Definition of done: builds warning-free · a test that would fail if the feature broke · a wire command
  for any new observable state · docs still true · no new duplicated constants or default arguments · and
  this file saying so.

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
