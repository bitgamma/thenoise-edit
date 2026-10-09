// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/geom.h"
#include "core/image.h"

namespace noise {

// What one undo step can put back is the rectangle an operation declared, and the bytes it
// held there; the stacks themselves are private to History.
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

    // `before` must be the bytes of `rect` as it was before the change. Records nothing for
    // an empty rectangle and clears the redo stack, because the timeline has forked.
    void record(Rect rect, std::vector<std::uint8_t> before_rgba);

    // Put back the state the other stack holds, keeping the pixels it displaces so the
    // step can be run again. Returns the rectangle touched, or nullopt when there is
    // nothing to step, or when the image no longer matches what was recorded - an image
    // that was resized makes every recorded rectangle a lie, so the history is dropped.
    std::optional<Rect> undo(Image& image);
    std::optional<Rect> redo(Image& image);

    bool can_undo() const;
    bool can_redo() const;
    void clear();
    HistoryStats stats() const;

private:
    struct Stacked {
        Rect rect;
        std::vector<std::uint8_t> rgba;
        int step;  // entries sharing it are undone and redone together
    };

    static void pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved);
    static int steps_in(const std::vector<Stacked>& stack);
    std::optional<Rect> step_back(std::vector<Stacked>& from, std::vector<Stacked>& into,
                                  Image& image);
    void drop_oldest_until_within_budget();

    std::size_t budget_bytes_;
    std::vector<Stacked> undo_stack_;
    std::vector<Stacked> redo_stack_;
    std::size_t bytes_ = 0;
    int next_step_ = 0;
};

}  // namespace noise
