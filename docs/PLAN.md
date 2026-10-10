# Plan — where the build stands

Working note for the next session. Everything below was observed, not assumed; re-check what matters
before building on it. Git has the history, `docs/probes/` has the measurements, `command.list` has the
live wire surface, `AGENTS.md` has the house rules. This file keeps only the reasoning those cannot
carry, the decisions, and the order the rest goes in.

Gate: `cmake -B build -G Ninja && cmake --build build && xvfb-run -a ctest --test-dir build` →
**26/26 green in Debug and Release**, warnings are errors. Release tree is `build-release`. Every suite
passes under `xvfb-run`; `test_ai_compose_probe` and the two wire-driven ones need a display and skip
loudly without one.

## Where the AI path stands

Built, hermetic, no network in the default `ctest` run:

| piece | what it is | checks |
|---|---|---|
| `core/blur`, `core/resample` | the two pixel primitives the AI path needs and the app can reuse: 3-pass odd box blur / `feathered_mask`, and Lanczos-3 over premultiplied alpha | `test_blur` 81, `test_resample` 625 |
| `ai/region` | the region compiler — geometry (`bbox → grow → clamp → snap → fit`, inverse map) and prep. No document, no request | `test_region` 105 |
| `ai/compose`, `ai/colour_match` | the result compositor — resample onto the crop, colour-match, feather, lock band, clip to layer alpha | `test_compose` 235 762 |
| `ai.compose` (wire) | lands a result file on the document through `Document::paint`, or previews it and reports what the policy resolved to | `test_app` 56 273 |
| `tools/ai_compose_probe.py` | the same, on the running app, with the report checked against the exported pixels | `test_ai_compose_probe` |

In order from here:

3. **Roles, recipes, field policy** (`ai/registry`, `ai/recipe`, `ai/provider`) over a **recorded**
   `/v1/models` body: `roles.json` and `recipes/*.json` read with the JSON reader already in the tree,
   then resolution order, `size_policy`, the tier-1 field set, and the builder validating its own values
   — the server answers `200` to `seed: "not-a-number"`, so nobody downstream checks. Field policy is
   asserted on serialized bytes, no test doubles in the request path. Recorded fixtures are the hermetic
   tier; the default run stays offline.
4. **Transport** (`ai/http`, `ai/queue`) — health, models, load, edits/generations/upscale, cancel, the
   adaptive timeout, the latency EMA that must not learn from a server-cache hit, the durable queue,
   `ai.log`. The only piece that touches the world, and where **libcurl** arrives (see decisions).
5. **The first command end to end** — `inpaint-remove` against the recorded backend, then against the dev
   box: preview, apply through the document doors, wire commands for each step. The apply half already
   exists as `ai.compose`, so this slice is really the request half plus the job around it.

**What the request side still needs** (each found from the other end, not guessed):

- `Document` has no **flatten over a rectangle of the visible layers at or below the active one**.
  `prep_region` takes a caller-supplied `base`, and the only flatten that exists is the whole-document
  composite — which would bake a layer *above* the active one into the patch and then write that back into
  the layer below. This is the first thing slice 5 has to build.
- The crop's alpha flattened over a colour sampled from the mask ring (`AI-BACKEND.md`, Region Compiler
  step 6, marked there as not built).
- `prep_region` returns `plan.crop`-sized straight RGBA bytes; the resample to `plan.requested` and the PNG
  encode belong to the request builder. Kept out of `prep` deliberately so the prep can be checked against
  the statistics it is supposed to preserve.

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

## Decisions that stand

- **`.ora` out** of the product and the design: one semantics for one stack. Flat PNG/JPEG/WebP in, via system
  libjpeg/libwebp/libpng found by pkg-config — that dependency question is already answered.
- **libcurl joins with slice 4 and not one commit earlier**: an unused dependency is a second route to the
  network with nothing watching it, and slices 1-3 are hermetic.
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
