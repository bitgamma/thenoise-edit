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

6. **The UI for it** — the Generate panel, `Remove`/`Replace` on the selection, the model-swap confirm with
   the ETA `server.state` already carries, and the jobs tray. Every one of these is a caller of a command
   that exists; none of them needs new AI code. Start where the P0 flow starts: select → `Remove` →
   `ai.preview` in the confirm → `ai.run` → `job.apply`.
7. **Live run of the app path** (`ai_job_probe.py --live --max-images 2`) on the dev box, then the first
   recipe-tuning pass. This is the one thing that can change a decision rather than confirm one: prep
   choice per recipe, `context.ratio`, whether `colour_match` should default off for `new-image` (it does),
   and what an ordinary edit costs. Ask for `$THENOISE_URL` / `$THENOISE_API_KEY` before starting; ~2 images
   for the probe, ~10 for a tuning pass.
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
