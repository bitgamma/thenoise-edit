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

bool Document::set_layer_visible(int index, bool visible) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.visible == visible) {
        return true;
    }
    layer.visible = visible;
    // Whole canvas, not a rectangle: what is correct elsewhere depends on whether the composite
    // was even built while this layer was hidden.
    pending_ = Rect{0, 0, width(), height()};
    dirty_ = true;
    ++revision_;
    return true;
}

bool Document::set_layer_opacity(int index, double opacity) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    if (!(opacity >= 0.0 && opacity <= 1.0)) {
        return false;
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.opacity == opacity) {
        return true;
    }
    layer.opacity = opacity;
    pending_ = Rect{0, 0, width(), height()};
    dirty_ = true;
    ++revision_;
    return true;
}

std::string Document::checksum() const {
    std::string seed = std::to_string(layers_.size());
    for (const Layer& layer : layers_) {
        // The layer's state as well as its pixels: two documents that show different things are
        // different documents, whatever their flattened pixels happen to look like, and a
        // round-trip check that could not see a lost `visible` flag would pass on data loss.
        seed += layer.visible ? 'v' : 'h';
        const char* raw = reinterpret_cast<const char*>(&layer.opacity);
        seed.append(raw, sizeof(layer.opacity));  // the bytes, so two opacities never collide
        seed += blend_mode_name(layer.mode);
        seed += layer.pixels.checksum_hex();
    }
    return sha256_hex(seed);
}

namespace {

// The canvas rectangle of an image that covers the canvas whole.
Rect whole(const Image& image) {
    return Rect{0, 0, image.size().w, image.size().h};
}

// The one stack whose composite *is* a layer: one visible layer at full strength over nothing
// has no second operand, and taking the blend path anyway puts a canvas-sized pass on every
// frame of the cheapest document there is.
bool blends_to_itself(const std::vector<Layer>& layers) {
    if (layers.size() != 1) {
        return false;
    }
    const Layer& only = layers.front();
    return only.visible && only.opacity == kLayerOpacityDefault && only.mode == BlendMode::normal;
}

}  // namespace

const Image& Document::composite() const {
    if (blends_to_itself(layers_)) {
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
            if (!layer.visible) {
                continue;
            }
            blend_layer(composite_, layer.pixels, region, layer.mode, layer.opacity);
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
    if (!provisional_) {
        provisional_ = Provisional{id, want, target.region(want)};
        painted_ = Rect{};
        return;
    }
    const Rect grown = grown_rect(provisional_->rect, want, target.size(), kBufferGrowthFactor);
    if (grown == provisional_->rect) {
        return;
    }
    // No restore-and-resnapshot: everything outside what is already held aside has never been
    // written by this gesture, so the layer still holds its pristine bytes there. Widen from the
    // layer, then copy the held-aside part back over the overlap.
    Image wider = target.region(grown);
    copy_rows(wider, grown, provisional_->pixels, provisional_->rect, provisional_->rect);
    provisional_->pixels = std::move(wider);
    provisional_->rect = grown;
}

void Document::repaint_provisional(Rect dirty, const std::function<void(Image&, Rect)>& paint) {
    if (!provisional_) {
        return;
    }
    Image* target = layer_pixels(provisional_->layer);
    if (!target) {
        provisional_.reset();
        return;
    }
    const Rect area = intersect(dirty, provisional_->rect);
    if (area.empty()) {
        return;
    }
    copy_rows(*target, whole(*target), provisional_->pixels, provisional_->rect, area);
    paint(*target, area);
    painted_ = unite(painted_, area);
    pending_ = unite(pending_, area);
    ++revision_;
}

void Document::commit_provisional() {
    if (!provisional_) {
        return;
    }
    if (!painted_.empty()) {
        // The step covers what was painted, not the buffer that grew around it: the bytes
        // history stores are paid for by the user's undo budget.
        const Image before = held_aside(painted_);
        const std::span<const std::uint8_t> bytes = before.pixels();
        history_.record(provisional_->layer, painted_, {bytes.begin(), bytes.end()});
        dirty_ = true;
    }
    provisional_.reset();
    painted_ = Rect{};
}

void Document::discard_provisional() {
    if (!provisional_) {
        return;
    }
    if (Image* target = layer_pixels(provisional_->layer)) {
        copy_rows(*target, whole(*target), provisional_->pixels, provisional_->rect, painted_);
        pending_ = unite(pending_, painted_);
        ++revision_;
    }
    provisional_.reset();
    painted_ = Rect{};
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

// A row copy between images that each index from their own corner: `dst_at` and `src_at` say
// where each image sits in document coordinates, `area` is the rectangle to move. The held-aside
// snapshot is sized to the stroke and the layer to the canvas, and the snapshot grows during a
// gesture, which is what makes an origin on each side the only honest signature.
void Document::copy_rows(Image& dst, Rect dst_at, const Image& src, Rect src_at,
                         Rect area) const {
    if (area.empty()) {
        return;
    }
    const std::size_t row = static_cast<std::size_t>(area.w) * 4;
    for (int y = 0; y < area.h; ++y) {
        std::memcpy(dst.at(area.x - dst_at.x, area.y + y - dst_at.y),
                    src.at(area.x - src_at.x, area.y + y - src_at.y), row);
    }
}

Image Document::held_aside(Rect absolute) const {
    Image out = Image::filled(Extent{absolute.w, absolute.h}, 0, 0, 0, 0);
    // `out` sits at `absolute` in document coordinates, which is what makes it index from zero.
    copy_rows(out, absolute, provisional_->pixels, provisional_->rect, absolute);
    return out;
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
