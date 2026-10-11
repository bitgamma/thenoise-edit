# Plan — where the build stands

Working note for the next session. Everything below was observed, not assumed; re-check what matters
before building on it. Git has the history, `docs/probes/` has the measurements, `command.list` has the
live wire surface, `AGENTS.md` has the house rules. This file keeps only the reasoning those cannot
carry, the decisions, and the order the rest goes in.

Gate: `cmake -B build -G Ninja && cmake --build build && xvfb-run -a ctest --test-dir build` →
**31/31 green in Debug and Release**, warnings are errors. Release tree is `build-release`. Every suite
passes under `xvfb-run`; the probes need a display and skip loudly without one. The live tier
(`-DNOISE_LIVE_TESTS=ON`, label `live`: `tn_probe_live`, `ai_job_probe_live`) is not registered by
default and skips without `$THENOISE_URL` even when registered.

## Where the AI path stands

Built, and the default `ctest` run opens no socket — `NOISE_AI_FIXTURES` pointed at `tests/fixtures/http`
makes even the running app replay:

| piece | what it is | checks |
|---|---|---|
| `core/blur`, `core/resample` | the two pixel primitives: 3-pass odd box blur / `feathered_mask`, and Lanczos-3 over premultiplied alpha | `test_blur` 81, `test_resample` 625 |
| `ai/region` | the region compiler — geometry (`bbox → grow → clamp → snap → fit`, inverse map), `prep_region`, `flatten_over_ring` | `test_region` 106 |
| `ai/compose`, `ai/colour_match` | the result compositor — resample, colour-match, feather, lock band, extent, blend | `test_compose` 312 629 |
| `ai/profile`, `ai/registry` | discovery parsing; `roles.json` ∩ `/models` → one model per role, with a reason and a fix command | `test_roles` 66 |
| `ai/recipes` | `recipes/*.json` and prompt substitution; the shipped set checked against the shipped roles | `test_recipes` 113 |
| `ai/provider` | the request builder and the field whitelist, asserted on the serialized bytes, plus the cache key | `test_provider` 115 |
| `ai/transport`+`http`+`http_curl`+`recorded` | the HTTP seam, the libcurl implementation, and the replay transport the tests use | in `test_backend` |
| `ai/server`, `ai/cache`, `ai/queue` | profile and latency EMA; request-digest blob cache; durable one-worker queue with cancel and retry | `test_backend` 232 |
| `ai.*`, `server.*`, `job.*`, `cache.stats` (wire) | the whole path as commands the UI and a driver share | `test_app` 56 448 |
| `tools/ai_compose_probe.py`, `tools/ai_job_probe.py` | the landing, and the whole recipe→request→job→landing path, on the running app | `test_ai_compose_probe`, `test_ai_job_probe` |

In order from here:

6. **The Generate panel** — the document-scoped recipes (`new-image*`, `reimagine`, `flatten-colorize`)
   on the same seam the selection panel uses (`RecipeSet` filtered by scope instead of by `selection`),
   with `job.apply {into: "new_layer"}` as the landing. Then the jobs tray: variants as `seed+i` jobs
   landing one by one, A/B wipe, Accept/Discard. Draft→refine stages ride on the same queue; the EMA the
   ETA needs is already measured and read.

**Done since then, in one line each.** Remove/Replace/Fill/Heal/Upscale on the selection have a panel
("Edit selection", tabbed with Layers and Log so the canvas keeps its area - the golden capture proves
it), driven end to end by one command, `ai.intent`, which is also what the tests drive: intents come
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
- **How far a write may exist is a mask, not a mode** (`extent`, multiplied into the coverage after the
  feather): an edit passes `alpha_coverage(layer)`, a generation the canvas filled. The compositor's rule
  stays "blend by coverage", so `inpaint-remove` and `new-image` differ by data they carry and nothing
  branches on the operation — and `extent = alpha ∩ layer.mask`, which a boolean could not have expressed,
  comes free. Two things about it are load-bearing: it is multiplied **after** the feather (before would
  bleed coverage past the silhouette the caller confined the write to — `test_compose` kills that mutant),
  and the recipe will name it (`composite.extent: layer|canvas`) rather than the code guessing.
- **Lab conversion is private to `ai/colour_match`.** One consumer, so it stays there; a second one pulls it to
  `core`, and adding the seam now would be the wrong kind of clean.

**Architecture verdict (AGENTS §1).** The compositor takes `(target, result, plan, coverage, extent, policy)` and the
app can already reach it through the command table, so slice 5 lands in the same place rather than routing
around it — the two narrowings found are both on the *request* side (the flatten door, the ring-colour alpha)
and neither forces a change to what is built. Nothing here is a second representation of an existing idea: the
coverage is the document's `Mask`, the blend is `restrict_to`, the blur and resampler moved into `core` when a
second caller appeared (`ai/region` prep and `ai/compose` share `box_blur`). `core` grew exactly two things for
this slice — the `PaintFence` argument and the `alpha_coverage` producer — and both grew because a second
*kind* of writer arrived rather than a second writer: a stroke and a composite disagree about the one thing
the door used to decide for itself, and an edit and a generation disagree about how far their pixels may go.

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
  a layer *above* the active one into the patch it uploads and then write that back below it. `prep` and the
  ring colour are callers enough to justify it; a whole-document composite would have been the lazy wrong answer.

**Architecture verdict.** The queue never touches the document — it keeps a `Landing` (crop, requested,
canvas, coverage PNG path, extent, policy, target layer, the pre-edit composite checksum) and the *app*
applies it through the same `land_answer` door `ai.compose` uses, which is why `test_backend` runs the whole
queue with no document in the process and why `job.apply` cannot disagree with `ai.compose` about feather or
extent. `ai/` stays free of `app/`, `core/` gained nothing new this slice, and the only app-level object is
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
- **A read must be free, so the preview is compiled by whoever changed something.** The panel calls
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

**A panel that only reacts to its own widgets is a dead end.** `Run` stayed greyed after a rectangle was
drawn, and only moved when the combo was switched: the preview was compiled `if (!args.empty())`, and a
marquee on the canvas passes no arguments to anything. The rule is now that an argument always recompiles,
and a change the command was not told about recompiles **once its key has held still for one read** - so a
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
