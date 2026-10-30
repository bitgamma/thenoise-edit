// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <string>
#include <vector>

#include "core/history.h"
#include "core/image.h"
#include "core/layer.h"
#include "core/mask.h"

namespace noise {

// Budgeted in stored bytes rather than step count: one region operation on a large document
// is worth a great many dabs, and dropping whole steps is what keeps the budget predictable.
inline constexpr std::size_t kHistoryBudgetBytes = 128 * 1024 * 1024;

// A document with no layer has no canvas, and every command that edits pixels needs one, so
// the last layer cannot be removed.
inline constexpr std::size_t kMinLayers = 1;
// An upper bound on geometry allocations, shared by all canvas commands.
inline constexpr std::int64_t kMaxCanvasPixels = 64LL * 1024 * 1024;

// What limits a write to a layer. `own_coverage` is the claim that the edit's own extent is the limit:
// an AI composite arrives with a feathered mask deliberately wider than the selection it was compiled
// from, and fencing that by the selection puts back the hard edge the feather exists to remove.
enum class PaintFence {
    selection,
    own_coverage,
};

// What asking a layer's picture to change size answered.
struct LayerScale {
    bool done = false;
    // The largest factor that fits this layer's picture and coverage inside the canvas: the number to put
    // in the refusal, so a rejected scale comes with the next one to try. 0 when there was no picture to
    // scale at all.
    double max_factor = 0.0;
    // Where the picture sits afterwards, or where it stands when the scale was refused.
    Rect picture{};
};

// The open document: a stack of layers, and the history of what changed in them. Pixels change
// only through `paint`, a provisional gesture or undo/redo, so every visible change can be put
// back exactly - within one layer, which is what a step is recorded against.
class Document {
public:
    Document();

    // A new or freshly opened document starts a new timeline: `path` empty means "never
    // saved", which is what makes doc.save report itself instead of guessing a filename.
    // Ids are assigned here, so a document always owns the identity its history names and a
    // file cannot name a layer this document does not have. The selection is part of becoming a
    // document rather than something done to it afterwards, which is the only way "opening a file
    // leaves the document clean" stays true however many fields a file carries.
    void reset(std::vector<Layer> layers, std::optional<Mask> selection, std::string path);

    const std::string& path() const;
    void set_path(std::string path);

    // Unsaved changes, conservative: undo does not clear it, because "back to what is on disk"
    // would mean holding the saved checksum as a second fact about the file.
    bool dirty() const;
    void mark_saved();

    bool empty() const;
    int width() const;
    int height() const;
    Extent size() const;
    // Advances on any canvas geometry edit, including undo/redo of that edit.
    // Pending AI jobs compiled for the previous geometry must not land after it changes.
    std::uint64_t geometry_revision() const;

    // Bottom-first. `active` is the layer `paint` and a gesture edit.
    std::span<const Layer> layers() const;
    int active() const;
    const Layer& layer(std::size_t index) const;

    // An empty `name` gets the next generated one. The new layer goes directly above the active
    // layer, which is where a painter expects it, and becomes active. Returns its index, or -1
    // when there is no canvas to size it from.
    // One step, like the layer an answer lands on - and the arrival is the whole of it: undo hands the
    // layer, wearing whatever `fill` gave it, to the redo side, so a fill costs no bytes beside the step.
    int add_layer(std::string name, Rgba8 fill);
    // A new layer where the creation and everything `write` puts into it are one step, rather than the
    // arrival alone as `add_layer` records, so an AI answer is a single edit - `Ctrl+Z` removes the result together with the layer it
    // arrived on, instead of leaving an empty one named after a job. The layer is transparent, goes
    // directly above the active one and becomes active. `write` gets the layer (pixels and optional
    // mask) in one transaction; a `false` means it refused, and then nothing is added and nothing is recorded. Returns the new index, or -1.
    int add_layer_written(std::string name, Rect bounds,
                          const std::function<bool(Layer&)>& write);
    // False when `index` is out of range or it is the last layer standing. The step holds the whole
    // layer, so undo returns its picture, mask, mode and opacity as well as its name - and the steps
    // written on it stay reachable underneath, which is why nothing invalidates them.
    bool remove_layer(int index);

    // The layer at `index` to `to`, both stack indices, bottom-first, with the moved layer left active.
    // `index == to` succeeds without a step, a revision or a dirty flag: nothing moved, and the same rule
    // a write that changed no pixel follows keeps the timeline honest. What the step holds is the order
    // itself, so undo puts the stack back as it was - and because a reorder changes what the composite
    // shows everywhere, the whole canvas is repainted rather than a rectangle.
    bool move_layer(int index, int to);

