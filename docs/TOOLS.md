# The tool-first model (agreed, being built)

This is the concept the interface and the AI path are being re-aligned to. It is not a UI note: several
decisions below change `ai/`, the landing door and the command surface. Anything in the other docs that
contradicts this file is the older text and should be corrected as the code that implements it lands.

## The concept

AI editing and generation are **first-class editing commands**, not a mode. There is no "AI tab": the tools
that already exist get an AI action where one makes sense, and the user works with concepts they already
know (erase, paint, resize, extend a canvas) rather than with a server. The AI part is expected to be
invisible in the same way a Lanczos filter is invisible; what is visible is the *area it will affect* and
the *cost*.

## Decisions

| # | decision | consequence |
|---|---|---|
| A | **An AI answer always lands on a new layer**, directly above the active one, named after the operation and the job | no `Apply` step, no preview-then-commit: the layer *is* the preview — hide it, blend it, mask it, delete it, `Ctrl+Z` it. `job.apply {into: "active"}` is deleted, not defaulted |
| B | **The upload is always the flattened composite** of the visible layers up to and including the active one (`Document::flatten_over`), never one layer's pixels | already how it works; the boundary is deliberate — see "why up to, not all" below |
| C | **The mask is the model of "the area I mean"**; a marquee, a brush stroke, an eraser stroke and a layer mask are all editors of it, and *how it is drawn and how it is displayed are UX choices*, not data-model choices | the tinted-area overlay vs marching ants is a view change; `Mask` + `select.*` + `mask.*` stay as they are |
| D | **The hole belongs to the operation, never to the tool** (`Recipe::operation` = `reconstruct / in_place / transform / from_words`, refused at parse when contradicted) | landed 2026-10-11 in `f60c4d9`; it is what stopped Replace uploading a blurred face |
| E | **Words are required for `in_place` and `from_words`, and there are no prefilled defaults.** A masked reconstruction asks for **no words**: the mask says what to remove | `Recipe::ask` shrinks to at most one free-text instruction field; `target` disappears from the masked recipes |
| F | **No AI tab, but AI state stays visible**: resident model, what a press will cost in seconds, and that a press will swap the model on a shared box | one status line, kept out of the tool surface; the Log tab and the debug wire keep the detail |
| G | **Outpaint works by growing the canvas with transparent pixels, sending the flattened frame, and cutting the original frame area out of the answer** so only the new area lands, on its own layer | needs the region compiler to accept a crop that reaches outside the document (today `plan_region` clamps) — see "extend" below |

## The verbs

`Replace` is deliberately not a verb: it is two operations wearing one word, and that ambiguity is what
produced the blurred-face bug.

| verb | command · key | recipe | operation | hole in the upload | words | lands |
|---|---|---|---|---|---|---|
| **Remove** | `ai.remove` · `Ctrl+Backspace` | `inpaint-remove` | `reconstruct` | yes — the marked area blurred away | none | new layer |
| **Fill** | `ai.fill` · `Ctrl+Shift+F` | `region-fill` | `reconstruct` | yes — the marked area destroyed | what should appear | new layer |
| **Change** | `ai.change` · `Ctrl+Shift+C` | `inpaint-replace` | `in_place` | never (the pixels are the reference) | what should change | new layer |
| **Generate** | `ai.generate` · `Ctrl+G` | `new-image` | `from_words` | no source image | the picture | new layer |
| **Extend** | — planned | — | `extend` (planned) | the added transparent area | optional | new layer, cut out to the new area |
| **Upscale** | — planned | `upscale` | `transform` | never | none | new layer, or the document when the document is resized |

Built rows are in `kVerbs`; a row is `{verb, recipe, key}` and nothing else — the label, the words and
the scope all come from the recipe, so there is no second copy of them to keep in step. Loading checks
each row against its recipe (`verb_table_error`) and refuses the whole table when one contradicts it.

Tools are the surface; these are the policies. Several tools drive the same policy (Eraser and Brush both
drive `reconstruct`, differing only in the polarity of the stroke — a brush adds to the mask, an eraser
subtracts), and one tool can drive several (`Resize`: ordinary scaling, or `transform`). A table maps tool
→ policy, and loading refuses a tool whose declared hole contradicts its policy's.

## Why "up to and including the active layer", not "all layers"

If the upload composited layers *above* the active one, those pixels would appear twice after landing: once
baked into the answer, once as the layers that are still there. Flattening up to the active layer and
landing the new layer directly above it is the only combination that composes without double-applying
content. When nothing is above the active layer the two definitions coincide, which is the common case and
why the distinction has not bitten yet.

## Extend (outpaint) — the mechanism, as agreed

1. Grow the canvas by transparent pixels at an anchor (the Photoshop behaviour), which is a document
   operation the AI path does not own.
2. Compile against a region that reaches outside the old frame: context from the old pixels, emptiness
   outside. `plan_region`'s `CLAMP` to the canvas is the promise that has to change, and it is the reason
   `extend` is the last of the verbs rather than the first.
