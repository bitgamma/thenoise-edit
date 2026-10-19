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
   history entry is the replaced stack itself - see *The stack becomes one layer* below). What the slice
   owes: moving and scaling a layer's picture, inline rename, and the panel's own row of buttons pinned
   clear of the scrolling card list. The panel is always visible: it is where a person goes to understand
   what an edit did.
6. **Blend, and images as input.** Drop an image → its own layer → *Blend* with no prompt repaints it into
   the scene on that layer. Needs the third recipe scope, `region: layer`, whose source is the layer's
   pixels and whose coverage is its alpha. *Blend* with a prompt, then control images (openpose), come in
   through the same door.
7. **Document geometry.** `doc.crop`, `doc.resize` for the canvas - with *AI fill* on the grown strip, the
   outpaint mechanism below - and *Image size* with a method: Lanczos, `AI upscale (fast)` (ESRGAN),
   `AI upscale (best)` (an edit model, so one `roles.json` row and one recipe). The seam to think about
   first is history: `Document` fixes its `Extent` at creation today, and the journal holds full-size
   snapshots, so a size change needs a decision about what undo means across it.

   **Outpaint, the mechanism.** Grow the `Extent`, mark the grown border as the coverage, compile the
   region with the original frame *inside* it as context, and let the answer land on a new layer clipped to
   the grown strip, so the original pixels stay byte-untouched. It is `reconstruct` with a different mask
   story, which is why it wants its own `operation: extend` rather than a weakened call.
8. **The tuning pass and the parked niceties.** Recipe wording and `prep.radius_px` per verb against hole
   size (specified at the end of this file, needs the box), the `enhance` prompt-improver behind its own
   button, the keymap file, autosave.

Seed is not a user-facing concept: **Retry** re-asks the same request with a different seed and lands
another layer. Roles and recipes are ours to pin for now - the sets are in `assets/roles.json` and
`assets/recipes/`, loaded from disk, and the plan is that a person edits those files, not a dialog, so
nothing here needs a settings UI to be foreseen later.

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

**`Applied::structure` is the right flag here, and `coverage` is the wrong one.** A mask change moves no
pixel of any layer, but it changes what the composite shows everywhere the layer reaches — which is what
`structure` already means, and what makes `Document::undo` widen the pending region to the whole canvas.
Reusing it is a fact, not a fudge: `Applied::coverage` says "no texture was repainted", which is exactly
false for a mask.

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

**The step moves coverage and nothing else**, which is a fact the renderer needs: `Applied::coverage` is
set and `structure` is not, so `Document::undo` neither widens the pending region nor calls
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
says it is, the rule the last card is already held to. **Not yet decided**: whether that row is pinned
outside the scrolling list, so the buttons for the whole stack are where they always are rather than at the
bottom of a scroll.

**Owed from here**: moving and scaling a layer's picture, inline rename, and `layer.remove` in the timeline
(`layer_absent` is the entry deletion always needed; the decision is whether deleting belongs in the
timeline at all). The coverage-undo decision further up is still the bigger of the two.

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
| `app/coverage.h` (`coverage.target`) | one coverage, two lifetimes: `cover` and the marquee write whichever `marked`/`mask` says, a gesture keeps the lifetime it was pressed on, marking writes no pixel byte and costs one undo step per gesture, and colour stays a tool rather than a target | `test_app`, `wire_input`, `mask_speed_probe` |
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
