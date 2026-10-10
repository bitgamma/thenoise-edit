// SPDX-License-Identifier: Apache-2.0
#include "core/history.h"

#include <algorithm>
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

// The coverage equivalent of `paste_rect`: one byte per pixel, rows addressed through the mask's own
// accessors so the plane's derived facts are dropped along with the write.
void paste_coverage(Mask& mask, Rect rect, const std::uint8_t* bytes) {
    const std::size_t row = static_cast<std::size_t>(rect.w);
    for (int y = 0; y < rect.h; ++y) {
        std::copy_n(bytes + static_cast<std::size_t>(y) * row, row,
                    mask.row(rect.y + y).begin() + rect.x);
    }
}

}  // namespace

History::History(std::size_t budget_bytes) : budget_bytes_(budget_bytes) {}


void History::record(std::uint64_t layer_id, LayerPlane plane, Rect rect,
                     std::vector<std::uint8_t> before) {
    redo_stack_.clear();
    if (rect.empty()) {
        return;
    }
    // One record is one step today; entries sharing a number - which an operation touching
    // several rectangles can still produce - are undone, redone and dropped together.
    const int step = ++next_step_;
    bytes_ += before.size();
    undo_stack_.push_back({layer_id, plane, rect, std::move(before), step});
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
    drop_entries([layer_id](const Stacked& entry) { return entry.layer == layer_id; });
}

void History::drop_mask(std::uint64_t layer_id) {
    drop_entries([layer_id](const Stacked& entry) {
        return entry.layer == layer_id && entry.plane == LayerPlane::mask;
    });
}

void History::drop_entries(const std::function<bool(const Stacked&)>& names) {
    for (std::vector<Stacked>* stack : {&undo_stack_, &redo_stack_}) {
        for (std::size_t index = stack->size(); index-- > 0;) {
            if (names((*stack)[index])) {
                bytes_ -= (*stack)[index].bytes.size();
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

Layer* History::layer_of(std::vector<Layer>& layers, std::uint64_t id) {
    for (Layer& layer : layers) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

bool History::fits(const Layer& layer, const Stacked& entry) {
    return entry.plane == LayerPlane::pixels ? entry.rect.inside(layer.pixels.size())
                                             : layer.mask.has_value() &&
                                                   entry.rect.inside(layer.mask->size());
}

std::optional<History::Applied> History::step_back(std::vector<Stacked>& from,
                                                   std::vector<Stacked>& into,
                                                   std::vector<Layer>& layers) {
    if (from.empty()) {
        return std::nullopt;
    }
    for (const Stacked& entry : from) {
        Layer* target = layer_of(layers, entry.layer);
        // A layer or mask that is gone should already have been dropped by `drop_layer` or
        // `drop_mask`; arriving here means the caller lost track, which is the same class of lie as
        // a resized layer.
        if (!target || !fits(*target, entry)) {
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
        Layer& layer = *layer_of(layers, entry.layer);
        touched.layer = entry.layer;
        touched.rect = unite(touched.rect, entry.rect);
        bytes_ -= entry.bytes.size();
        // The bytes this step displaces are what makes the step repeatable, and they come from the
        // same plane the entry was recorded against.
        if (entry.plane == LayerPlane::pixels) {
            const Image displaced = layer.pixels.region(entry.rect);
            into.push_back({entry.layer, entry.plane, entry.rect, copy_of(displaced), step});
            bytes_ += into.back().bytes.size();
            paste_rect(layer.pixels, entry.rect, entry.bytes.data());
        } else {
            const Mask displaced = layer.mask->region(entry.rect);
            into.push_back({entry.layer, entry.plane, entry.rect, displaced.plane(), step});
            bytes_ += into.back().bytes.size();
            paste_coverage(*layer.mask, entry.rect, entry.bytes.data());
        }
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
            bytes_ -= undo_stack_.front().bytes.size();
            undo_stack_.erase(undo_stack_.begin());
        }
    }
}

}  // namespace noise
