# Plan — where the build stands

Working note for the next session. Everything below was observed, not assumed; re-check what matters
before building on it. Git has the history, `docs/probes/` has the measurements, `command.list` has the
live wire surface, `AGENTS.md` has the house rules. This file keeps only the reasoning those cannot
carry, the decisions, and the order the rest goes in.

Gate: `cmake -B build -G Ninja && cmake --build build && xvfb-run -a ctest --test-dir build` →
**32/32 green in Debug and Release**, warnings are errors. Release tree is `build-release`. Every suite
passes under `xvfb-run`; the probes need a display and skip loudly without one. The live tier
(`-DNOISE_LIVE_TESTS=ON`, label `live`: `tn_probe_live`, `ai_job_probe_live`) is not registered by
default and skips without `$THENOISE_URL` even when registered.

## The interface is being re-aligned — read [TOOLS.md](TOOLS.md) first

The human gave the concept in prose and it was agreed in the abstract on 2026-10-11: AI is not a tab,
each tool gets an AI action, the mask (however it is drawn) is the user's one way to say "the area I
mean", an answer always arrives as a **new layer**, and the verbs are Remove / Fill / Change / Generate /
Extend / Upscale - with `Replace` deliberately absent, because it is two operations wearing one word.
The decisions table, the flatten boundary, the outpaint mechanism and the slice order are in
[TOOLS.md](TOOLS.md); it wins over older prose elsewhere until the code catches up.

## Where the AI path stands

Built, and the default `ctest` run opens no socket — `NOISE_AI_FIXTURES` pointed at `tests/fixtures/http`
makes even the running app replay:

| piece | what it is | checks |
|---|---|---|
| `core/blur`, `core/resample` | the two pixel primitives: 3-pass odd box blur / `feathered_mask`, and Lanczos-3 over premultiplied alpha | `test_blur` 81, `test_resample` 625 |
| `ai/region` | the region compiler — geometry (`bbox → grow → clamp → snap → fit`, inverse map), `prep_region`, `flatten_over_ring` | `test_region` 1 279 |
| `ai/compose`, `ai/colour_match` | the result compositor — resample, drift measured against the document and corrected on the answer, feather, lock band, blend through `restrict_to` | `test_compose` 237 643 |
| `core/history`, `Document::add_layer_written` | an edit that creates a layer: creation and pixels are one undo step, and undo takes the layer away again | `test_history` 221 |
| `ai/profile`, `ai/registry` | discovery parsing; `roles.json` ∩ `/models` → one model per role, with a reason and a fix command | `test_roles` 66 |
| `ai/recipes` | `recipes/*.json` and prompt substitution; the shipped set checked against the shipped roles | `test_recipes` 175 |
| `ai/provider` | the request builder and the field whitelist, asserted on the serialized bytes, plus the cache key | `test_provider` 115 |
| `ai/transport`+`http`+`http_curl`+`recorded` | the HTTP seam, the libcurl implementation, and the replay transport the tests use | in `test_backend` |
| `ai/server`, `ai/cache`, `ai/queue` | profile and latency EMA; request-digest blob cache; durable one-worker queue with cancel and retry | `test_backend` 363 |
| `ai.*`, `server.*`, `job.*`, `cache.stats` (wire) | the whole path as commands the UI and a driver share, verbs included | `test_app` 56 683 |
| `tools/ai_compose_probe.py`, `tools/ai_job_probe.py` | the landing, and the whole recipe→request→job→landing path, on the running app | `test_ai_compose_probe`, `test_ai_job_probe` |

In order from here:

1. **Auto-land, so rule A means what it says** — `Landing{auto_land, landed}` durable in the queue
   record, one idempotent app beat called by every door that pumps, and `tools/wire_input.py` asserting
   the flagship from the marquee to the new layer through injected X events with nothing pressed in
   between. Design in "What rule A still owes".
2. **Fill and Change verified as typed sequences** - the boxes exist; the probe that runs each end to
   end on the recordings and reports the layer it made does not, and the words a recipe asks for are
   the only place a run can still silently mean nothing.
