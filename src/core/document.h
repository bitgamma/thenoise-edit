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

#include <optional>

namespace noise {

// Budgeted in stored bytes rather than step count: one region operation on a large document
// is worth a great many dabs, and dropping whole steps is what keeps the budget predictable.
inline constexpr std::size_t kHistoryBudgetBytes = 128 * 1024 * 1024;

// A document with no layer has no canvas, and every command that edits pixels needs one, so
// the last layer cannot be removed.
inline constexpr std::size_t kMinLayers = 1;

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

    // Bottom-first. `active` is the layer `paint` and a gesture edit.
    std::span<const Layer> layers() const;
    int active() const;
    const Layer& layer(std::size_t index) const;

    // An empty `name` gets the next generated one. The new layer goes directly above the active
    // layer, which is where a painter expects it, and becomes active. Returns its index, or -1
    // when there is no canvas to size it from.
    int add_layer(std::string name, Rgba8 fill);
    // False when `index` is out of range or it is the last layer standing.
    bool remove_layer(int index);
    // False when `index` is out of range. Selecting changes no pixels, so it bumps no revision.
    bool select_layer(int index);

    // The selection: the pixels a write may change. `nullopt` is *no selection*, which is the whole
    // canvas, and it is not the same state as a mask that covers nothing - that one stops every
    // write. Both are needed, because inverting an empty selection selects everything and subtracting
    // everything leaves nothing, and one value cannot tell those apart.
    //
    // The selection changes no pixel and enters no undo step, exactly like a layer's visibility: a
    // step puts pixels back, and one that also moved the selection would undo something the user never
    // asked about. It is part of the document, so it is saved with it and it marks the document dirty.
    const std::optional<Mask>& selection() const { return selection_; }
    // False when the mask is not the size of the canvas. A selection is a claim about these pixels, so
    // cropping or padding one to fit would move the claim; the caller fixes the size or is told.
    bool set_selection(std::optional<Mask> selection);

    // What the composite is made of, changed without touching a pixel. All three bump the
    // revision - the canvas shows the composite, so a renderer that re-uploads on it would
    // otherwise keep showing a layer that is meant to be gone - and all three mark the document
    // dirty. None enters history, nor does adding or removing a layer. `opacity` is 0..1, and out
    // of range is false rather than a clamped value.
    // The layer's mask: what the composite hides. A mask must be the size of the canvas, and a
    // different size is false rather than a resized mask - a mask that quietly shifted would hide
    // pixels nobody asked about. Like visibility it changes no pixel and enters no undo step, but it
    // does change the canvas, so it bumps the revision, marks the document dirty and widens the
    // pending region to the whole canvas, because what a mask hides is outside its bounds too.
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

    // Bumped by anything a step could undo and by anything that changes what the canvas shows;
    // the renderer re-uploads on it.
    std::uint64_t revision() const;

    // Applies `edit` to the active layer and records the pixels `bounds` held beforehand. An
    // edit that writes outside `bounds` will not be fully undone, which is why bounds is not
    // inferred. What the edit wrote is then pulled back to what was there wherever the selection
    // does not reach, so no writer has to know a selection exists - and an edit left with no pixel
    // changed records no step, marks nothing dirty and bumps no revision.
    void paint(Rect bounds, const std::function<void(Image&)>& edit);

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
    std::optional<Provisional> provisional_;
    std::optional<Mask> selection_;
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
