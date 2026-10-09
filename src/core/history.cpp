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
    bytes_ += before_rgba.size();
    undo_stack_.push_back({rect, std::move(before_rgba)});
    drop_oldest_until_within_budget();
}

bool History::can_undo() const { return !undo_stack_.empty(); }
bool History::can_redo() const { return !redo_stack_.empty(); }

std::optional<Rect> History::undo(Image& image) {
    if (undo_stack_.empty()) {
        return std::nullopt;
    }
    if (!undo_stack_.back().rect.inside(image.size())) {
        clear();
        return std::nullopt;
    }
    Stacked entry = undo_stack_.back();
    undo_stack_.pop_back();
    swap_in(image, std::move(entry), redo_stack_);
    return entry.rect;
}

std::optional<Rect> History::redo(Image& image) {
    if (redo_stack_.empty()) {
        return std::nullopt;
    }
    if (!redo_stack_.back().rect.inside(image.size())) {
        clear();
        return std::nullopt;
    }
    Stacked entry = redo_stack_.back();
    redo_stack_.pop_back();
    swap_in(image, std::move(entry), undo_stack_);
    return entry.rect;
}

void History::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
    bytes_ = 0;
}

HistoryStats History::stats() const {
    HistoryStats out;
    out.undo_depth = static_cast<int>(undo_stack_.size());
    out.redo_depth = static_cast<int>(redo_stack_.size());
    out.bytes = bytes_;
    return out;
}

void History::swap_in(Image& image, Stacked entry, std::vector<Stacked>& keep) {
    bytes_ -= entry.rgba.size();
    Image displaced = image.region(entry.rect);
    keep.push_back({entry.rect, copy_of(displaced)});
    bytes_ += keep.back().rgba.size();
    paste_rect(image, entry.rect, entry.rgba.data());
}

void History::drop_oldest_until_within_budget() {
    while (bytes_ > budget_bytes_ && !undo_stack_.empty()) {
        bytes_ -= undo_stack_.front().rgba.size();
        undo_stack_.erase(undo_stack_.begin());
    }
}

}  // namespace noise
