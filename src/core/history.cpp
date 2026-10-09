// SPDX-License-Identifier: Apache-2.0
#include "core/history.h"

#include <cstring>
#include <span>
#include <utility>

namespace noise {

namespace {

void paste_rect(Image& image, Rect rect, const std::uint8_t* rgba) {
    const std::size_t row = static_cast<std::size_t>(rect.w) * 4;
    for (int y = 0; y < rect.h; ++y) {
        std::memcpy(image.at(rect.x, rect.y + y), rgba + static_cast<std::size_t>(y) * row, row);
    }
}

std::vector<std::uint8_t> copy_of(const Image& image) {
    const std::span<const std::uint8_t> bytes = image.pixels();
    return {bytes.begin(), bytes.end()};
}

}  // namespace

History::History(std::size_t budget_bytes) : budget_bytes_(budget_bytes) {}


void History::record(std::uint64_t layer_id, Rect rect, std::vector<std::uint8_t> before_rgba) {
    redo_stack_.clear();
    if (rect.empty()) {
        return;
    }
    // One record is one step today; entries sharing a number - which an operation touching
    // several rectangles can still produce - are undone, redone and dropped together.
    const int step = ++next_step_;
    bytes_ += before_rgba.size();
    undo_stack_.push_back({layer_id, rect, std::move(before_rgba), step});
    drop_oldest_until_within_budget();
}

bool History::can_undo() const { return !undo_stack_.empty(); }
bool History::can_redo() const { return !redo_stack_.empty(); }

std::optional<History::Applied> History::undo(std::vector<Layer>& layers) {
    return step_back(undo_stack_, redo_stack_, layers);
}

std::optional<History::Applied> History::redo(std::vector<Layer>& layers) {
    return step_back(redo_stack_, undo_stack_, layers);
}

void History::drop_layer(std::uint64_t layer_id) {
    const auto names = [layer_id](const Stacked& entry) { return entry.layer == layer_id; };
    for (std::vector<Stacked>* stack : {&undo_stack_, &redo_stack_}) {
        for (std::size_t index = stack->size(); index-- > 0;) {
            if (names((*stack)[index])) {
                bytes_ -= (*stack)[index].rgba.size();
                stack->erase(stack->begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
    }
}

void History::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
    bytes_ = 0;
}

HistoryStats History::stats() const {
    HistoryStats out;
    out.undo_depth = steps_in(undo_stack_);
    out.redo_depth = steps_in(redo_stack_);
    out.bytes = bytes_;
    return out;
}

Image* History::layer_of(std::vector<Layer>& layers, std::uint64_t id) {
    for (Layer& layer : layers) {
        if (layer.id == id) {
            return &layer.pixels;
        }
    }
    return nullptr;
}

std::optional<History::Applied> History::step_back(std::vector<Stacked>& from,
                                                   std::vector<Stacked>& into,
                                                   std::vector<Layer>& layers) {
    if (from.empty()) {
        return std::nullopt;
    }
    for (const Stacked& entry : from) {
        Image* target = layer_of(layers, entry.layer);
        // A layer that is gone should already have been dropped by `drop_layer`; arriving here
        // means the caller lost track, which is the same class of lie as a resized layer.
        if (!target || !entry.rect.inside(target->size())) {
            clear();
            return std::nullopt;
        }
    }
    std::vector<Stacked> group;
    pop_step(from, group);
    const int step = ++next_step_;
    Applied touched;
    // Newest rectangle first: the step goes back in the order it came, then reversed.
    for (auto it = group.rbegin(); it != group.rend(); ++it) {
        Stacked entry = std::move(*it);
        Image& target = *layer_of(layers, entry.layer);
        touched.layer = entry.layer;
        touched.rect = unite(touched.rect, entry.rect);
        bytes_ -= entry.rgba.size();
        Image displaced = target.region(entry.rect);
        into.push_back({entry.layer, entry.rect, copy_of(displaced), step});
        bytes_ += into.back().rgba.size();
        paste_rect(target, entry.rect, entry.rgba.data());
    }
    return touched;
}

void History::pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved) {
    const int step = stack.back().step;
    while (!stack.empty() && stack.back().step == step) {
        moved.push_back(std::move(stack.back()));
        stack.pop_back();
    }
}

int History::steps_in(const std::vector<Stacked>& stack) {
    int steps = 0;
    int previous = 0;
    for (const Stacked& entry : stack) {
        steps += entry.step != previous ? 1 : 0;
        previous = entry.step;
    }
    return steps;
}

void History::drop_oldest_until_within_budget() {
    while (bytes_ > budget_bytes_ && !undo_stack_.empty()) {
        const int step = undo_stack_.front().step;
        while (!undo_stack_.empty() && undo_stack_.front().step == step) {
            bytes_ -= undo_stack_.front().rgba.size();
            undo_stack_.erase(undo_stack_.begin());
        }
    }
}

}  // namespace noise
