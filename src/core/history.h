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

// What one undo step can put back is the rectangle an operation declared, the layer it was on,
// and the bytes it held there; the stacks themselves are private to History.
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

    // `before` must be the bytes of `rect` in layer `layer_id` as it was before the change.
    // Records nothing for an empty rectangle and clears the redo stack, because the timeline
    // has forked.
    void record(std::uint64_t layer_id, Rect rect, std::vector<std::uint8_t> before_rgba);

    struct Applied {
        std::uint64_t layer;
        Rect rect;
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

    bool can_undo() const;
    bool can_redo() const;
    void clear();
    HistoryStats stats() const;

private:
    struct Stacked {
        std::uint64_t layer;
        Rect rect;
        std::vector<std::uint8_t> rgba;
        int step;  // entries sharing it are undone and redone together
    };

    static void pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved);
    static int steps_in(const std::vector<Stacked>& stack);
    std::optional<Applied> step_back(std::vector<Stacked>& from, std::vector<Stacked>& into,
                                     std::vector<Layer>& layers);
    void drop_oldest_until_within_budget();

    static Image* layer_of(std::vector<Layer>& layers, std::uint64_t id);

    std::size_t budget_bytes_;
    std::vector<Stacked> undo_stack_;
    std::vector<Stacked> redo_stack_;
    std::size_t bytes_ = 0;
    int next_step_ = 0;
};

}  // namespace noise
