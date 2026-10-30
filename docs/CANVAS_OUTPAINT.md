# Canvas size, crop, outpaint and editable edges

The editor distinguishes **canvas size** (change the picture's bounds, preserve source pixels)
from **image size** (resample the picture, a separate feature not implemented here). All commands
are also available from the command/debug wire.

## Enlarge the canvas and outpaint

1. Expand **Canvas size / Crop** in the tool panel. Set width/height, then pick an anchor.
   The anchor determines where the original image sits; e.g. anchoring at **right** places
   newly added width to its left. No old pixel is resampled.
2. Keep **Select added area for AI Fill** enabled and press **Apply canvas size**. Every
   old layer and its mask moves with the image. Only the new (transparent) border becomes
   the marked selection. If no new area was added, an existing selection is translated.
3. Use the existing **Fill** operation, with whatever prompt the operation requires. The
   original scene is compiled as the context inside the request crop, and the returned
   pixels are composited onto a *new* layer, limited to the selected border and its feather.
   The old source layers are not overwritten. The result can be re-asked with a new seed.
4. To generate a broader area, change the marked area before the request: draw a rectangle,
   lasso or brush a region spanning both old and new pixels. **All** selects the entire
   enlarged document, allowing a whole-image **Change** (with a prompt) instead of a border
   outpaint. **Generate** follows its own whole-document recipe.

The canvas has the exact requested pixel dimensions (up to 64 megapixels). The *AI request*
may be rescaled to the model's published pixel cap; check the request quotation before
running a large full-scene generation.

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

For local mask editing beyond the original selection, clear that selection first: painting may
otherwise be clipped by the still-marked area. **New AI edit** does not silently clear it.

## Crop

Set **Crop to rectangle** (x, y, width, height), or mark an area and press **Crop to
selection**. The latter uses the marked area's **bounding rectangle**, not an irregular
cutout. Both retain pixels only inside the crop; all layer masks and selection coordinates
move with it. Undo restores the entire pre-crop document, including off-canvas pixels.

## Adjust a generated edge after the request

Select the result layer. The **Adjust layer edge** panel appears when that layer has a mask.
Use **Hard edge** to threshold its coverage, **Grow / shrink** to reveal or hide more of the
*already generated* crop, and **Additional feather** for softer blending. Press **Apply edge**
to commit an undoable mask edit. The numeric shift/feather are *incremental*: applying them
again makes a further change. The preview can be reversed exactly with Undo. For a local
correction, switch coverage target to **layer mask** and use **Cover / Un-cover** with a brush
or another selection tool instead of expanding the entire edge. Full-colour generated pixels
are retained under the mask, so shrinking and re-expanding does not require regeneration.
Only pixels inside the original returned crop exist to reveal; pixels farther out require
another AI request.

## Wire command examples

The tool panel invokes these same commands (arguments are JSON objects):

```json
{"cmd": "doc.resize", "args": {"width": 1536, "height": 1024, "offset_x": 256, "offset_y": 0, "mark_added": true}}
{"cmd": "doc.crop", "args": {"rect": [100, 50, 900, 700]}}
{"cmd": "doc.crop", "args": {}}
{"cmd": "mask.edge", "args": {"shift": -8, "hard": true, "feather": 0}}
{"cmd": "mask.edge", "args": {"shift": 4, "hard": false, "feather": 6}}
```

`offset_x` and `offset_y` are the new coordinates of the **old** image origin. A negative
offset crops that side. A resize with unchanged extent and zero offset is a no-op. A crop
rectangle must lie fully inside the current canvas. After a resize/crop (including its
undo/redo), queued AI jobs from the earlier geometry cannot land onto the wrong coordinates.
