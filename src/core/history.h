// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/geom.h"
#include "core/image.h"
#include "core/layer.h"

namespace noise {

// What one undo step can put back is the rectangle an operation declared, the plane of the layer it
// was on, and the bytes it held there; the stacks themselves are private to History.
struct HistoryStats {
    int undo_depth = 0;
    int redo_depth = 0;
    std::size_t bytes = 0;
};

// Undo and redo as a stack of rectangles. Each entry holds the state that is *not* current,
// so an undo hands the current pixels to the redo stack instead of keeping a second copy.
// The budget drops whole entries from the bottom: losing the oldest step is honest, losing
// the newest is not, and a step is the unit an AI operation costs.
class History {
public:
    explicit History(std::size_t budget_bytes);

    // `before` must be the bytes of `rect` in `layer_id`'s `plane`, in that plane's own units: four
    // bytes per pixel of colour, one per pixel of coverage. Records nothing for an empty rectangle and
    // clears the redo stack, because the timeline has forked.
    void record(std::uint64_t layer_id, LayerPlane plane, Rect rect,
                std::vector<std::uint8_t> before);

    // Everything recorded between `begin_step` and `end_step` comes back as **one** edit. A gesture that
    // changes the layer stack and writes pixels has to be undone as the one thing the user did.
    void begin_step();
    void end_step();

    // A layer this step created. Undo takes the layer away and puts it, whole, on the redo side, so redo
    // re-inserts it at `index` wearing whatever it held when the step was undone - which is why a layer
    // created with a fill needs no pixel entry beside this one, while a layer created *and written*
    // records that write as the pixel entries of the same step. `size` is what makes a re-inserted layer
    // the layer it was: a rectangle of the same step still has to fit it.
    void record_layer_created(std::uint64_t layer_id, int index, Extent size);

    // The stack's order, recorded as it stands. Applying the entry puts the layers into the order it
    // holds and keeps the order it displaced, which is the same "an entry holds the state that is not
    // current" rule as a rectangle of pixels. No pixel is touched, so the caller learns from
    // `Applied.structure` that what the composite shows changed everywhere, not inside one rectangle.
    void record_layer_order(std::vector<std::uint64_t> order);

    // A layer this step **took away** - merged into the layer under it, say. `layer` is the whole of it,
    // pixels and mask and name included, so undo puts the layer back as it was rather than as an empty
    // one; `index` is where it sat. `Document::remove_layer` files its departure under this entry, which
    // is why nothing in `History` invalidates steps by layer id: a step naming a layer that is not there
    // for the moment is one undo away from being applicable, not a lie. One removal per step: `index` is
    // read against the stack the layer goes back into, and putting one layer back cannot shift another.
    void record_layer_removed(Layer layer, int index);

    // A step that replaced the **whole stack** with layers none of the old ones can become - flattening
    // is the case. The entry holds the stack that is not current and applying it swaps the two, so undo
    // and redo are the same operation and every layer returns to the place it came from; encoding the
    // same thing as one arrival-plus-departure entry per layer needs an index per layer, and indices
    // read against a shrinking stack do not say where the layers go back. Such a step records nothing
    // else: the swap carries every pixel, and anything else would describe a layer it is about to
    // replace.
    void record_stack_replaced(std::vector<Layer> stack);

    // The document's **marked coverage** - the area it marks, which is a document property that is
    // saved and belongs to no layer. The entry holds the coverage that is not current and applying it
    // swaps the two, so undo and redo are the same operation; like `stack_replaced` it names no layer,
    // so no change to the stack can make it inapplicable. A canvas-sized plane per step is the price,
    // and it is a quarter of what a full-canvas pixel step costs.
    void record_coverage(std::optional<Mask> marked);

    // A layer's **mask plane** - a mask put on a layer that had none, or taken off one that had it. The
    // same swap as `record_coverage`, aimed at one layer instead of the document, and `nullopt` is one
    // of the two states travelling through it: the layer had no mask. Coverage written *into* a mask a
    // layer already has is `record` with `LayerPlane::mask`, not this; what makes a plane arriving its
    // own entry is that there is no rectangle to write into and no bytes to displace, so the step is the
    // plane's existence and restoring it means putting the whole of it back.
    void record_layer_mask(std::uint64_t layer_id, std::optional<Mask> mask);

    struct Applied {
        std::uint64_t layer;
        Rect rect;
        // The step moved the layer stack, or a layer's mask plane, so what the canvas shows changed
        // everywhere it composites, not only inside `rect`.
        bool structure = false;
        // The step moved the marked coverage. It put back no pixel: the composite is byte-identical
        // and no layer holds anything different, so a step that is nothing else re-uploads and
        // resamples nothing.
        bool coverage = false;
    };

