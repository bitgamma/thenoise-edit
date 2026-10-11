# PLAN — the reasoning the code cannot carry, and what is next

Last full re-read of the tree against the docs: **2026-10-11**, after the AI path stopped persisting
anything. Read [TOOLS.md](TOOLS.md) first: it is the editing-concept re-alignment (verbs, selections,
what an operation is) and it is where the UI work is tracked. `docs/ARCHITECTURE.md` is the module map,
`docs/AI-BACKEND.md` the server contract, `docs/USER-GUIDE.md` what a person sees.

## The shape of it, in one paragraph

An AI operation is an ordinary editing command with a compile step in the middle. The user marks an area
(mask, marquee, layer mask) and presses a verb; the region compiler turns the marked area into a crop that
satisfies a size policy the model chose, plus a prepped upload built from the flattened composite; the
recipe builds a request from a field policy that lives in exactly one place; the queue sends it and lands
the answer **on its own new layer** as soon as it arrives. Nothing between the key press and the layer has
a hidden shortcut: no cache, no persisted job, no Apply step.

## Rules every change has to keep

These are not preferences; each one exists because breaking it was observed as a bug.

1. **Rule A — an answer always lands on a new layer, above the layer the region was flattened from.** No
   preview-into-the-active-layer mode exists. The upload is always the flattened composite of the region
   (`Document::flatten_over(crop, active)`), never the single layer.
2. **A per-frame read must not compute anything expensive.** `ai.intent` is read by the status line every
   frame: it quotes (endpoint, size, model, steps, measured ~s) from geometry and tables, and preps/
   resamples/PNG-encodes **nothing**. `AppState::upload_encodes` counts every upload encode, and a test
   fails if reading raises it. `ai.preview` is the only pre-flight compile, and it exists for drivers and
   probes, not for a frame.
3. **Nothing persists except the document.** No answer cache, no `jobs.json`, no `job.retry`. The runtime
   directory is scratch — startup deletes the previous run's `ai/` and `captures/` — and no code reads
   anything back from it. Pressing the verb again *is* the retry: it recompiles against the current
   document and costs a second request.
4. **The words an operation asks for are typed.** An `ask` field has no default anywhere; a run with a
   blank or whitespace-only asked-for field is refused, naming the recipe and the field (Rule E).
5. **Every feature is observable on the debug wire, through the same command table the UI uses.** If the
   UI can't do it via the table, neither can a driver — that is a finding to fix, not a reason for a side
   door. `screenshot` / `image.get` exist so a vision model can look at the result.
