// SPDX-License-Identifier: Apache-2.0
#include "core/document.h"

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

bool Document::undo() {
    if (!history_.undo(image_)) {
        return false;
    }
    ++revision_;
    return true;
}

bool Document::redo() {
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
