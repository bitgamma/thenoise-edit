# Canvas size, crop, outpaint and editable edges

The editor distinguishes **canvas size** (change the picture's bounds, preserve source pixels)
from **image size** (resample the picture, a separate feature not implemented here). All commands
are also available from the command/debug wire.

## Enlarge the canvas and outpaint

1. Open **Canvas size / Crop**, enter width and height, and choose one of nine anchors. The
   anchor fixes the original image's position; the operation moves pixels and masks without
   resampling.
2. Press **Resize & Fill...**. This resizes the document, selects *only* the exposed area,
   returns Cover / Un-cover to **marked area**, and opens the normal **Fill** prompt with an
   editable suggestion. **No AI request is sent by resizing.** Press **Run** to start the
   existing `region-fill` recipe. A model connection is required for generation, not for
   resizing. Alternatively press **Resize canvas** for geometry alone; with **Select added
   area for AI Fill** enabled, the new border is selected but the Fill prompt does not open.
3. Once the generated answer lands, it becomes a new result layer with a retained editable
   mask. The existing image remains untouched. Its transparent extension receives full
   coverage right up to the *new canvas boundary*; feathering is only allowed to soften
   the seam where original pixels already exist.
4. To generate a broader region, manually mark both old and new pixels before pressing
   **Fill** / **Change**, or select **All** to work on the full enlarged document.
   **Generate** uses its own whole-document recipe instead.

The canvas has the requested pixel dimensions (up to 64 megapixels). The *AI request*
may be rescaled to the model's pixel budget; the floating prompt's **Run** is the point
where an expensive request is actually submitted.

## From a marked area to the next AI edit

The floating **Heal / Remove / Fill / Change** bar has three states:

1. **Ready:** select an area and choose an operation. Cover/Un-cover writes to the **marked
   area** for the next AI request.
2. **Generating:** the bar stays in place but all four operations are disabled. The same
   readiness check also blocks repeat launches from the panel and keyboard shortcuts. A failed
   or cancelled job unlocks the buttons without entering result editing.
3. **Result editing:** after the answer has **successfully landed** as a masked result layer,
   the whole bar becomes **Editor mode active** with **New AI edit** underneath. Cover/Un-cover
   now edits the result **layer mask** directly; local edge corrections do not re-run AI.

**New AI edit** ends the result-edit session, points Cover back to the **marked area**, and
keeps the result layer and its pixels unchanged. The four AI operations return if a selection
still exists; otherwise, draw a new mark to show them. If the old selection was cleared, the
return button remains near the result mask (or the original request bounds when its mask is
empty). Changing the active layer does not silently start another AI edit. Undoing/removing the
result layer ends its edit session, and undo/redo never repeats an AI request. The special
first AI result on a fully blank canvas has no separate layer mask, so it stays in ready mode.

Editing an existing **layer mask** is independent of the marked area: Cover / Un-cover and
mask shape tools can adjust it even when a selection for an AI operation remains visible.
**New AI edit** does not remove the previous mask or clear the selection; it only routes
painting back to the marked area for the next generation.

## Crop

Set **Crop to rectangle** (x, y, width, height), or mark an area and press **Crop to
selection**. The latter uses the marked area's **bounding rectangle**, not an irregular
cutout. Both retain pixels only inside the crop; all layer masks and selection coordinates
move with it. Undo restores the entire pre-crop document, including off-canvas pixels.

## Adjust a generated edge after the request

Select any layer with a mask. **Adjust layer edge** has two independent absolute
sliders, each with **− / +** buttons for precise one-pixel changes:

- **Grow / shrink:** 0 is the original edge; +10 grows by 10 px, returning to 0
  restores the original mask **byte for byte**. Negative values shrink it.
- **Feather:** a freshly generated result starts at the actual feather radius used
  on landing (for example 8 px), with that value at the centre of the initial
  scale. 0 makes a hard edge. Returning to the original radius restores the
  original coverage bytes when Grow / shrink is also back to 0.

Each change is visible immediately; the sliders **never reset on release**.
Their displayed scales scroll when a value reaches the end, without changing
that value (e.g. Grow / shrink -50..+50 becomes 0..+100 and then +50..+150).
The possible extremes depend on the canvas dimensions. No Apply, Commit or
Harden button exists; to undo a +10 adjustment just choose 0 again, without
using Undo. A drag still records at most one optional Ctrl+Z history entry,
whereas click increments are applied immediately. The original edge is retained
per layer during this document session; independent Cover/Un-cover strokes rebase
it on the modified mask rather than overwriting those local corrections.

For local corrections, select **layer mask** under Coverage, then paint **Cover / Un-cover**
using a brush or shape tool. The actual generated pixels remain stored under the mask;
shrink/re-expand can reveal them without another request, but only inside the crop the model
returned. Any area beyond it needs a new request.

A soft seam is meaningful only over existing image content. A freshly enlarged *transparent*
canvas has no old pixels to blend into at its outer boundary, so selected transparent pixels
are composited fully opaque. This avoids the washed-out semi-transparent strip at the edges
of an outpaint.

## Wire command examples

The tool panel invokes these same commands (arguments are JSON objects):

```json
{"cmd": "doc.resize", "args": {"width": 1536, "height": 1024, "offset_x": 256, "offset_y": 0, "mark_added": true}}
{"cmd": "doc.outpaint.prepare", "args": {"width": 1536, "height": 1024, "offset_x": 256, "offset_y": 0, "prompt": "a continuation of the forest"}}
{"cmd": "doc.crop", "args": {"rect": [100, 50, 900, 700]}}
{"cmd": "doc.crop", "args": {}}
{"cmd": "mask.edge.state", "args": {}}
{"cmd": "mask.edge.preview", "args": {"shift": 10}}
{"cmd": "mask.edge.finish", "args": {}}
{"cmd": "mask.edge.preview", "args": {"shift": 0}}
{"cmd": "mask.edge.finish", "args": {}}
{"cmd": "mask.edge.preview", "args": {"feather": 0}}
{"cmd": "mask.edge.finish", "args": {}}
{"cmd": "mask.edge.preview", "args": {"feather": 8}}
{"cmd": "mask.edge.finish", "args": {}}
```

`doc.outpaint.prepare` opens the Fill prompt, but does not queue a model job; press **Run**
(or issue the existing `ai.fill` command) to generate. `mask.edge.state` reports
both absolute values and the scrolling ranges. `mask.edge.preview` takes absolute
values relative to the unchanged mask captured for that layer; omitted values remain
unchanged. `finish` closes one optional Undo gesture without resetting slider
positions, and `cancel` returns to the start of the current gesture. A size/feather
preview updates only the union of its previous and next edge footprints, including the
full blur halo. All other canvas pixels are left untouched, even for one-pixel +/- clicks;
there is no deferred or approximate result. `mask.edge`
remains the legacy one-shot relative command for the debug wire. Offsets specify the old image origin in new canvas
coordinates, and undo/redo of a resize or crop rejects stale AI answers.