    // The whole stack into one layer, holding exactly what the canvas showed - hidden layers and all the
    // blending included, because that is the picture a person is looking at when they ask for it. Its
    // history entry is the stack itself rather than an entry per layer: nothing stays behind to carry the
    // undo of what left, and a stack put back by swapping needs no position per layer - one removal per
    // step is as much as an index can be trusted with. The new layer is visible, at full opacity, in normal
    // mode and unmasked - not for tidiness: those are the facts that let the composite be the layer, so the
    // canvas cannot re-blend what was just baked. False when there is nothing to flatten.
    bool flatten_layers();

    // The layer's picture, and the coverage that hides part of it, scaled about the middle of what the
    // picture holds. A layer is the size of the canvas and always will be, so scaling a layer scales what
    // it holds *inside* that rectangle: everything outside the scaled picture comes out transparent, and a
    // scale that would push the picture past a canvas edge is refused rather than clipped - losing the
    // part of someone's drawing that fell off the edge is not a reversible reading of "twice as big", and
    // the refusal carries the largest factor that does fit.
    //
    // The mask goes with the picture under the same map, because it is a claim about the same pixels:
    // scaling one without the other would hide parts that were shown and show parts that were hidden. A
    // mask that covers everything, or nothing, is the same mask at any size and is left alone - filtering
    // it would spend a pass putting ringing where there was none.
    //
    // Like a merge this is not a paint, so the selection fences nothing: it changes which pixels of the
    // layer exist at all, not what was written into them. One undo step, holding the rectangle both planes
    // moved over, and a scale that fits its own box exactly (factor 1) records nothing.
    LayerScale scale_layer(int index, double factor);

    // Change the extent without resampling existing pixels. `offset` is where old (0,0)
    // sits in the new canvas. Negative offsets crop, positive offsets expose empty pixels.
    // `mark_added` selects precisely the newly exposed area for AI Fill/outpainting.
    // Undo/redo exchange every layer, mask and selection as one state.
    bool resize_canvas(Extent target, int offset_x, int offset_y, bool mark_added);
    bool crop_canvas(Rect crop);

    // The layer at `index` onto the one under it, exactly as the canvas shows them: the upper layer's
    // mask, blend mode and opacity are applied on the way down, and it leaves the stack while the layer
    // under it keeps its identity - name, mode, opacity, mask - which is what makes the undo exact. An
    // invisible upper layer contributes nothing, so merging it takes it away without moving a pixel; it
    // still comes back on undo. False when `index` is out of range or has nothing under it.
    //
    // A merge is not a paint: the marked area limits what a write may change, not which layers the stack
    // holds, so no selection fences this.
    bool merge_down(int index);
    // False when `index` is out of range. Selecting changes no pixels, so it bumps no revision.
    bool select_layer(int index);

    // The selection: the pixels a write may change. `nullopt` is *no selection*, which is the whole
    // canvas, and it is not the same state as a mask that covers nothing - that one stops every
    // write. Both are needed, because inverting an empty selection selects everything and subtracting
    // everything leaves nothing, and one value cannot tell those apart.
    //
    // The marked area is a document property - it is saved with the document, so it marks it dirty -
    // changing it is an edit: `set_selection` records the coverage it replaces as one undo step. What
    // that step puts back is coverage and never a pixel, so undoing a mark re-uploads nothing and
    // resamples no thumbnail; see `Applied::coverage`.
    const std::optional<Mask>& selection() const { return selection_; }
    // False when the mask is not the size of the canvas. A selection is a claim about these pixels, so
    // cropping or padding one to fit would move the claim; the caller fixes the size or is told.
    // While a marked gesture is open the write still happens but the step does not, because the
    // gesture holds the one step for the whole of itself.
    bool set_selection(std::optional<Mask> selection);

    // A gesture over the marked area - the marquee from press to release, `cover` and `un-cover` aimed
    // at it. One gesture, one step, by the same door a pixel stroke uses: `begin_marked` holds the
    // coverage the press found, writes during the drag record nothing, `commit_marked` records what the
    // press found if the gesture changed anything, and `cancel_marked` puts it back and records nothing.
    // Without the held-aside copy a stroke would either compound its own ink once per event or push a
    // canvas-sized step per frame, and a marquee would push one per pixel it moved over.
    // False when there is no canvas, or when a gesture already holds a layer's plane - a press writes
    // one lifetime of one thing.
    bool begin_marked();
    // Restores the coverage the press found inside `dirty` and lets `paint` write into that region, so
    // the event costs what it added and the coat is the stroke's own opacity rather than one coat per
    // frame. Does nothing outside a gesture.
    void repaint_marked(Rect dirty, const std::function<void(Mask&, Rect)>& paint);
    void commit_marked();
    void cancel_marked();
    bool in_marked() const;

