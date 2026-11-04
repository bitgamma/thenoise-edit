# Canvas size, crop, outpaint and editable edges

The editor distinguishes **canvas size** (change the picture's bounds, preserve source pixels)
from **image size** (resample all image content). All commands are also available from the
command/debug wire.

## Image size, AI upscaling and deferred resolution

**Image size / Upscale** has a shared target **Width / Height**, aspect lock, and 2x/4x presets.
Choose *one* of these operations; neither changes the canvas by adding empty borders:

- **Resize (Lanczos3)** creates a new document version from the visible composite by default, without altering the original layers. **Entire layer stack** (Lanczos3 only) creates a new version in which every original layer, mask, and blue selection is independently resampled. Both operate locally, and neither invents details.
- **Upscale (AI)** chooses an explicit model from the server catalogue (including separately
  named 2x and 4x ESRGAN models). It sends the **entire visible composite at its original
  resolution** to `/images/upscale`. This endpoint does not take edit-model grid/snapping
  or the edit model's 2-Mi-pixel region budget. For instance, 4 MP at native 2x yields
  16 MP, which fits the 64-Mi-pixel document cap. The requested-scale answer becomes
  a single layer in a **new document version**; the original version retains its
  native-resolution layers, masks and ordering. Switch versions to return to it.
- **Use target size for next AI edit** does **not** start an ESRGAN job. It reserves
  the selected output width/height for the next ordinary **Change / Fill / Remove / Heal**
  request to `/images/edits`. The original full image is uploaded at its present resolution,
  the **same edit-model request** asks for the target resolution, and one returned image
  becomes a new, larger **document version**. A 1024x1024 image can thus be edited (e.g. "make the
  hair blonde") and requested at 2048x2048 in **a single model generation**, provided
  that model supports the larger output.

Deferred resolution is one-shot after a successful landing. A failed/cancelled request
leaves it armed for retry. Changing document geometry invalidates it. If the document
changes while the request runs, its response cannot replace the newer work. The pending
request does **not** use a second queue entry or a separate ESRGAN job. **The whole image
is regenerated at the larger size**; unlike a conventional selection-scoped edit, the
backend can alter details outside the selection. The panel makes this distinction explicit.
The chosen document target can have any dimensions. Only the **model request** is rounded
up to its output grid (normally 16 pixels per axis), and a correctly sized model answer
is resampled to the exact chosen document size on landing. The rounded request, rather
than the unsnapped target, must fit the edit model's published output pixel budget and
4096-pixel axis ceiling. Some edit backends snap the answer to a coarser latent grid
than the requested size (for example, 752x816 requested, 768x832 returned). Small
snapping differences are accepted and resampled to the exact chosen document dimensions.
Large unexpected size differences are refused without changing the document; the landing
error is shown in the Tools panel and recorded on the job.

The editor sends `upscale_factor` as a JSON number to `/images/upscale`, calculated
from the requested width/height and source dimensions. This requires a Lemonade build
that forwards the optional field to TheNoise. The factor must be between 1 and the
upscaler's native scale, and the **requested output** must fit 64 Mi pixels. Only a
possible one-pixel dimension-rounding difference is locally resampled; a server that
ignores the factor and returns full native size is refused rather than silently
accepted. Upscalers with unknown native factor are not assumed to be 2x or 4x.
Generative deferred output may change aspect ratio; ESRGAN may not.

A connected server is necessary for the model-based operations. Pixel upscalers use the
dedicated upscale endpoint; selecting one does not unload the image editing model and
does not require a model-switch confirmation. The local operation does not require a
server. A resolution-changing composite/AI upscale creates a new document version. Each version
has independent layer ordering, selection and Undo/Redo history. Original layers remain at
native resolution, and the selected version (including all others) is saved in the project.
Within a version the layer stack is freely reorderable, but layers cannot be dragged across
versions of different resolutions. A version switch never resamples pixels. Each upscale
version also remembers the version that created it: **Ctrl+Z** first undoes regular layer
edits in the active version, then returns to its parent version; **Ctrl+Y** brings the
upscaled version back without regenerating it or destroying its layers. The parent and
child retain their independent layer order and undo history. Explicit selection of a
version in the versions list is navigation, not an Undo action. The version ancestry is
saved in the project so switching back after a reload still works; individual pixel-edit
history remains session-local. When **Entire layer stack** is checked, AI Upscale and
**Use target size for next AI edit** are disabled; only Lanczos3 is available.

Debug wire examples:

```json
{"cmd": "doc.resample", "args": {"width": 2048, "height": 2048}}
{"cmd": "doc.resample", "args": {"width": 2048, "height": 2048, "whole_stack": true}}
{"cmd": "doc.version.list", "args": {}}
{"cmd": "doc.version.select", "args": {"id": 1}}
{"cmd": "image.upscale", "args": {"width": 2048, "height": 2048, "model": "RealESRGAN-2xplus-TheNoise"}}
{"cmd": "image.upscale.defer", "args": {"width": 2048, "height": 2048}}
{"cmd": "image.upscale.status", "args": {}}
{"cmd": "ai.preview", "args": {"recipe": "inpaint-replace", "vars": {"instruction": "make the hair blonde"}}}
{"cmd": "ai.run", "args": {"recipe": "inpaint-replace", "vars": {"instruction": "make the hair blonde"}}}
{"cmd": "image.upscale.defer", "args": {"enabled": false}}
```

`image.upscale.defer` and `image.upscale.status` report both `target` (final document
dimensions) and `model_request` (grid-aligned generation dimensions). The AI Upscaler
warning appears immediately below its button and only mentions a limit if that specific
limit blocks the chosen size.

## Enlarge the canvas and outpaint

1. Open **Canvas size / Crop**, enter width and height, and choose one of nine anchors. The
   anchor fixes the original image's position; the operation moves pixels and masks without
   resampling.
2. Press **Resize & Fill...**. This resizes the document, selects *only* the exposed area,
   and opens the normal **Fill** prompt with the editable suggestion
   `extended the background`. **No AI request is sent by resizing.** Press **Run** to start
   the existing `region-fill` recipe. A model connection is required for generation, not for
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

## Mark, generate, and refine

Cover / Un-cover and selection tools **always edit the blue marked area**.
The floating **Heal / Remove / Fill / Change** buttons are available when a mark is
present, and disabled while the AI request is running. After a successful landing the
floating bar disappears, leaving the original selection intact rather than blocking
part of the image. Adjust the selection or draw another to show the buttons again.

The answer appears on a new result layer; the **image thumbnail stays selected**.
Click the mask thumbnail only if you want to refine the result with Brush / Eraser,
shown with an optional red mask overlay. Clicking the image thumbnail returns to pixel
painting. No New AI edit button or extra commit/apply step is needed. Undo/Redo never
repeat the AI request.

## Rotate

The **90 deg CW** and **90 deg CCW** buttons rotate the whole document: every layer's
pixels, its optional mask, and the marked area. Width and height swap. Each rotation
is one Undo/Redo step and invalidates AI answers queued for the old geometry.

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
per layer during this document session; Brush / Eraser strokes on a mask rebase
it on the modified mask rather than overwriting those local corrections.

For local corrections, click the result layer's mask thumbnail and use **Brush / Eraser**.
The actual generated pixels remain stored under the mask;
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
{"cmd": "doc.outpaint.prepare", "args": {"width": 1536, "height": 1024, "offset_x": 256, "offset_y": 0, "prompt": "extended the background"}}
{"cmd": "doc.rotate", "args": {"direction": "cw"}}
{"cmd": "doc.rotate", "args": {"direction": "ccw"}}
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
