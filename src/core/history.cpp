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


void History::record(Rect rect, std::vector<std::uint8_t> before_rgba) {
    redo_stack_.clear();
    if (rect.empty()) {
        return;
    }
    // One record is one step today; entries sharing a number - which an operation touching
    // several rectangles can still produce - are undone, redone and dropped together.
    const int step = ++next_step_;
    bytes_ += before_rgba.size();
    undo_stack_.push_back({rect, std::move(before_rgba), step});
    drop_oldest_until_within_budget();
}

bool History::can_undo() const { return !undo_stack_.empty(); }
bool History::can_redo() const { return !redo_stack_.empty(); }

std::optional<Rect> History::undo(Image& image) {
    return step_back(undo_stack_, redo_stack_, image);
}

std::optional<Rect> History::redo(Image& image) {
    return step_back(redo_stack_, undo_stack_, image);
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

std::optional<Rect> History::step_back(std::vector<Stacked>& from, std::vector<Stacked>& into,
                                      Image& image) {
    if (from.empty()) {
        return std::nullopt;
    }
    for (const Stacked& entry : from) {
        if (!entry.rect.inside(image.size())) {
            clear();  // A resized image makes every recorded rectangle a lie.
            return std::nullopt;
        }
    }
    std::vector<Stacked> group;
    pop_step(from, group);
    const int step = ++next_step_;
    Rect touched;
    // Newest rectangle first: the step goes back in the order it came, then reversed.
    for (auto it = group.rbegin(); it != group.rend(); ++it) {
        Stacked entry = std::move(*it);
        touched = unite(touched, entry.rect);
        bytes_ -= entry.rgba.size();
        Image displaced = image.region(entry.rect);
        into.push_back({entry.rect, copy_of(displaced), step});
        bytes_ += into.back().rgba.size();
        paste_rect(image, entry.rect, entry.rgba.data());
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