    // What the composite is made of, changed without touching a pixel. All three bump the
    // revision - the canvas shows the composite, so a renderer that re-uploads on it would
    // otherwise keep showing a layer that is meant to be gone - and all three mark the document
    // dirty. None enters history, nor does adding or removing a layer. `opacity` is 0..1, and out
    // of range is false rather than a clamped value.

    // A layer's name: what the panel shows, what an error message points at and what the container
    // stores. `normalize_layer_name` is what decides whether the name is one, and a name that is not is
    // false rather than a quietly shortened one. No pixel moves and no layer's content changes, so a
    // card's thumbnail is untouched - and it is one undo step, because a mistyped name is not a reason to
    // lose the stroke under it.
    bool rename_layer(int index, std::string name);

    // The layer's mask: what the composite hides. A mask must be the size of the canvas, and a
    // different size is false rather than a resized mask - a mask that quietly shifted would hide
    // pixels nobody asked about. Putting one on and taking one off record the *plane* (`Kind::mask_replaced`);
    // writing coverage into one records pixels (`paint_mask`). Neither moves the layer's picture, and both
    // change the canvas, so both bump the revision, mark the document dirty and widen the pending region
    // to the whole canvas, because what a mask hides is outside its bounds too.
    bool set_layer_mask(int index, std::optional<Mask> mask);

    bool set_layer_visible(int index, bool visible);
    bool set_layer_opacity(int index, double opacity);
    bool set_layer_mode(int index, BlendMode mode);

    // Every layer's pixels, bottom-first, with the layer count in front: two documents with
    // different stacks differ here even when the flattened result happens to match.
    std::string checksum() const;

    // The flattened stack: what the renderer uploads, what a flat export writes, and what the
    // container stores as `mergedimage.png`. An invisible layer is not in it, and a layer at
    // 50 % contributes half its own alpha. Built on demand and repaired only over the rectangles
    // the layers changed since it was last asked for, because a stack that is re-blended whole
    // puts a canvas-sized pass on every frame of a live stroke.
    const Image& composite() const;

    // The visible layers at or below `top_layer`, cropped to `bounds`, over transparent. A request's
    // base must not include a layer the answer is written back *under*: the patch would bake in content
    // that then lands twice. `top_layer` is normally the active layer.
    Image flatten_over(Rect bounds, std::size_t top_layer) const;

    // Bumped by anything a step could undo and by anything that changes what the canvas shows;
    // the renderer re-uploads on it.
    std::uint64_t revision() const;

    // Applies `edit` to the active layer and records the pixels `bounds` held beforehand. An
    // edit that writes outside `bounds` will not be fully undone, which is why bounds is not
    // inferred. With `fence` == `selection`, what the edit wrote is then pulled back to what was there
    // wherever the document's selection does not reach; with `own_coverage` the edit's own extent is the
    // limit, and keeping inside it is the caller's claim. An edit left with no pixel changed records no
    // step, marks nothing dirty and bumps no revision.
    void paint(Rect bounds, PaintFence fence, const std::function<void(Image&)>& edit);

    // The same door over the active layer's coverage instead of its colour: `bounds` recorded, the
    // selection applied to it, no step for a write that changed nothing. False when there is no
    // canvas or no mask to write - a stroke does not put a mask down, because a mask appearing under
    // the brush would change what the document holds without anyone asking. True when the write was
    // attempted, including when the selection stopped every byte of it.
    bool paint_mask(Rect bounds, const std::function<void(Mask&)>& edit);