3. **Generate with aspect presets** - `Ctrl+G` runs `new-image` today and lands a new layer; what is
   missing is the aspect choice (`1:1, 3:4, 4:3, 16:9` at the model's native scale) and the two style
   variants reachable from the panel rather than only from the wire.
4. **Extend** - the canvas mechanism: resize anchored, the original frame cut out of the answer, and the
   `extend` operation, so outpainting lands on a new layer with the existing pixels byte-untouched.
5. **Upscale / resize**, and selection tools the P0 still lists (lasso, wand, feather, expand/shrink).
6. **The jobs tray** - variants as `seed+i` jobs landing one by one, A/B wipe, Accept/Discard;
   draft→refine stages ride on the same queue and the EMA the ETA needs is already measured and read.

**Done since then, in one line each.** Remove/Replace/Fill/Heal/Upscale on the selection got a panel
("Edit selection", tabbed with Layers and Log so the canvas keeps its area), driven end to end by one
command, `ai.intent` - since replaced by the verb rows in the Tool panel, see "the verbs, and what the
window is now", which is also what the tests drive: intents come
from the recipe table (`label`, `ask`), the compiled request and its cache digest are shown before Run,
a model swap is priced and confirmed, and Apply lands the answer through the same door as `ai.compose`.
Verified on the wire and by looking: `select.rect` → `ai.intent {vars}` → `ai.intent {run}` → `job.wait`
→ `job.apply` on a fixture server and in the window, with the panel read before and after
(`the region moved / recompile` after the write, `stale` true, undo restoring the document).
7. **The recipe-tuning pass**, now that the app path is measured live
   (`probes/2026-10-11-ai-job-live.md`: 3.4 s for a 256² edit on a resident Klein-9B, 0.7 s when the
   server's own cache answered, and the landing exact). What the picture still shows is **texture**: the
   replacement carries a finer grain than its frame (L std 4.4 against 2.0) and a faint rectangular edge
   survives the 8 px feather. So the questions are `prep` per recipe (`blur` at small crops is the suspect),
   `context.ratio`, and whether `inpaint-remove` wants more steps below ~384 px. ~10 images; ask first, and
   the probe is the harness — it already re-lands the same request and re-measures.
8. **The pieces of the design nothing has asked for yet**: the `enhance` role (a role with no preference
   list, refused by every recipe), warm-up after a swap, tier-2 fields with a per-server gate, `ai.log` as a
   rolling file, the keyring/0600 config for the key, and the in-process `heal` fallback. Each is listed in
   `AI-BACKEND.md` next to the class or recipe it belongs to; none is a seam the built path routes around.

## Reasoning the code cannot carry (the compositor slice)

- **Pixel-lock is asserted against the *effective* coverage** — feathered, lock-banded, alpha-clipped — which
  `ComposeReport::coverage` returns. Asserting against the mask that was *sent* would be asserting against an
  intention; the report is the thing that ran. Whole-image diff, outside the crop too, never a sampled point.
- **The blend has one home.** The mix is `Mask::restrict_to`, the same premultiplied write-door rule a stroke
  goes through; it is exact at coverage 0 and 255, which is what makes the lock invariant provable rather than
  approximate. A second blend beside it is the way this breaks.
- **The inverse map is two rectangles, not a scale.** The round-trip test — written from the compositor's side,
  which is why it caught it — showed the documented `scale` + rounding-error form was wrong: the answer covers
  the whole crop and arrives at `requested`, and rounding up to the size multiple makes those two
  non-proportional, so mapping by scale drifts by exactly the rounding the compositor no longer performs.
- **The lock band bites only at crop edges that have canvas outside them.** At a crop edge that *is* the canvas
  edge there is nothing to protect, and a band there eats into the region the mask covers — the opposite of its
  purpose. `AI-BACKEND.md` said "outer N px of the crop rect" and now says this.
- **Auto feather comes from the mask's mean thickness** (`2 · area / perimeter`, the band width the ants'
  boundary runs already give) `/ 8`, clamped to 2..8 px. Region size is not the input: a crop is grown and
  snapped, so a small mask in a large crop would be handed a feather far wider than the feature it is meant to
  hide. Half a radius lands on each side, so a feature ends up at most a quarter of its own width in transition.
- **The resample is unconditional.** At 1:1 it is byte-exact because the kernel is interpolating and the weights
  are normalised among themselves, so the compositor never branches on "already the right size" — a branch there
  is one more path that can silently differ from the pixel-lock proof.
- **`answered_as_requested` is reported, not enforced.** The engine answers a snapped size and the resample
  copes; absorbing that silently is where a drift investigation would have to start over.
- **The destination is not the reference.** `compose_result` is given `target` — what the answer is blended
  onto, a fresh empty layer under rule A — and `reference`, the document the region was compiled from, and
  measures colour drift against the second while mixing with the first. One image serving both roles is what
  the build did while answers landed on the layer they came from; the moment that stopped being true the
  correction had no colour to disagree with, reported "no drift", and returned the dark patch a person
  reports as "it put a darker area there". `test_compose` pins both halves: an edit that changed only the
  subject is left exactly alone, and a drift over an empty destination is still measured and still taken off.
- **Coverage is the only restrictor.** The build had a second mask, `extent` — the active layer's alpha for
  an edit, the canvas filled for a generation, named by `composite.extent` in the recipe — multiplied into
  the coverage after the feather. Rule A removed its reason for existing: an empty destination has no
  silhouette to protect, and what was left of the mechanism could only discard correct answers (the probe of
  the time reported `0 changed, 8 992 clipped` for an answer that was fine). Deleting it took the recipe key,
  the wire argument, the report counter, the queue's copy of it and `core`'s `alpha_coverage` producer, so the
  number of places that get to say "how far may a write exist" is one again.
- **Lab conversion is private to `ai/colour_match`.** One consumer, so it stays there; a second one pulls it to
  `core`, and adding the seam now would be the wrong kind of clean.

**Architecture verdict (AGENTS §1).** The compositor takes
`(target, reference, result, plan, coverage, policy)` and the app reaches it through the command table, so the
next slice lands in the same place rather than routing around it. Nothing in it is a second representation of
an existing idea: the coverage is the document's `Mask`, the blend is `restrict_to`, the blur and resampler live
in `core` because two callers share them (`ai/region` prep and `ai/compose` both use `box_blur`). `core` grew
one thing for the composite path — the `PaintFence` argument — and it grew it because a second *kind* of writer
arrived rather than a second writer: a stroke and a composite disagree about the one thing the door used to
decide for itself. The other thing `core` grew for it, `alpha_coverage`, was deleted with the extent it served,
which is the check that it was never load-bearing on its own.

### The one thing that was wrong, and how it was found

The compositor's unit tests were green and its report was a lie. The probe drove the real app — fixture
scene with a disc in it, the same scene without the disc as the "answer", the document exported before and
after — and the counts disagreed: 25551 pixels reported changed, 19579 in the document. `Document::paint`
fenced the write by the **selection**, exactly as a brush needs, and the fence cut the feather off. So the
soft edge came back hard, and the report described an edit the document had partly thrown away.

`Document::paint` now takes the claim explicitly (`PaintFence::selection` for a stroke,
`own_coverage` for an edit that arrives with a coverage of its own), because "may this write reach past the
selection?" is a property of the *edit*, not of the door, and a door that guesses will guess wrong for
whoever writes next. The undo step was always recorded against `bounds`, so unfencing costs nothing there.

The same looking found `colour_matched`, which reported whether the correction *ran* while sitting one
letter from the policy flag `colour_match`, which says whether it was *asked*. It is `colour_corrected`
now: reporting a request as an effect lets a probe believe a match happened when the patch already agreed
with its ring.

Measured on the live app, worth keeping as the reference numbers for the first recipe: auto feather picked
8 px for a 140×140 mask and its transition measured ~11 px on each side of the selection edge; the ring
corrected a deliberate cast from a mean 25.7 units off the true background to 1.7; the exact answer landed
with a worst-channel error of 0.5. **Settled by the human: 8 px is the right ceiling.** A colour step as
large as the probe's deliberately paints shows a Mach-band edge at that width, and the answer is the
recipe naming a wider `feather_px` for a job that expects a big colour change — not a larger default, which
would cost every ordinary edit a quarter of its feature width.

**Red-check of the slice.** 24 mutants over `compose`/`blur`/`resample`/`colour_match`: **22 killed**, 0
anchors unusable, re-run against the final tree. The 2 survivors are equivalents, and the reason matters
more than the count:

- dropping the "coverage has no pixels" guard — the empty-bounds refusal fires one line later, so only the
  error *text* changes; a test would pin wording, not behaviour.
- dropping the "no alpha ⇒ no colour" guard in the resample's down-pass — with alpha 0 the code then divides by
  zero and `lround` of `inf`/`NaN` is unspecified, landing on 0 here. The guard removes arithmetic that is not
  defined, not an observable byte, so a test could only pin this platform's arithmetic.

Four more mutants for the door: fence everything, fence nothing, the composite asking for `selection`, the
stroke asking for `own_coverage` — all four red, the middle two on `test_document` and `test_app` together.

A timing guard in the same run was alarming on a loaded box and is recalibrated the way the lesson below
says: it had demanded 50× when the code's real Debug ratio is ~25×, and the walk it guards is bounded — the
cost is `refresh` re-tallying the rows a write touched. Factor 4 now, with 3× headroom either way, 60 Debug
runs and 15 under 24-way load clean, the degenerate walk still red.

## Reasoning the code cannot carry (the request and job slice)

- **A request has one representation.** `BuiltRequest` used to carry a JSON object *and* a multipart field
  list, filled in parallel: for `/images/edits` the JSON body was built, shown in the preview, and never
  sent — a preview describing bytes that do not go out is exactly the kind of doc-lying that costs a day
  later. Now it is one ordered `fields` list plus the PNG, and `json_body()` and the multipart parts are
  both encoded from it. `multipart()` is the only branch, and the transport owns it.
- **`prep_region` takes the region as its own image, not a canvas and a rectangle.** It used to index
  `base.at(crop.x + x, crop.y + y)`, and the app hands it a crop that `Document::flatten_over` already cut:
  correct for every unit test (which all put the crop at the origin) and an out-of-bounds read the moment a
  selection sat anywhere else. The probe found it by crashing the app on a real selection; `test_region` now
  preps the same pixels at the origin and 16 px into a bigger picture and requires the same digest.
- **Classify a failure from the body, not from the endpoint.** A 400 from `/images/edits` was mapped to
  "this model cannot edit" because of its path, and the hint told the user to mark a working editor as
  incapable when the real cause was a missing field. Size, edit-capability and plain refusal now each read
  the error object's own words, and `recorded` fixtures hold all three shapes because the client branches on
  them.
- **The latency model must refuse to learn before it has a real sample.** A first reply inside the server's
  own cache ceiling used to *seed* the entry, so one cache hit promised five seconds for a five-minute job
  forever. The ceiling is checked before anything is learned, and `predict` answers nothing until a real run
  of that exact `(model, size, steps)` has happened on this box.
- **Cancellation is about the job, not the transfer.** A queued entry is dropped and never reaches the
  server (proved by counting requests per recording after the queue is joined); a running one is stopped by
  the flag the transport polls; a landed one answers `already succeeded; its result stays at <path>`. A
  cancelled entry is what `job.retry` is for, and retrying a *succeeded* job is refused — the same request
  can only be a cache hit, so a retry there would pretend to re-ask.
- **`RecordedTransport` is a server, so it is stateful and threaded and cannot be copied.** It hands out
  captures and hit counts while the queue's worker writes them: an unguarded read produced a test that
  passed with a `fprintf` in it and failed without one. Internal mutex, accessors by value, `unique_ptr`
  everywhere. A recording can also hold (`delay_ms`) and fail at the transport layer
  (`transport_error: cancelled|timeout`) — otherwise "queued", "in flight" and "cancelled" are states no
  hermetic test can reach, and those are the ones a user presses.
- **Two digests, two jobs.** `JobRecord::cache_key` is the hash of the *request* (what the cache is keyed
  on, and what the latency EMA records a timing against); `JobRecord::blob` is the hash of the *answer* —
  the one fact about the result that exists nowhere else, since the request's own blob id already sits in
  the stored preview.
- **A `JobQueue` cannot resume a request it did not finish**, so a record found `running` at open becomes
  `failed`/`interrupted` on disk immediately. The alternative is a job that looks alive forever, and the
  answer's path is kept in the record either way because an answer that arrived is never thrown away.
- **`Document::flatten_over(bounds, top_layer)` is a document door, not an AI one**: the region must not bake
  a layer *above* the active one into the patch it uploads and then write that back below it. It has a second
  load-bearing caller since answers started arriving on their own layer — it is the colour reference the
  landing measures drift against — which is the usual sign: the door that was right for the request is right
  for the answer too, because they are the same pixels seen from either side.

**Architecture verdict.** The queue never touches the document — it keeps a `Landing` (crop, requested,
canvas, coverage PNG path, policy, the layer the region was flattened from, the pre-edit composite checksum)
and the *app* applies it through the same `land_answer` door `ai.compose` uses, which is why `test_backend` runs the whole
queue with no document in the process and why `job.apply` cannot disagree with `ai.compose` about feather,
coverage or where the pixels go. `ai/` stays free of `app/`, `core/` gained nothing new this slice, and the only app-level object is
`AiSession` (tables, transport, client, cache, queue, registry), opened once by the host: commands read it,
nothing reconstructs it, and `server.state` reports whether the tables loaded rather than letting a command
refuse for a reason it does not name.

## Reasoning the code cannot carry (the selection panel)

- **One command owns the panel's whole surface.** `ai.intent` answers the intents on offer, the values in
  force, whether the action could run and why not, the compiled price and the job, and it takes the
  changes to all of them. The panel takes no `AppState`, so it has no private knowledge to be wrong
  about, and a scripted "remove the lamp" is the same sequence of table calls the buttons make. A second
  way to set the intent - a widget writing straight into state - is how a UI and a probe start meaning
  different things.
- **A read must be free, so the preview is compiled by whoever changed something.** *(Superseded: the
  read path now encodes nothing at all, and the `key`/`stale` machinery was deleted - see "what a frame
  may cost, and what a preview is".)* The panel calls
  `ai.intent {}` every frame; recompiling there would encode a region each frame. Instead the intent
  stores the request beside a key (`document revision`, `selection bounds`, `covered`, `active layer`) -
  the revision moves with every pixel write and the selection moves with none of them, so the pair
  notices both - and a reader that finds it out of date is told through `stale`, which the panel shows as
  the orange line rather than quoting a stale size and ETA. `run` refuses a stale intent, because a
  button that charges for a request the screen did not show is the worst possible surprise.
- **A refusal is not cleared by a read.** `intent.error` is cleared only when the call asked for
  something; a panel reading every frame would otherwise show the reason for a failed Run for one frame
  and then nothing.
- **The queue is pumped when its records are read.** `ai.intent` advances the queue before reporting the
  job row, or the panel would show a finished job as running until some other command happened to pump.
  (The same reasoning made `job.list`/`job.get`/`job.apply` pump; the panel is just another reader.)
- **The seed is session state, not an argument default.** `ai.preview` and `ai.run` used to require a
  `seed`, which is right about requests and wrong about people: nobody types a seed to remove a lamp, and
  inventing one inside the command would break reproducibility. `AiSession::seed` is the one field, the
  panel shows it, `server.state` reports it, and variants remain the explicit `seed + i` case. A default
  that lives in the request path is the bug this closes.
- **`arg_int` joins `arg_bool` and `arg_rect`: absent leaves the caller's variable.** A reader that
  defaults on absent is how the colour correction got switched off in the last slice; the family now has
  one rule, stated in the header, and `want_int` is for arguments that really are required.
- **`label` and `ask` are recipe data because the palette is derived.** `RecipeSet::selection_intents()`
  is "scope `selection`, endpoint `edits` or `upscale`" - adding an operation means adding a JSON file,
  and a recipe that stops being a selection operation disappears from the panel on its own. A
  hard-coded list of buttons would have been the same feature with a second source of truth.
- **Two buttons per frame is a bug, not a style.** `if (ImGui::Button("Run") && swap) ... else if
  (ImGui::Button("Run"))` draws two controls and swallows the first click, because `Button` draws as it
  asks. Draw once into a `const bool`, then branch.

- **The handshake belongs to a command; the *ask* belongs to the host.** `ai.intent {connect: true}` is
  the whole `Connect` button, so the wire starts the same session a person starts by clicking and the UI
  holds no socket and no belief about one. The automatic ask is in the frame loop, once, after a frame has
  been presented, and `server.connect` sets `connect_attempted` before anything can refuse - so "once"
  holds even when there is no transport to attempt with.
- **A docked tab that is not the visible one does not draw.** Two claims were written here before this
  one, both wrong, both from a single screenshot each time: first that the panel asks when it is looked
  at, then - after the app connected with *Layers* in front - that hidden tabs draw anyway. The truth is
  that the panel is the front tab for the first frames and `Layers` takes over after that, so a panel-driven
  handshake fired by luck of the layout, and a panel-driven *refresh* stops whenever the person looks at
  their layers instead. Behaviour a person is entitled to cannot be gated on which tab happens to be
  front: the ask moved to the host, where it now fires with any tab showing.

**Architecture verdict.** The panel added no AI code and no document surface: it reads `roles.json`
through the registry, compiles through `ai.preview`, queues through `ai.run` and lands through
`job.apply`. `ai/` gained two parsed keys and a filter over recipes it already held; `core/` gained
nothing. The next slice (Generate) needs the same filter with the other scope and the same landing door,
which is the test of whether this seam was cut in the right place.

**A blur that re-sums its window stops the window.** Drawing a marquee over a third of a photo froze the
editor, and the reason was arithmetic rather than AI: `box_blur` re-added all `2r+1` samples for every
output pixel, and the prep radius is `min(w,h)/12`, so prep cost grew as *area × min dimension* - 9.1 s
measured for a 1200x900 region, on the thread that draws. A running sum (slide the window, add one element,
drop one, edges counted at their clamped index) made it independent of radius: **0.20 s** for the same
region, **0.57 s** end to end for a 2500x2000 selection on a 4000x3000 document in Release. Output was
proved identical to the old arithmetic over 3456 shape/radius/pass/channel combinations before anything was
believed, because a blur that gets faster by changing pixels is a worse bug than a slow one.

**The queue's mutex did not cover what it handed out.** `JobQueue::latency()` returned a bare reference to
a `std::map` the worker inserts into when an answer lands, and the panel read an estimate from it every
frame - a tree walked while another thread rebalances it does not fail, it spins inside `find`. The fix is
in the types (`LatencyModel`, `BlobCache` guard their own contents) rather than in a comment telling callers
to be careful, because the comment was already there and was already being violated from the other side of
an API. ThreadSanitizer named it exactly (`data race ... LatencyModel::predict_locked`) with the reader lock
removed, and reports nothing with it in; that red/green pair is the new concurrency test in `test_backend`,
which asserts exact final values rather than merely not crashing.

**A panel that only reacts to its own widgets is a dead end.** *(The compile-when-settled answer below is
superseded; the lesson - a reader that only reacts to its own widgets is broken, and so is one that
re-encodes behind them - stands.)* `Run` stayed greyed after a rectangle was
drawn, and only moved when the combo was switched: the preview was compiled `if (!args.empty())`, and a
marquee on the canvas passes no arguments to anything. The rule then was that an argument always
recompiled, and a change the command was not told about recompiles **once its key has held still for one
read** - so a
dragged marquee does not encode a region per frame, and the price becomes correct a frame after the hand
stops. `stale` remains true during that window, and `Run` refuses a stale preview, so the panel cannot
charge for a request it did not show.

**Measure the build you mean.** The first numbers after the fix came from the Debug tree, which is 10-30×
this workload, and they said "still 3.8 s". Both trees are gated anyway, but a perf claim has to say which
one it came from, and `frame_ms` in a Debug run is not evidence about a Release hang.

**A test nobody calls is green in both directions.** Four suites closed their anonymous namespace above
their last tests, so a test function added at the bottom of `test_app.cpp` had external linkage and
`-Wunused-function` could not see it: the new `server.connect` refusal test compiled, sat there, never
ran, and the run was greener for it. Those namespaces now close before `main()`, which makes an orphan a
build error - confirmed by adding one and watching `-Werror` refuse it. A scripted `str.replace` whose
anchor has moved is the same shape and reports nothing either: an edit that *cannot* fail quietly beats
one that claims success, and a claim about the tree is worth a `grep` before it is worth a sentence in a
commit message. The same hole, one layer up: the panel's `Connect` was missing from the built app while
its command was tested and green, because nothing in a C++ test draws a window. `ai_job_probe` now checks
the app itself - `connect_attempted` and `connected` before anything in the probe asks - which is the only
defence against an edit to the host or to `ui.cpp` that compiles, tests green, and changes nothing a person
can use.

## Reasoning the code cannot carry (what an operation is)

Three separate facts were living in one field, and the UI inherited the confusion:

| fact | question it answers | home |
|---|---|---|
| the region | what does the model get shown? | `ai/region` `plan_region` |
| the hole | what is destroyed on purpose, inside the mask? | `Recipe::prep`, applied by `prep_region` |
| the coverage | where is the answer allowed to land? | the selection/mask, through `ai/compose` |

Blurring belongs to the middle row only. `prep` was a recipe-level field applied to the whole crop, so
`inpaint-replace` - "replace the eyes with green eyes" - uploaded a smeared face and returned something
unrelated to the picture, and the panel offered the same knob for both kinds of job. The concept that was
missing from the data is now in it: `Recipe::operation` is `reconstruct | in_place | transform |
from_words`, and the parser refuses a hole on anything but `reconstruct`, a `reconstruct` without a hole,
a `reconstruct` spanning the document, `from_words` on the `edits` endpoint, and a recipe that states no
operation at all. Parse-time, not runtime: a recipe that contradicts itself spends a person's time and the
box's quota every time it runs.

`role` is the other axis and shares no value with it - one role (`edit`) serves both `reconstruct` and
`in_place`. Naming `operation: edit` alongside `role: edit` was the first draft and it is exactly the kind
of two-names-one-word accident that makes a codebase lie later.

`prep_region(crop, coverage_in_crop, prep, seed)` now destroys the covered pixels and leaves every other
byte of the crop as the document holds it. `average` and `noise` fill from the mean of the **context**: the
hole's own mean is the colour of the thing being got rid of. The `noise` draw advances once per pixel
whatever the mask says, so a hole's noise depends on the crop's geometry and not on how much happened to be
covered. `tools/prep_proof.py` (registered as `test_prep_proof`, recorded transport, spends nothing) renders
each panel recipe's upload and asserts it whole-image: `in_place` 0 px changed, `reconstruct` 0 context px
changed and 16 384 / 16 384 of the hole destroyed (`blur` 16 204 - the rest is flat area a blur cannot
alter).

The colour correction had the same disease in the opposite direction. `colour_match_ring` pulled the answer
onto the Lab mean and sigma of the ring outside the mask, which for a face surrounded by dark hair and
background means: paint the face dark. It is now `colour_match_drift`, and the only thing it measures is
**the drift on the band the model was also shown** - the context the region compiler grew the box by,
paired pixel against pixel against the document - and subtracts that from the whole answer, spread included
and clamped. A correct in-place edit therefore gets no correction at all, which is the regression test
(`test_an_answer_that_changed_only_the_subject_is_left_exactly_alone`): 3 075 failures with the old
estimator put back, 0 with the new one. The live numbers in `probes/2026-10-11-ai-job-live.md` (9.0 L drift
with the correction accidentally off, 0.08 L with it) measure the estimator that no longer exists; the
paired one is owed one live run before the probe says anything about it.

Failure modes worth keeping:

- **A masked operation that writes through a shared plane** - the `noise` pass first wrote its jitter into
  the working plane for every pixel, so the context came out noisy and the "context is byte-identical"
  check caught it in the same run that added the check.
- **A fixture with nothing under the mask proves nothing**: a blur over a flat area is that same flat
  area, so "the hole's identity is destroyed" needs detail under the hole - the fixture hole straddles the
  edge of the two-tone image.
- **A colour estimator needs a subject with a different colour from its surroundings.** Every colour test
  here used flat fields until there was a bright patch in a dark frame; the flat ones could not see a patch
  painted dark.
- **Untracked tools disappear.** Four throwaway probe scripts this session were gone from the tree mid-task
  (shared box), and a claim in `docs/` that points at a tool is a claim about a file. `prep_proof.py` is
  committed and registered as a test; that is the bar for a tool a document cites.

## Reasoning the code cannot carry (an answer, its layer, and its undo)

Rule A of [TOOLS.md](TOOLS.md) — every AI answer arrives on its own new layer — looked like a two-line change
to the landing. It moved the ground under three things that had each assumed an answer landed *on* something.

**Fact table.**

| piece | what it is | how it is seen |
|---|---|---|
| `compose_result(target, reference, …)` | the destination and the colour reference, as two arguments | `test_compose`: drift over an empty destination is still measured, and the landed pixel moves toward the document, not toward nothing |
| `AnswerLanding{above_layer, layer_name}` | which layer the region was flattened from, and what the new layer is called | `job.apply` names it `Remove j0001`; `ai.compose` names it `answer <digest8>`; both report `layer` and `layer_name` |
| `Document::add_layer_written(name, bounds, write)` | creation and first pixels in one undo step, nothing added when `write` refuses | `test_history`, and `ai_compose_probe`'s layer count |
| `History` structural entries (`layer_created`, `layer_absent`) | an edit that adds or removes a layer is a step, not a write | `test_history`: one `Ctrl+Z` returns the layer *and* its pixels |
| `Document::retarget_active()` | undo/redo cannot leave `active_` pointing at a layer that is gone | `test_history`: `active` after undoing an answer |

**The reference is not the destination, and the tests found it before a person did.** The first version passed
the destination as its own reference, which was exactly right while an answer landed on the layer it came
from and silently wrong the moment it did not: drift is the paired difference on the context band, an empty
layer has no colour there, so the estimator reported "no drift" — the true answer to a question that no
longer made sense — and the dark patch the correction was built to remove came back. The lesson is not
"add a parameter", it is that a function whose two roles are played by one image has no way to say when the
identity stops holding. `ComposeReport` also gained nothing to detect it: `colour_corrected` was `false`, and
a `false` is only a finding if someone expects `true`, which is why the test asserts the correction runs
rather than that it does not crash.

**Two orders an undo has to get right, and neither fails loudly.** A step is a group of entries sharing a step
number. A structural entry must be applied **before** the pixel entries when it re-inserts a layer (the pixels
need somewhere to land) and **after** them when it removes one (their bytes go back first). Getting it
backwards does not raise: the pixel entry simply finds no layer. And the entry pushed onto the opposite stack
must carry the *new* step number — carrying the old one split the group in two, so redo re-inserted the layer
and left its pixels stranded on the other stack. Both bugs were invisible to the eye in the window and
impossible to miss in `test_history`.

**Pixel-lock changed shape, and one probe claim with it.** The invariant used to be "outside the coverage, the
document is byte-identical". It is now stronger where the answer is concerned and weaker where the export is:
the layer the answer was compiled from does not change at all, while the flattened export can differ in fewer
pixels than `pixels_changed` reports, because a pixel the model reproduced exactly is written without being
visible. `ai_compose_probe` and `ai_job_probe` therefore assert `0 < visible ≤ written` and `0 outside the
coverage` — a subset, not an equality. An equality assertion there would have been a lie in the new shape and
a passing one is only luck.

**Failure modes worth writing down because they are not obvious from the code.**

- A layer added *before* the compose runs leaves an empty layer named after a job that produced nothing every
  time the answer is refused for geometry. `add_layer_written` is why the refusal path adds nothing: the write
  happens inside the creation and a `false` rolls the whole thing back.
- `flatten_over` returns the part of the rectangle that met the canvas, region-relative, not a canvas-sized
  image. Landing that into a canvas-sized reference is an off-by-the-crop-origin write; the assertion in
  `Image::at` caught it as an out-of-bounds abort on the first probe run whose crop reached an edge, which is
  the only kind of catch worth relying on here.
- An answer whose crop lies outside the document after the user resized things must refuse naming the layer
  it was compiled from (`…is no longer in the document…`), not land somewhere plausible.
- The `extent` deletion looked reversible and was not worth reverting: a knob with one legal value is a
  parallel representation of a decision already made, and it kept the report field, the queue field and
  `core`'s alpha→mask producer alive for its sake.

## Reasoning the code cannot carry (the verbs, and what the window is now)

The interface stopped being a panel and became four keys. That moved no AI behaviour - it moved *where a
promise is stated*, which is where the design work was.

**Fact table.**

| piece | what it is | how it is seen |
|---|---|---|
| `app::kVerbs` (`ai_verbs.{h,cpp}`) | rows `{VerbId, recipe, key}` - the surface. Label, words and scope are read from the recipe, never restated | `ai.verbs` rows; `command.list` shows `ai.remove` bound to `Ctrl+Backspace` |
| `verb_table_error(recipes, kVerbs)` | each verb's contract over the recipe's facts, checked at session open | `test_app`: an operation swapped, an ask that grew, a recipe missing, two verbs on one recipe - each refused, all four rows then not-ready |
| `ai.<verb> {vars?, seed?, confirm?}` | one row of the table, as a command; it calls `ai.intent` with its recipe named | `test_app` runs the flagship: mark → `ai.remove` → `job.wait` → `job.apply` → a layer named `Remove j0001` |
| `missing_region(recipe, state)` | the region a recipe needs with nothing marked; the region door refuses on it and the readiness forecast asks it | `ai.verbs` rows grey with the reason; `ai.remove` refuses over an unmarked frame |
| `ai.verbs` row `swap`/`resident` | whether that verb would have to load a model first | the window's confirmation names both models before the press |

**A verb is not an operation and not a recipe.** `Recipe::operation` says what happens to the pixels;
`VerbId` says what a person means when they press something; the recipe is how the AI path compiles it.
Collapsing any two of the three is what produced the original bug: the panel offered "Replace", the
recipe did `in_place`, and the mask was treated as a hole. Keeping them separate costs one small table
and one load-time check, and buys the refusal that a row which drifted from its recipe cannot ship.

**The key lives in the command row, not the widget.** `kVerbs` carries canonical shortcut text into
`table.add`, and the window asks `commands.shortcuts_for(...)`/`ai.verbs` for what to print. A binding
that collided with another refuses the registration at startup, so a verb key cannot quietly lose to
`edit.undo`.

**`vars` now accepts two kinds of name.** A recipe's `ask` fields have no defaults by rule, so the
accept set is `variables ∪ ask` - and switching verbs forgets what the next one neither asks nor
declares, destructively: coming back to Change after Remove shows an empty box, which the test asserts
rather than smooths over. That is rule E behaving, not a bug to paper over with retained history.

**What the window is.** The "Edit selection" panel is deleted: the verbs are a section of the Tool panel
(one row each, keys beside them, reason on hover), the brush sliders appear only for a tool that paints
(`core::tool_mode` decides, the panel does not), and the AI line - verb, model, ~seconds, cache digest,
swap warning, job and Apply - sits on its own line in the status bar, because a bar that shares one line
with zoom/size/path/undo gives the AI facts a third of the width and a clipped swap warning is worse than
none. The status bar therefore has two lines, the canvas rect grew a little, and
`tests/golden/view_pan_zoom.png` was re-captured with `wire_drive.py --update-golden` (verified by eye:
same checkerboard at the same zoom, 680×481 instead of 680×445).

### Rule A landed: an answer lands itself

The `Apply` step is gone. Every succeeded job lands on its own layer as soon as it is finished, and the
two things that could go wrong with that are the design, not the plumbing:

| decision | why | how it is seen |
|---|---|---|
| `Landing::document_epoch`, stamped by `ai.run` from `AppState::document_epoch` (bumped by `note_new_document` in `doc.new`/`doc.open`) | "the answer lands by itself" cannot mean "into whatever file is open when the model finally answers". A job lands in the document that asked for it, or nowhere | `job.apply` over a foreign document refuses naming both epochs; the beat leaves it alone |
| `landed` + `landed_layer` + `report`, durable in `jobs.json` | reopening the app must not stack a second copy, and "the answer reached no pixel the mask did not claim" has to be checkable after the landing, whoever asked | `job.get` carries the landing and its compose report; the probe reads the report rather than pressing anything |
| one beat, `pump_ai_landings(state)`, called by every door that reads the queue (`ai.intent`, `job.list`, `job.get`, `job.wait`) | no door owns landing, so no door can be forgotten and none can double-apply | four reads after one `job.wait` leave the layer count where it was |
| **the beat does not re-land what undo took away** | an undo is a decision; an auto-lander that undoes the undo is a fight with the person using the app. `job.apply` is the door back, and it checks whether the layer it named is still there | the same test: undo → reads → still one layer; `job.apply` → the same pixels again |
| a refusal is recorded once (`land_refused` + `land_error`) | a beat that retried a refusal every frame would spend a composite per frame and bury the sentence in the log | `land_job` records both outcomes; a successful explicit `job.apply` clears a previous refusal |

`job.apply` survives as the idempotent version of the same door: "put that answer on the document" for a
job whose layer is still there answers with the landing's own report and adds nothing. Its `apply`
argument is deleted — there is no preview mode, because the layer is the preview — and there is no
destination argument either, which was already rule A.

## Reasoning the code cannot carry (the verbs, and what the window is now)

The interface stopped being a panel and became four keys. That moved no AI behaviour - it moved *where a
promise is stated*, which is where the design work was.

**Fact table.**

| piece | what it is | how it is seen |
|---|---|---|
| `app::kVerbs` (`ai_verbs.{h,cpp}`) | rows `{VerbId, recipe, key}` - the surface. Label, words and scope are read from the recipe, never restated | `ai.verbs` rows; `command.list` shows `ai.remove` bound to `Ctrl+Backspace` |
| `verb_table_error(recipes, kVerbs)` | each verb's contract over the recipe's facts, checked at session open | `test_app`: an operation swapped, an ask that grew, a recipe missing, two verbs on one recipe - each refused, all four rows then not-ready |
| `ai.<verb> {vars?, seed?, confirm?}` | one row of the table, as a command; it calls `ai.intent` with its recipe named | `test_app` runs the flagship: mark → `ai.remove` → `job.wait` → `job.apply` → a layer named `Remove j0001` |
| `missing_region(recipe, state)` | the region a recipe needs with nothing marked; the region door refuses on it and the readiness forecast asks it | `ai.verbs` rows grey with the reason; `ai.remove` refuses over an unmarked frame |
| `ai.verbs` row `swap`/`resident` | whether that verb would have to load a model first | the window's confirmation names both models before the press |

**A verb is not an operation and not a recipe.** `Recipe::operation` says what happens to the pixels;
`VerbId` says what a person means when they press something; the recipe is how the AI path compiles it.
Collapsing any two of the three is what produced the original bug: the panel offered "Replace", the
recipe did `in_place`, and the mask was treated as a hole. Keeping them separate costs one small table
and one load-time check, and buys the refusal that a row which drifted from its recipe cannot ship.

**The key lives in the command row, not the widget.** `kVerbs` carries canonical shortcut text into
`table.add`, and the window asks `commands.shortcuts_for(...)`/`ai.verbs` for what to print. A binding
that collided with another refuses the registration at startup, so a verb key cannot quietly lose to
`edit.undo`.

**`vars` now accepts two kinds of name.** A recipe's `ask` fields have no defaults by rule, so the
accept set is `variables ∪ ask` - and switching verbs forgets what the next one neither asks nor
declares, destructively: coming back to Change after Remove shows an empty box, which the test asserts
rather than smooths over. That is rule E behaving, not a bug to paper over with retained history.

**What the window is.** The "Edit selection" panel is deleted: the verbs are a section of the Tool panel
(one row each, keys beside them, reason on hover), the brush sliders appear only for a tool that paints
(`core::tool_mode` decides, the panel does not), and the AI line - verb, model, ~seconds, cache digest,
swap warning, job and Apply - sits on its own line in the status bar, because a bar that shares one line
with zoom/size/path/undo gives the AI facts a third of the width and a clipped swap warning is worse than
none. The status bar therefore has two lines, the canvas rect grew a little, and
`tests/golden/view_pan_zoom.png` was re-captured with `wire_drive.py --update-golden` (verified by eye:
same checkerboard at the same zoom, 680×481 instead of 680×445).

### What rule A still owes: the answer should land itself

Rule A says *no Apply step*. `job.apply` is gone as an **option** (there is no "into the active layer"),
but a finished answer still waits in the status bar for `Apply`, because nothing in the app lands it:
`ai/queue` deliberately cannot touch the document, and the doors that `pump()` only advance the queue.
The design, when this lands:

- `ai::Landing` gains `auto_land` (set by a verb press, not by a driver's `ai.run`, which still applies
  when it chooses) and `landed` - both durable in `jobs.json`, because "this answer was already put on the
  document" must survive the run that made it, or reopening the app stacks a second copy.
- One app-level beat, `pump_ai_landings(state, table)`, called by every door that pumps
  (`ai.intent`, `job.wait`, `job.list`, `job.get`); idempotent through the `landed` flag, so no door owns
  it and none can double-apply.
- `tools/wire_input.py` already injects the first half: a dragged marquee, then `Ctrl+Backspace`, and the
  job that appears is `inpaint-remove` (`j0001 ... state=succeeded` on the recordings) - the only way to
  test the host's own key handling. When auto-land lands, extend that section to assert what rule A
  promises with nothing pressed in between: a new layer whose name says `Remove`, and `Ctrl+Z` taking the
  layer and its pixels away. Until then "mark → key → layer" is tested to the queue, and the probe says
  so in its own comment rather than claiming the last step.

## Reasoning the code cannot carry (what a frame may cost, and what a preview is)

The verbs put `ai.intent` on the status bar, which is to say on every frame, and the canvas stopped: a
marquee over a 1600x1000 document ran at ~10 fps (`frame_ms` pinned at 100.0) where the same window had
run at ~250. The command auto-compiled the upload preview, its debounce counted the wrong thing, and two
readers per frame (verb rows, then status line) defeated it: the second ask settled the key the first had
just seen, so a region prep + resample + PNG encode - ~30 ms release, ~10x that in Debug - went onto every
frame of a drag. The panel had the same branch and nobody noticed, because it usually sat tabbed behind
Layers and Log.

Two fixes were tried in order and the second is the one that survived:

| attempt | frame_ms while dragging | upload encodes in a 120-step marquee |
|---|---|---|
| as shipped (settle counted in reads, two readers) | 100.0 | 95 |
| one reader per frame, settle counted in frames | ~37 | 95 — at 250 fps two frames is 8 ms, shorter than the gap between mouse moves |
| one reader per frame, settle counted in published `frame_ms` | ~196 fps | 0, then 1 once the drag stopped |
| **nothing on the read path encodes at all** (what shipped) | 4.1 idle, ~250 fps | **0**, at any frame rate, however long the drag |

**The rule the last line states.** Reading the AI action may plan geometry and read tables and nothing
else. `AppState::upload_encodes` counts the one place a region is prepped, resampled and PNG-encoded, and
`ai.intent` reports it, so "choosing not to run costs nothing" is an assertion rather than a claim:
`test_choosing_not_to_run_costs_no_upload_encode` marks an area 25 times, reads the action 75 times, and
requires the counter to not move; then `ai.preview` (+1) and `ai.run` (+1). Muting it - adding one encode
to the quote - turns 5 checks red. Debounce windows, settle constants and `steady_ms` were all deleted on
the way here: a window is a guess about cadence, and the invariant needs none.

**What a preview of an AI edit is not.** There is no cheap preview of an *answer*: the answer is what the
model takes 30 seconds to say. What `ai.preview` produces is the **request** - endpoint, the size the
region compiler settled on, the field list, the prepped upload PNG and its blob digest, the cache key. Its
value is that the upload is a claim about the document, and uploads are exactly where the bugs were (a
patch sent dark, a subject blurred because `in_place` was treated as a hole); `tools/prep_proof.py` renders
those uploads for every recipe and asserts whole-image differences, and that is how both were caught. So it
stays, where it always was: **a command a person or a probe asks for**, never something the frame does.
The window shows a `RequestQuote` instead - endpoint, size, model, steps, and the box's measured time for
that triple, all from geometry and tables - and the request's digest appears with the job that had it.

| piece | rule | how it is seen |
|---|---|---|
| `draw_ui` reads `ai.intent` once and passes the answer down | the window is one reader; one action, one read, one price | frame time while dragging; `upload_encodes` |
| `quote_request()` (`ai_commands.cpp`, where the region compiler lives) | what a press would send, computed without touching pixels | the status line's `272x224` and `~3.4 s` |
| `AppState::upload_encodes` | the cost of the read path is a number on the wire | the test above, red at 5 checks under a one-line mutation |
| `ai.preview` | the only thing that preps and encodes besides a run, and only on being asked | `prep_proof.py`, `ai_compose_probe.py` |

Nothing here is a rule about AI: it is that **a per-frame reader pays for whatever the command it reads
does**, so either the command is cheap by construction or the frame has to stop reading it. "Cheap when
nothing changed" is not cheap by construction; a debounce is a bet on how fast a person moves the mouse.

## Decisions that stand

- **`.ora` out** of the product and the design: one semantics for one stack. Flat PNG/JPEG/WebP in, via system
  libjpeg/libwebp/libpng found by pkg-config — that dependency question is already answered.
- **libcurl** (`pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)`) — approved by the human, and it
  arrived with the slice that speaks HTTP rather than before: an unused dependency is a second route to the
  network with nothing watching it. It is behind `ai/transport.h`, and nothing that a test runs reaches it:
  the recorded transport answers the same interface.
- **`.noise`: any new field bumps `manifest.json:format`** (currently **4**) and older files are refused by
  number. No optional fields under an old version.
- **Roles and recipes are JSON** (`roles.json`, `recipes/*.json`) like the manifest and the wire. Consequences
  written into `AI-BACKEND.md`: no comments, a parse failure names key and file, `prompt` is an **array of
  lines**, policy values keep their names (`context.ratio`, `composite.lock_band_px`).
- **Live tier**: runs only with `THENOISE_URL`/`THENOISE_API_KEY`, tagged `live`, `-DNOISE_LIVE_TESTS=ON`,
  `--max-images`; ask before spending ~10 images and name the tests and the cost. Assert contract and
  invariants, never model output; timings go to `docs/probes/`, never into an assertion. Which model answers a
  role **rotates with what the box has downloaded**, so nothing in code or docs may name one.
- **Never a URL, hostname or key in the repo** — docs, probes, comments, fixtures, commit messages. Write
  `$THENOISE_URL` and "the dev box".
- **CI waits until MVP**; the local gate above is the gate. `container_interop` needs a display: run the suite
  under `xvfb-run` or it skips.
- **The command table refuses an argument its command does not declare** (wire-contract change, approved). A
  silently ignored argument makes a probe lie, and it has.
- **Order set by the human**: masks, then painting masks, then AI-server editing; the lasso and the wand much
  later.

## Still open off the AI path

- `edit` group: `cut`, `copy`, `paste` (then `crop`/`flatten` when there is tool UI for them).
- Mask panel: enable/disable without deleting, apply (destructive), view a mask alone, and a thumbnail as a
  pointer target — reorder wants the same target, so do it once.
- Selection shapes: the lasso (a freehand path gesture, shaped like `stroke`, not like the marquee), and
  grow/shrink/feather as one pass over the plane; the wand is a seed plus the same patch hand-off in
  `core/maskops`.
- Cost, in the order the measurements say: per-layer content rectangles, GPU stamp preview during the drag,
  per-layer GPU textures plus the shader blend pair checked against `core/blend`.
- Deliberately not done: CI, the `enhance` role, the blob store (until history or variants must outlive a
  session).

## Things that bite twice

- **A test rectangle starting at the origin tests one corner of the addressing and nothing else.** Every
  mask-gesture test had `y = 0`, so a copy that forgot to subtract the destination's own origin survived the
  whole suite and killed the app on the first real drag. Gesture and region tests want a non-zero origin *and*
  a growth event.
- **An optional argument whose reader defaults it silently switches the feature off, and flat fixtures
  hide the fact.** `arg_bool` wrote `false` when the key was absent, so `job.apply` and `ai.compose` ran
  without the colour correction every edit recipe asks for — while `policy` in the report echoed whatever
  the caller had initialised, which is how it stayed green. A live answer was 9.0 L units lighter than the
  ring it landed in; the fix makes the same request land 0.08 L off. Two rules: an absent optional argument
  leaves the caller's value alone (the default belongs to the table, and the report must come from the value
  that ran), and a colour test needs a frame with structure, because matching a flat patch to a flat ring
  reproduces the ring and both settings then "work".
- **A test rectangle at the origin hides an offset bug once, and a second rectangle at the origin hides it
  twice.** The prep/region tests all had `crop = {0, 0, w, h}`, so `prep_region` adding `crop.x/y` to a
  crop-local image survived a green suite and asserted the app on the first real selection. Any function
  taking both an image and a rectangle should be tested with the rectangle off the origin, or should not take
  the rectangle at all (which is what it does now).
- **A test that passes when you add a print and fails when you remove it is a thread bug.** `hits()` on the
  recorded transport was read while the queue's worker still wrote it. Guard the state inside the transport
  rather than reasoning about when it is safe to look, and assert on it after the thread is joined when the
  claim is about what the server saw at all.
- **A test process that gives every harness the same directory makes the second harness destroy the
  first's** — `Harness` used `pid` alone, so a nested one wiped the job directory under a running queue.
  Per-instance, not per-process.
- **A module's own tests cannot see the door in front of it.** The compositor was green, and the document it
  wrote into was discarding its feather. Anything that crosses a seam needs one check driven through the real
  app — which is what a probe in `tools/` is for, and why `ai.compose` shipped with one.
- **Set a timing guard from the gap between implementations, not from the healthy ratio.** A factor that is
  true on a quiet box alarms on a shared one, and an alarm that is wrong once is a guard nobody reads. Take
  the fastest call of several on each side, measure the phases separately when one of them allocates megabytes,
  and say the numbers in the comment.
- **Our reader accepting our writer proves nothing.** Any new format surface needs a second implementation
  reading it (`tools/container_probe.py`).
- **"Slower" names a symptom, not a stage.** The reported bug was mask painting; the paint cost 0.01 ms and a
  panel readout cost 32 ms. Time the stages before changing any of them, and report the case that got slower.
- **A checksum that forgets a plane makes every round-trip check built on it weaker than it reads.**
  `Document::checksum()` omitted masks for the whole time they existed.
- **Docs that name modules the tree does not have are a live hazard.** `ARCHITECTURE.md` listed
  `ai/compiler` and `ai/composite`; building `ai/region` and `ai/compose` under the real names and leaving the
  doc alone would have sent the next session looking for files that never existed. Fixing the doc is part of
  the change.
- **A mutation harness keeps the file contents it read at start.** Do not edit product sources mid-run — it
  writes them back after every mutant and silently reverts the edit. Rebuild objects as well as sources when
  restoring, and never believe a red you did not rebuild for. `build-failed` is not a caught mutation: anchors
  that stop compiling or never matched need rewriting.
- **`app.state.canvas` includes the void outside the document.** Counting pixels over it counts what was never
  drawn: measure the document rectangle.
- **`app.state.stroke` is cleared on release**, so a gesture's `plane`/`dabs`/`marquee` must be read between the
  last move and the release.
- **A stray `noise-edit` makes `test_input_gestures` red**: XTest goes to the focused window. `pgrep -f
  noise-edit` before chasing one.
- **A `view.set` before the app's first fit is overridden by that fit**; derive the mapping from the capture
  instead of from the reply. Pixel-index arithmetic in a probe is as load-bearing as the code it measures — print
  the image size next to any bbox so an impossible number cannot pass for a finding.
- `nlohmann::json::contains()` is always false for arrays. `app.state.frame_ms` includes the swap wait: read
  per-command wire timings for work, `frame_ms` for cadence.

## Environment

- No `DISPLAY`; a Wayland var may be set with no compositor → `xvfb-run -a`.
- `XDG_RUNTIME_DIR=/run/user/1000` is **not writable** here; anything writing runtime files needs
  `NOISE_RUNTIME_DIR`. `test_app` injects its own, `wire_drive.py` uses a temp dir.
- Killed runs can leave Xvfb holding a ctest output pipe, which looks like a hang: `pkill -x Xvfb` (the `-x`
  matters — `pkill -f Xvfb` matches your own shell).
- Commits need `--no-gpg-sign` (`commit.gpgsign=true`, no signing key here).
- `cmake --build build && ./build/tests/test_<name>` runs any windowless suite standalone;
  `NOISE_RUNTIME_DIR=/tmp/nr ./build/noise-edit --width=1000 --height=620 some.png` under `xvfb-run` gives a
  live app to drive on the wire.
