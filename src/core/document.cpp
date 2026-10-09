// SPDX-License-Identifier: Apache-2.0
#include "core/document.h"

#include <cstring>
#include <span>
#include <utility>

namespace noise {

Document::Document() : history_(kHistoryBudgetBytes) {}

void Document::reset(Image contents, std::string path) {
    image_ = std::move(contents);
    path_ = std::move(path);
    history_.clear();
    ++revision_;
}

const std::string& Document::path() const { return path_; }

void Document::set_path(std::string path) { path_ = std::move(path); }

bool Document::empty() const { return image_.empty(); }
int Document::width() const { return image_.size().w; }
int Document::height() const { return image_.size().h; }
Extent Document::size() const { return image_.size(); }
const Image& Document::pixels() const { return image_; }
std::string Document::checksum() const { return image_.checksum_hex(); }
std::uint64_t Document::revision() const { return revision_; }

void Document::paint(Rect bounds, const std::function<void(Image&)>& edit) {
    if (image_.empty()) {
        return;
    }
    const Rect touched = intersect(bounds, Rect{0, 0, image_.size().w, image_.size().h});
    if (touched.empty()) {
        return;
    }
    Image before = image_.region(touched);
    edit(image_);
    const std::span<const std::uint8_t> bytes = before.pixels();
    history_.record(touched, {bytes.begin(), bytes.end()});
    ++revision_;
}

void Document::begin_provisional(Rect area) {
    const Rect want = intersect(area, Rect{0, 0, image_.size().w, image_.size().h});
    if (want.empty()) {
        return;
    }
    if (provisional_) {
        const Rect grown = unite(provisional_->rect, want);
        if (grown == provisional_->rect) {
            return;
        }
        paste(provisional_->rect, provisional_->pixels);
        provisional_ = Provisional{grown, image_.region(grown)};
        ++revision_;
        return;
    }
    provisional_ = Provisional{want, image_.region(want)};
}

void Document::repaint_provisional(const std::function<void(Image&)>& paint) {
    if (!provisional_) {
        return;
    }
    paste(provisional_->rect, provisional_->pixels);
    paint(image_);
    ++revision_;
}

void Document::commit_provisional() {
    if (!provisional_) {
        return;
    }
    const std::span<const std::uint8_t> bytes = provisional_->pixels.pixels();
    history_.record(provisional_->rect, {bytes.begin(), bytes.end()});
    provisional_.reset();
}

void Document::discard_provisional() {
    if (!provisional_) {
        return;
    }
    paste(provisional_->rect, provisional_->pixels);
    provisional_.reset();
    ++revision_;
}

bool Document::in_provisional() const { return provisional_.has_value(); }

Rect Document::provisional_area() const { return provisional_ ? provisional_->rect : Rect{}; }

void Document::paste(Rect rect, const Image& snapshot) {
    const std::size_t row = static_cast<std::size_t>(rect.w) * 4;
    for (int y = 0; y < rect.h; ++y) {
        std::memcpy(image_.at(rect.x, rect.y + y), snapshot.at(0, y), row);
    }
}




bool Document::undo() {
    discard_provisional();
    if (!history_.undo(image_)) {
        return false;
    }
    ++revision_;
    return true;
}

bool Document::redo() {
    discard_provisional();
    if (!history_.redo(image_)) {
        return false;
    }
    ++revision_;
    return true;
}
bool Document::can_undo() const { return history_.can_undo(); }
bool Document::can_redo() const { return history_.can_redo(); }
HistoryStats Document::history() const { return history_.stats(); }

}  // namespace noise
