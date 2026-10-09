// SPDX-License-Identifier: Apache-2.0
#include "core/document.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/blend.h"
#include "core/hash.h"

namespace noise {

Document::Document() : history_(kHistoryBudgetBytes) {}

void Document::reset(std::vector<Layer> layers, std::string path) {
    layers_ = std::move(layers);
    for (Layer& layer : layers_) {
        layer.id = next_layer_id_++;
    }
    active_ = layers_.empty() ? 0 : static_cast<int>(layers_.size()) - 1;
    pending_ = Rect{0, 0, width(), height()};
    path_ = std::move(path);
    dirty_ = path_.empty();  // nothing on disk holds this document yet
    history_.clear();
    ++revision_;
}

const std::string& Document::path() const { return path_; }

void Document::set_path(std::string path) { path_ = std::move(path); }

bool Document::dirty() const { return dirty_; }

void Document::mark_saved() { dirty_ = false; }

bool Document::empty() const { return layers_.empty(); }

int Document::width() const { return layers_.empty() ? 0 : layers_.front().pixels.size().w; }
int Document::height() const { return layers_.empty() ? 0 : layers_.front().pixels.size().h; }
Extent Document::size() const { return layers_.empty() ? Extent{0, 0} : layers_.front().pixels.size(); }

std::span<const Layer> Document::layers() const { return layers_; }

int Document::active() const { return active_; }

const Layer& Document::layer(std::size_t index) const { return layers_[index]; }

int Document::add_layer(std::string name, Rgba8 fill) {
    if (empty()) {
        return -1;
    }
    const int at = active_ + 1;
    Layer added;
    added.id = next_layer_id_++;
    added.name = name.empty() ? default_layer_name(layers_.size()) : std::move(name);
    added.pixels = Image::filled(size(), fill.r, fill.g, fill.b, fill.a);
    layers_.insert(layers_.begin() + at, std::move(added));
    active_ = at;
    pending_ = Rect{0, 0, width(), height()};
    dirty_ = true;
    ++revision_;
    return at;
}

bool Document::remove_layer(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    if (layers_.size() == kMinLayers) {
        return false;
    }
    history_.drop_layer(layers_[static_cast<std::size_t>(index)].id);
    layers_.erase(layers_.begin() + index);
    pending_ = Rect{0, 0, width(), height()};
    if (index < active_) {
        --active_;
    }
    // Removing the top layer leaves the one below it active, at the index the removed one had.
    active_ = std::min(active_, static_cast<int>(layers_.size()) - 1);
    dirty_ = true;
    ++revision_;
    return true;
}

bool Document::select_layer(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    active_ = index;
    return true;
}

std::string Document::checksum() const {
    std::string seed = std::to_string(layers_.size());
    for (const Layer& layer : layers_) {
        seed += layer.pixels.checksum_hex();
    }
    return sha256_hex(seed);
}

const Image& Document::composite() const {
    if (layers_.size() == 1) {
        // One layer over nothing is that layer. This is not a shortcut: blending a stack of
        // one has no second operand, and taking the blend path anyway would put a full-image
        // pass on every frame of a single-layer document.
        return layers_.front().pixels;
    }
    if (empty()) {
        composite_ = Image{};
        pending_ = Rect{};
        return composite_;
    }
    const Extent canvas = size();
    if (composite_.size() != canvas) {
        composite_ = Image::filled(canvas, 0, 0, 0, 0);
        pending_ = Rect{0, 0, canvas.w, canvas.h};
    }
    const Rect region = intersect(pending_, Rect{0, 0, canvas.w, canvas.h});
    if (!region.empty()) {
        // The region is rebuilt from transparent rather than patched, so it does not matter how
        // many layers came and went since it was last correct.
        const std::uint8_t transparent[4] = {0, 0, 0, 0};
        composite_.fill_rect(region, transparent);
        for (const Layer& layer : layers_) {
            blend_over(composite_, layer.pixels, region);
        }
    }
    pending_ = Rect{};
    return composite_;
}

std::uint64_t Document::revision() const { return revision_; }

void Document::paint(Rect bounds, const std::function<void(Image&)>& edit) {
    if (empty()) {
        return;
    }
    Image& target = layers_[static_cast<std::size_t>(active_)].pixels;
    const Rect touched = intersect(bounds, Rect{0, 0, target.size().w, target.size().h});
    if (touched.empty()) {
        return;
    }
    Image before = target.region(touched);
    edit(target);
    const std::span<const std::uint8_t> bytes = before.pixels();
    history_.record(layers_[static_cast<std::size_t>(active_)].id, touched,
                    {bytes.begin(), bytes.end()});
    pending_ = unite(pending_, touched);
    dirty_ = true;
    ++revision_;
}

void Document::begin_provisional(Rect area) {
    if (empty()) {
        return;
    }
    Image& target = layers_[static_cast<std::size_t>(active_)].pixels;
    const Rect want = intersect(area, Rect{0, 0, target.size().w, target.size().h});
    if (want.empty()) {
        return;
    }
    const std::uint64_t id = layers_[static_cast<std::size_t>(active_)].id;
    if (provisional_) {
        const Rect grown = unite(provisional_->rect, want);
        if (grown == provisional_->rect) {
            return;
        }
        paste(*layer_pixels(provisional_->layer), provisional_->rect, provisional_->pixels);
        provisional_ = Provisional{id, grown, target.region(grown)};
        pending_ = unite(pending_, grown);
        ++revision_;
        return;
    }
    provisional_ = Provisional{id, want, target.region(want)};
}

void Document::repaint_provisional(const std::function<void(Image&)>& paint) {
    if (!provisional_) {
        return;
    }
    Image* target = layer_pixels(provisional_->layer);
    if (!target) {
        provisional_.reset();
        return;
    }
    paste(*target, provisional_->rect, provisional_->pixels);
    paint(*target);
    pending_ = unite(pending_, provisional_->rect);
    ++revision_;
}

void Document::commit_provisional() {
    if (!provisional_) {
        return;
    }
    const std::span<const std::uint8_t> bytes = provisional_->pixels.pixels();
    history_.record(provisional_->layer, provisional_->rect, {bytes.begin(), bytes.end()});
    dirty_ = true;
    provisional_.reset();
}

void Document::discard_provisional() {
    if (!provisional_) {
        return;
    }
    if (Image* target = layer_pixels(provisional_->layer)) {
        paste(*target, provisional_->rect, provisional_->pixels);
        pending_ = unite(pending_, provisional_->rect);
    }
    provisional_.reset();
    ++revision_;
}

bool Document::in_provisional() const { return provisional_.has_value(); }

Rect Document::provisional_area() const { return provisional_ ? provisional_->rect : Rect{}; }

Image* Document::layer_pixels(std::uint64_t id) {
    for (Layer& layer : layers_) {
        if (layer.id == id) {
            return &layer.pixels;
        }
    }
    return nullptr;
}

void Document::paste(Image& into, Rect rect, const Image& snapshot) {
    const std::size_t row = static_cast<std::size_t>(rect.w) * 4;
    for (int y = 0; y < rect.h; ++y) {
        std::memcpy(into.at(rect.x, rect.y + y), snapshot.at(0, y), row);
    }
}

bool Document::undo() {
    discard_provisional();
    const auto moved = history_.undo(layers_);
    if (!moved) {
        return false;
    }
    pending_ = unite(pending_, moved->rect);
    ++revision_;
    return true;
}

bool Document::redo() {
    discard_provisional();
    const auto moved = history_.redo(layers_);
    if (!moved) {
        return false;
    }
    pending_ = unite(pending_, moved->rect);
    ++revision_;
    return true;
}
bool Document::can_undo() const { return history_.can_undo(); }
bool Document::can_redo() const { return history_.can_redo(); }
HistoryStats Document::history() const { return history_.stats(); }

}  // namespace noise