3. Send the flattened frame. The editor always sends **opaque** pixels: a transparent area is flattened over
   a colour measured from the ring (`flatten_over_ring`), which is strictly better than the server's own
   blend-to-white for models without alpha (measured reasoning: a white halo where the frame edge was is
   visible in the result, a ring-colour continuation usually is not). The claim that only `Qwen-Image-2.1`
   takes RGBA natively is a server fact we do not depend on, precisely because we never send alpha.
4. Cut the original frame area out of the answer and land what remains on its own layer. Because the cut-out
   holds no pixels inside the old frame, landing it above the active layer cannot double-apply anything.

As an operation, `extend` is its own kind rather than a `reconstruct` with `prep: none`: the rule
"`reconstruct` must destroy the hole" exists because a hole that still shows its subject is not a question
a model can answer, and `extend`'s hole is empty rather than destroyed. Weakening that rule to accommodate
outpaint would reopen the hole through which the blurred-face bug arrived.

## What this deletes

* the **Edit selection panel** (`ui.cpp`'s widget). `ai.intent` the *command* stays — it is the door the UI
  and a driver share, and the probes assert through it.
* `job.apply {into: "active"}` and `ai.compose`'s land-into-the-active-layer path. **Gone** — the argument
  is refused by name, like any other the command does not declare.
* `ExtentSource` (`layer_alpha` / `canvas`), `composite.extent` in recipes, `extent` on the wire,
  `ComposeReport::pixels_clipped_by_extent`, and `core`'s `alpha_coverage` producer, which existed only to
  feed it. Under A the destination is always a fresh empty layer, so "how far a write may exist" has exactly
  one answer, and a knob with one value is a lie about a choice. Coverage is the only restrictor. **Gone.**
* the answer going through `Document::paint`. It lands through `Document::add_layer_written` instead, so an
  answer and the layer holding it are one undo step — see `PLAN.md`, "an answer, its layer, and its undo".
* prefilled `ask` defaults, and `target` on the masked recipes. **Gone** (`7442538`): an asked-for name
  may not also be in `variables`, `from_words` needs exactly one field, a transform none, and
  `build_prompt` refuses a run where an asked field is blank - so Remove really is zero typing.

## Slice order

1. Landing is always a new layer; delete `into` and the extent concept. **Done** — answers arrive on a new
   layer named for the operation and the job (`Remove j0001`), one `Ctrl+Z` takes layer and pixels together, and
   the drift correction now measures against the document rather than against an empty destination. Observed on
   the wire: `ai_compose_probe` (layer count grows by one, undo returns the document byte for byte, patch mean
   error 25.67 → 0.53) and `ai_job_probe`.
2. A tool table (tool → policy → whether it asks for words), the **Remove** sequence
   (`mark → one key → answer as a new layer`, zero typing), and the panel deleted in the same pass.
   **Done.** `src/app/ai_verbs.{h,cpp}` is the table (`VerbId` → recipe → key) and the commands it
   generates — `ai.remove` `Ctrl+Backspace`, `ai.fill` `Ctrl+Shift+F`, `ai.change` `Ctrl+Shift+C`,
   `ai.generate` `Ctrl+G` — with `ai.verbs` answering the same rows for the window. The
   "Edit selection" panel is gone: the verbs are a section of the Tool panel and keys on the keyboard,
   the brush rows appear only for a tool that paints, and the AI facts rule F asks for live on their
   own line in the status bar. Pressing Remove with nothing marked refuses ("works on a selection and
   there is none") rather than reconstructing the whole frame.
   **Not done: the last clause of rule A.** A finished answer still waits for `Apply` (in the status
   line) because the queue has no app-level beat that lands it. The design is in `PLAN.md`
   ("what rule A still owes"); until it lands, the flagship is *mark → key → Apply*, one press longer
   than agreed.
3. **Change** and **Fill**: the same sequence with one empty box, no defaults.
4. **Generate**: words only, empty document allowed, new layer.
5. **Extend** (canvas grow + outpaint) then **Upscale** (`doc.resize` does not exist yet; ordinary scaling
   first, AI as the option).

## House rule for every probe

Starting the app means taking responsibility for it. `wire_drive.start_app` registers each child and
reaps its **process group** on exit and on SIGINT/SIGTERM: the child is an `xvfb-run` shell with an Xvfb
and the editor inside it, so signalling the shell alone leaves both of them running on the display with
nobody's name on them. `wire_input.py` registers Xvfb with the same reaper *before* the wait that can
fail, because a raise inside `__enter__` never reaches `__exit__`. A probe that crashes must leave no
`noise-edit`, no `Xvfb`, and no stale `/tmp/.X*-lock`.

## What is still owed to the box

* One live run measuring the paired drift correction (`colour_match_drift`) on a real answer — the numbers
  in `probes/2026-10-11-ai-job-live.md` belong to the estimator that no longer exists.
* The recipe/prompt pass (~10 images) that picks prep, context ratio and steps per verb, which only makes
  sense once the verbs exist. Neither has been spent; both need the credentials exported.
