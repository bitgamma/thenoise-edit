// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

    // A layer this step created. Undo takes the layer away; redo re-inserts it, empty, at `index`, and
    // the pixels it received arrive with it as the pixel entries of the same step. `size` is what makes a
    // re-inserted layer the layer it was: a rectangle of the same step still has to fit it.
    void record_layer_created(std::uint64_t layer_id, std::string name, int index, Extent size);

    struct Applied {
        std::uint64_t layer;
        Rect rect;
        // The step moved the layer stack, so what the canvas shows changed everywhere it composites,
        // not only inside `rect`.
        bool structure = false;
    };

    // Put back the state the other stack holds, keeping the pixels it displaces so the
    // step can be run again. Returns what was touched, or nullopt when there is nothing to
    // step, or when a recorded rectangle no longer fits its layer - a resized layer makes
    // every rectangle it holds a lie, so the whole history is dropped rather than half
    // restored.
    std::optional<Applied> undo(std::vector<Layer>& layers);
    std::optional<Applied> redo(std::vector<Layer>& layers);

    // A change to a layer that has been deleted cannot be put back. The entries naming that
    // layer leave both stacks; the rest of the timeline, including other layers of the same
    // step, stays, because losing an unrelated layer's undo is worse than losing one that is
    // impossible.
    void drop_layer(std::uint64_t layer_id);
    // The same for a layer's mask: entries naming coverage that is gone cannot be restored, so they
    // leave both stacks while the rest of the timeline stays.
    void drop_mask(std::uint64_t layer_id);

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
    };

    struct Stacked {
        std::uint64_t layer;
        Rect rect;
        std::vector<std::uint8_t> bytes;
        int step;  // entries sharing it are undone and redone together
        Kind kind = Kind::pixels;
        LayerPlane plane = LayerPlane::pixels;
        // Structural entries only: where the layer sat, what it was called, how big it was.
        std::string name;
        int index = 0;
        Extent size{};
    };

    static void pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved);
    static int steps_in(const std::vector<Stacked>& stack);
    std::optional<Applied> step_back(std::vector<Stacked>& from, std::vector<Stacked>& into,
                                     std::vector<Layer>& layers);
    void drop_oldest_until_within_budget();
    // Both stacks, entries the predicate names. Whole steps are dropped by
    // `drop_oldest_until_within_budget`, not by this: what is removed here cannot be restored at all.
    void drop_entries(const std::function<bool(const Stacked&)>& names);
    void drop_where(std::vector<Layer>& nothing, const std::function<bool(const Stacked&)>& names);

    // Remove the layer a `layer_created` entry names, or re-insert the one a `layer_absent` entry names,
    // pushing the opposite kind onto `into` under `step` so the pair stays one undoable step.
    static void step_structure(Stacked& entry, std::vector<Stacked>& into,
                               std::vector<Layer>& layers, int step);
    static Layer* layer_of(std::vector<Layer>& layers, std::uint64_t id);
    // True when the entry can still be applied: a pixel rectangle has to fit the plane it names, and a
    // structural entry has to find its layer on the side the step is stepping away from.
    static bool fits(const Layer* layer, const Stacked& entry);

    std::size_t budget_bytes_;
    std::vector<Stacked> undo_stack_;
    std::vector<Stacked> redo_stack_;
    std::size_t bytes_ = 0;
    int next_step_ = 0;
    // Non-zero between `begin_step` and `end_step`: records land in it instead of opening a step.
    int open_step_ = 0;
};

}  // namespace noise