    // Put back the state the other stack holds, keeping the pixels it displaces so the
    // step can be run again. Returns what was touched, or nullopt when there is nothing to
    // step, or when a recorded rectangle no longer fits its layer - a resized layer makes
    // every rectangle it holds a lie, so the whole history is dropped rather than half
    // restored. `marked` is the document's coverage plane, swapped with what a `coverage` entry holds,
    // and a layer's own mask is swapped with what a `mask_replaced` entry holds.
    std::optional<Applied> undo(std::vector<Layer>& layers, std::optional<Mask>& marked);
    std::optional<Applied> redo(std::vector<Layer>& layers, std::optional<Mask>& marked);

    bool can_undo() const;
    bool can_redo() const;
    void clear();
    HistoryStats stats() const;

private:
    // What one entry holds. `layer_created` is the layer present in the current state, so stepping back
    // removes it; `layer_absent` is the same fact from the other side, so stepping forward puts it back.
    // Both directions push the opposite kind, which is the structural form of "an entry holds the state
    // that is not current".
    enum class Kind {
        pixels,
        layer_created,
        layer_absent,
        layer_order,
        stack_replaced,
        coverage,
        mask_replaced,
    };

    struct Stacked {
        std::uint64_t layer;
        Rect rect;
        std::vector<std::uint8_t> bytes;
        int step;  // entries sharing it are undone and redone together
        Kind kind = Kind::pixels;
        LayerPlane plane = LayerPlane::pixels;
        // Structural entries only: where the layer sat, how big it was. Its name travels inside `whole`.
        int index = 0;
        Extent size{};
        // The stack's order, for `layer_order`.
        std::vector<std::uint64_t> order;
        // The whole layer, for `layer_absent`: what was there before it was taken away. Both roads that
        // produce one carry the layer itself - `record_layer_removed` files it, and undoing a creation
        // moves the live layer into the entry - so nothing has to be guessed back.
        Layer whole;
        // The stack on the far side, for `stack_replaced`.
        std::vector<Layer> stack;
        // The marked coverage on the far side, for `coverage`. `nullopt` is a real state here - the
        // document had no marked area - so this cannot be folded into `bytes`.
        std::optional<Mask> marked;
        // The layer's own mask on the far side, for `mask_replaced`, where `nullopt` again means the
        // layer had no mask at all.
        std::optional<Mask> mask;
    };

    // What an entry costs the budget: its bytes, plus the pixels and mask of a whole layer it carries.
    static std::size_t held(const Stacked& entry);
    static bool inserts(const Stacked& entry);
    static void pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved);
    static int steps_in(const std::vector<Stacked>& stack);
    std::optional<Applied> step_back(std::vector<Stacked>& from, std::vector<Stacked>& into,
                                     std::vector<Layer>& layers, std::optional<Mask>& marked);
    void drop_oldest_until_within_budget();

    // Remove the layer a `layer_created` entry names, or re-insert the one a `layer_absent` entry names,
    // pushing the opposite kind onto `into` under `step` so the pair stays one undoable step.
    static void step_structure(Stacked& entry, std::vector<Stacked>& into,
                               std::vector<Layer>& layers, std::optional<Mask>& marked, int step);
    static Layer* layer_of(std::vector<Layer>& layers, std::uint64_t id);
    // True when the entry can still be applied: a pixel rectangle has to fit the plane it names, and a
    // structural entry has to find its layer on the side the step is stepping away from.
    static bool fits(const Layer* layer, const Stacked& entry);
    // True when an `layer_order` entry names exactly the layers in `layers`: a permutation of anything
    // else is not an order this stack can be put into.
    static bool fits_stack(const std::vector<Layer>& layers, const Stacked& entry);
    // True when a `coverage` entry's plane still covers the canvas the stack has. A coverage of
    // another size is not a claim about these pixels, so the step is refused rather than a document
    // left with a mark that reaches off its own edge.
    static bool fits_coverage(const std::vector<Layer>& layers, const Stacked& entry);

    std::size_t budget_bytes_;
    std::vector<Stacked> undo_stack_;
    std::vector<Stacked> redo_stack_;
    std::size_t bytes_ = 0;
    int next_step_ = 0;
    // Non-zero between `begin_step` and `end_step`: records land in it instead of opening a step.
    int open_step_ = 0;
};

}  // namespace noise