6. **One fact, one home; no default arguments; no parallel representations.** Commands and keys: the
   command table. Verb → recipe/key: `app::kVerbs`. Roles/models: `roles.json` + `/v1/models` resolved in
   `ai/registry`. Allowed request fields: `ai/provider`'s field policy. Prompts/policy: `assets/recipes/
   *.json`. Blend maths: `core/blend`'s reference, mirrored only in `tests/test_blend`.
7. **The default `ctest` run is hermetic and opens no sockets.** `$NOISE_AI_FIXTURES` selects the recorded
   transport. Live-tier tests are labelled `live`, registered only with `-DNOISE_LIVE_TESTS=ON`, skip
   loudly without `$THENOISE_URL`/`$THENOISE_API_KEY`, assert contract and invariants never model output,
   and put the resident model back (`server.load`'s argument is `pinned`). No URL, host or key ever
   enters the repo.

## What is built, and how you see it

| seam | what it guarantees | how it is seen |
|---|---|---|
| `core/region` | compile → inverse-map is exact for off-canvas and tiny crops; output dims always satisfy `size_policy`; prep claims (`keep`/`destroy`) measured not asserted | `test_region` |
| `core/blend` | one separable per-channel rule; CPU reference == GPU numbers over a golden pixel set | `test_blend`, `tests/golden/` |
| `ai/compose` | **pixel-lock**: every byte outside the feathered coverage is identical to the source; paired colour drift (context band vs document band, pixel against pixel) subtracted from the whole answer | `test_compose`, `ai_compose_probe`, live drift check |
| `ai/provider` | field policy is the only place a field can be added; tier-2 and unknown fields refused by name; `image_url`/`mask_url` are never sent | `test_provider` |
| `ai/queue` | one worker, cancellable, in-memory; a job's timeout is predicted at queueing so it cannot change under it; **a re-ask is another request** | `test_backend` |
| `ai/server` | latency EMA per `(model, w×h, steps)` → ETA and `timeout_request`; a reply inside 1 s is the box's own answer and never a sample | `test_backend`, `docs/probes/` |
| `app::kVerbs` + `ai.verbs` | four verbs bound to recipes; a verb whose recipe contradicts it (wrong operation, an `ask` that grew, a second verb on one recipe) makes the row not-ready at session open | `test_app`, `ai.verbs` rows |
| `ai.intent` | the whole action surface (intents, quote, seed, job, session) in one command, read or run; the handshake lives here too | `test_app`, status line |
| `pump_ai_landings` | a succeeded job lands itself at the first door that reads the queue; it never re-lands what undo removed; a job may land only in the `document_epoch` it was queued under | `test_app`, `tools/wire_input.py` |
| `tools/ai_job_probe.py` | every marked-area verb on the recordings, each in the order a person has it: bare press refused naming recipe and field → preview carries the typed words in the tier-1 field set → the row's command with the committed words → answer lands itself on a layer named for the verb and the job → no pixel changed outside the coverage claimed → undo byte-for-byte | `test_ai_job_probe` |
| `tools/wire_input.py` | the flagship through injected X events: dragged marquee → `Ctrl+Backspace` → `['Layer 1','Remove j0001']` with nothing else pressed → `Ctrl+Z` → `['Layer 1']` | `test_input_gestures` |
| `tools/panel_words_probe.py` | the panel's own words box reaches its command: injected keys into the box found by sweeping the dock (not by remembered coordinates), then the verb's real key → job whose prompt carries the typed marker → layer named for the verb and the job; a bare key with nothing typed starts nothing | `test_panel_words` |
| `tools/wire_drive.py` | a probe that crashes leaves no `noise-edit`, no `Xvfb`, no stale `/tmp/.X11-lock/X*` (children start a new session; the whole group is signalled on exit/atexit/`SIGTERM`); a display number is claimed with `O_EXCL` and judged stale by its owner's pid, so probes starting together cannot take the same one | every probe run |

Measured on the dev box, 2026-10-11 (see `docs/probes/`): a 256² four-step `inpaint-remove` on a resident
model in **3.5 s**; landed answer's luminance **ratio 1.05** against its own context ring with
`colour_corrected=true`, 0 pixels changed outside the coverage it claimed, undo byte-for-byte.

## What is next, in order

1. **Fill and Change as verified typed sequences.** **Done.** `ai_job_probe` drives `remove`, `fill` and
   `change` end to end on the recordings (75 claims, three requests), each through the row's own command
   with the words its recipe asks for, and reports the layer it made: `Fill j0002`, `Change j0003`.
   `test_app` asserts the same seam plus the refusals - blank, whitespace-only and words filed against a
   field the recipe never asked for - for every one of the five shipped recipes that asks.

   Driving it exposed that the panel could not type at all: the section's single words box belonged to
   whichever recipe the intent held (`inpaint-remove`, which asks for nothing) and was keyed by the recipe
   at one end and the verb at the other, so text went in and never came out on a press. Each asking verb
   row now carries its own box, committed through `ai.intent {recipe, vars}`, and the press carries no
   `vars` - which is also what the key does, so button, key and wire are one door. Verified on the window
   with injected keystrokes, and that run is now `tools/panel_words_probe.py`: it finds each box by
   sweeping the dock and asking what the intent holds afterwards - letters no shortcut claims, committed
   with `Enter` - then presses the verb's real key and expects the layer. It found the boxes at
   `y=290`/`y=360` without being told where they are, so the panel can be relaid out without breaking it,
   and reverting the panel fix turns it red naming the recipe and the field the box was refused against.
   One behaviour around it is ImGui's, not ours: `Tab` leaves the box active, so the verb key is correctly
   swallowed until `Enter`/`Escape`/a click elsewhere - measured before concluding, because
   `WantCaptureKeyboard` being true made it look like our gate was wrong.
2. **Generate: aspect presets and the style variants.** `Ctrl+G` runs `new-image` and lands a layer today;
   what is missing is the aspect choice (`1:1, 3:4, 4:3, 16:9` at the model's native scale, from the
   recipe table not from code) and the two style variants reachable from the panel, not only from the wire.
3. **Extend (outpaint).** The canvas mechanism, which nothing else in the app has: resize anchored
   (`platform::resize_window` is the only seam where a window size can be set, and it is the right place
   to care), the original frame cut out of the answer, and a new `operation: extend` — not a weakened
   `reconstruct`, because the hole is a property of the operation. The answer lands on a new layer with
   the existing pixels byte-untouched.
4. **Upscale / resize**, then the P0 selection tools still missing: lasso, wand, feather, expand/shrink.
5. **The jobs tray**: variants as `seed+i` jobs landing one by one, A/B wipe, Accept/Discard. It reads the
   in-memory queue; the EMA it needs for progress already exists.

**The tuning pass** (recipe wording, `prep.radius_px` vs hole size) is specified at the end of this file
under "The tuning pass, specified before more images go out" and has **not** been run: 8 of the approved 10
live images, 2 held. Nothing it measures is ever asserted — numbers go to `docs/probes/`, and a recipe
value changes *because of* them.

## Reasoning the code cannot carry

* **The hole is a property of the operation, not of the region.** `prep` only means something for
  `reconstruct`; the recipe parser refuses a contradiction because a recipe that contradicts itself spends
  model quota to produce a wrong picture. Prep fills come from context, never from the hole — the hole's
  own mean colour is the colour of the thing being removed.
* **Colour match measures drift, not palette.** Paired context-band difference, pixel against pixel,
  subtracted from the whole answer. The earlier version matched whole-image palettes and painted the patch
  near-black on a textured frame (luminance ratio ~0.2 where today it measures 1.05). A flat fixture cannot
  show this: a fixture has to be able to *see* the bug it guards, which is why the live check runs on a
  grainy gradient with a hard-edged marker in it.
* **Destination and colour reference are different images.** Answers land on empty new layers, so drift
  must be measured against the document's own flattened composite, not against the answer itself.
* **An answer that exists is never discarded.** Cancelling a succeeded job refuses and names the answer's
  path. A landing that refuses is recorded once (`land_refused`, `land_error`) rather than retried per
  frame; a later success clears it.
* **Undo is a decision; an auto-lander must not fight it.** The beat never re-lands what undo removed.
  `job.apply` is the door back — idempotent: if the layer it named is still there it answers
  `already_landed` with that landing's own report and adds nothing; if the layer is gone, it lands the
  same bytes again. Undo of a layer-creating edit runs pixel entries before structural removal; redo the
  reverse.
* **A job may land only in the document that asked for it.** `document_epoch` is stamped at `ai.run` and
  bumped by `note_new_document` in `doc.new`/`doc.open`; an answer from an earlier document stays out and
  the explicit door refuses, naming both epochs. Note this is an **in-run** concern now that nothing is
  persisted — the epoch is about File→New while a request is in flight, not about restarts.
* **Two bug stories from persistence, so it does not come back sideways.** (1) `load_from_disk` parsed the
  id sequence with `find_first_not_of("0123456789")`, which matched the leading `j`, so the next run minted
  `j0001` again: `records_` was overwritten while `order_` gained a second entry, and every entry of a
  succeeded job was one more landing — seven layers for one answer, only the first with pixels. (2) The
  answer cache had no bound and no cleanup (13 MB against 2 jobs in one captured runtime dir) and hit only
  on a byte-identical re-ask, which is an artefact of probing, not of editing. If a future design wants
  either, it has to state the eviction policy and the identity scheme before writing the store.
* **A tool is not a policy.** Tools and verbs map to policies; a document that refuses to open under a
  mismatched tool is a finding, not a default.
* **Three verbs, no "Replace".** Remove, Fill, Change; Generate is document-scope. "Replace" is two
  operations wearing one word. The mask is the model's idea of "the area I mean"; stroke/marquee/layer-mask
  are editors of it, and the tinted view is UX, not data.
* **`seed` and the clock are parameters, never reads inside logic.** Anything nondeterministic takes a seed
  and a clock; that is what makes the queue and the ETA testable at all.

## Things that bite twice

* A claim in docs that cites a tool means the tool is committed and registered as a test.
* A test that is never called must be a build error — the anonymous namespace closes before `main()`.
* Re-capture golden screenshots with `wire_drive.py --update-golden` and look at them; a golden that was
  never looked at is a lie with a checksum.
* Prefer the `edit` tool over blind `str.replace`; verify a claim about the tree with `grep` before it goes
  into a doc or a commit message.
* After any probe or test run: `ps -eo args | grep -E 'noise-edi|Xvfb'` must come back empty. Never clean
  up with a broad `pkill -f` — it can match the shell running the cleanup.
* The box is shared and a model swap costs minutes: live tests stay small and restore what they switched.

## The tuning pass, specified before more images go out

Two recipe questions are open. They are measurement, not code, so they get a probe with a cap and a written
method — and **nothing they measure is ever asserted**, because a diffusion answer is not a golden file.
What the probe asserts stays the invariant set `ai_job_probe` already asserts per image; the tuning output
is numbers into `docs/probes/`, and a wording or radius then changes in `assets/recipes/*.json` because of
them.

* **A — Remove's wording, 3 images.** A plain `Remove the marked content` answer once cloned nearby texture
  instead of continuing it. Three candidate `prompt`s, same scene, same seed, 256×256: the current sentence;
  one adding `continue the existing texture and lighting; do not duplicate objects`; and a minimal
  `Remove the marked object.` Per answer: **seam contrast** (mean gradient magnitude on the coverage
  boundary ring), **repetition** (off-centre autocorrelation peak inside, against the same statistic on the
  context ring — a clone spikes, a continuation does not), and the **drift ratio**. Lowest seam-and-
  repetition wins, *only if* the high-frequency ratio below stays up, so "smear it flat" cannot win.
* **B — context radius against hole size, 4 images.** `prep.radius_px` {12, 24} × marked area {90×90,
  150×150}. A radius may recover context and must not soften the answer: **high-frequency ratio** =
  interior gradient energy / context gradient energy, near 1 required (`region_json`'s flat-region tolerance
  is 0.06, so under ~0.7 is a visible soft patch). Recorded as a table; if the big hole wants a different
  radius, the recipe grows a rule rather than a second constant.

8 of the approved 10 images, 2 held to re-run whichever cell looks like noise. One scene, one model, fixed
seed, resident model put back unpinned. Timings recorded, never asserted.

## Environment

* Build and gate: `cmake -B build -G Ninja && cmake --build build && xvfb-run -a ctest --test-dir build`.
  Release tree is `build-release`. `-Werror` with `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`;
  `xvfb-run -a` is required for the app tests. Commits take `--no-gpg-sign`.
* Live tier: `cmake -B build -DNOISE_LIVE_TESTS=ON && xvfb-run -a ctest --test-dir build -L live`, with
  `$THENOISE_URL` / `$THENOISE_API_KEY` exported (the dev box). Ask before spending more than ~10 images
  and name the cost.
* Definition of done: builds warning-free · a test that would fail if the feature broke · a wire command
  for any new observable state · docs still true · no new duplicated constants or default arguments · and
  this file saying so.