    // A gesture that is still going - a pointer stroke - shows its result before it ends, on
    // the layer that was active when it began. The bytes under `area` are held aside, every
    // repaint starts from them, and `commit` records them as one undo step. Without this a live
    // stroke would either compound its own opacity once per frame or push an undo step per
    // frame. Calling `begin_provisional` again widens the held-aside area, which costs nothing
    // per event: the buffer grows by a factor, and pixels outside what is already held aside
    // have not been written, so the layer still shows what the gesture found there.
    //
    // The plane is named because one gesture writes exactly one of them. False when there is nothing
    // of that plane to write - no canvas, or a `mask` gesture on a layer with no mask.
    bool begin_provisional(Rect area, LayerPlane plane);
    // Restores `dirty` from the held-aside bytes and calls `paint` for that rectangle only. The
    // rectangle is an argument because repainting the whole held-aside area per event is what
    // made a drag cost more with every event: 2.9 ms to 108 ms over one 40-event stroke at 2048².
    // Each one writes the plane its gesture began on; the other is refused in the log.
    void repaint_provisional(Rect dirty, const std::function<void(Image&, Rect)>& paint);
    void repaint_provisional_mask(Rect dirty, const std::function<void(Mask&, Rect)>& paint);
    void commit_provisional();
    void discard_provisional();
    bool in_provisional() const;
    Rect provisional_area() const;

    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
    HistoryStats history() const;

private:
    std::uint64_t geometry_revision_ = 0;
    struct Provisional {
        std::uint64_t layer;
        LayerPlane plane;
        Rect rect;
        // The bytes the gesture found at `rect`, in the units of the plane it names: four per pixel of
        // colour, one per pixel of coverage. A variant rather than two members because only one of
        // them can be true at a time, and a plane that switched mid-gesture would restore the other's
        // bytes over it.
        std::variant<Image, Mask> held;
    };

    // Every change to a **canvas** leaves through one of these three, so "what the canvas shows moved"
    // and "which layer's own content moved" cannot drift apart. The marked coverage is in none of them:
    // the composite does not read it, so a mark moves no texture and no thumbnail, and the overlay
    // reads the plane per frame. The second is what a viewer of a single
    // layer - a thumbnail - caches against, which is why a reorder and a layer arriving or going are the
    // first one's business only: they move no layer's content, and a card rebuilt for them would
    // re-sample the stack on a change that changed no picture. The third is for a step whose reach the
    // history reports as rectangles rather than as layers: undo and redo.
    void changed_composite();
    void changed_layer(Layer& layer);
    void changed_all_layers();

    Layer* layer_of(std::uint64_t id);
    Image* layer_pixels(std::uint64_t id);
    Mask* layer_mask(std::uint64_t id);
    // `dst_at` and `src_at` say where each image sits in document coordinates.
    void copy_rows(Image& dst, Rect dst_at, const Image& src, Rect src_at, Rect area) const;
    // `copy_rows` over coverage planes: same rule, one byte per pixel.
    void copy_coverage_rows(Mask& dst, Rect dst_at, const Mask& src, Rect src_at, Rect area) const;
    // The pristine bytes the gesture found inside `absolute`, taken out of the held-aside
    // snapshot, as an image of `absolute`'s own size.
    Image held_aside(Rect absolute) const;
    Mask held_aside_coverage(Rect absolute) const;
    // True when `region` of `after` still holds exactly what `before` did, row by row.
    bool unchanged(const Image& before, const Image& after, Rect region) const;
    bool unchanged(const Mask& before, const Mask& after, Rect region) const;

    // What the layers changed since `composite_` was last repaired. Accumulated by every pixel
    // change, emptied by the repair; structural changes widen it to the whole canvas.
    mutable Rect pending_;

    std::uint64_t next_layer_id_ = 1;
    std::vector<Layer> layers_;
    int active_ = 0;
    // After a step that could have removed the active layer: back to it when it survived, otherwise the
    // nearest one below where it sat. Never left pointing past the stack.
    void retarget_active(std::uint64_t preferred);
    std::optional<Provisional> provisional_;
    std::optional<Mask> selection_;
    // The gesture `begin_marked` opened. `marked_held_` is the coverage the press found, always
    // materialised because a stroke needs a plane to write into, and `marked_had_selection_` says
    // whether the document had one at all — the plane covering nothing and no plane are different
    // documents, and the gesture puts back whichever it found.
    bool marked_gesture_ = false;
    bool marked_had_selection_ = false;
    Mask marked_held_;
    // What the gesture has actually painted, which is smaller than the held-aside rectangle: the
    // undo step records this, and discarding restores this.
    Rect painted_;
    std::string path_;
    bool dirty_ = false;
    std::uint64_t revision_ = 0;
    History history_;
    mutable Image composite_;
    // Scratch for a masked layer: the layer scaled by its mask's coverage, at document coordinates,
    // so the blend reads it exactly as it reads a layer. Held by the document so a masked composite
    // does not allocate per layer per frame; unused, and so unallocated, while no layer has a mask.
    mutable Image masked_;
};

}  // namespace noise
