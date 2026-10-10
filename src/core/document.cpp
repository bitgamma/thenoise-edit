// SPDX-License-Identifier: Apache-2.0
#include "core/document.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/blend.h"
#include "core/hash.h"
#include "core/log.h"

namespace noise {

Document::Document() : history_(kHistoryBudgetBytes) {}

void Document::reset(std::vector<Layer> layers, std::optional<Mask> selection, std::string path) {
    layers_ = std::move(layers);
    for (Layer& layer : layers_) {
        layer.id = next_layer_id_++;
    }
    active_ = layers_.empty() ? 0 : static_cast<int>(layers_.size()) - 1;
    pending_ = Rect{0, 0, width(), height()};
    path_ = std::move(path);
    dirty_ = path_.empty();  // nothing on disk holds this document yet
    history_.clear();
    selection_ = std::move(selection);
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

bool Document::set_layer_mask(int index, std::optional<Mask> mask) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    if (mask && mask->size() != size()) {
        return false;
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.mask == mask) {
        return true;
    }
    // A gesture holding this layer's coverage ends here rather than writing over a plane that has
    // since been replaced: it is restored as it was, and not committed.
    discard_provisional();
    if (!mask) {
        // Steps naming coverage that is gone cannot be put back, so they leave the timeline. The
        // steps naming the same layer's colour stay, as they do when a whole layer is deleted.
        history_.drop_mask(layer.id);
    }
    // The whole canvas, not a box: a mask hides what is *outside* its bounds as much as what is
    // inside, so the pixels that changed are where two coverages differ - which is not a rectangle,
    // and a pending region taken from either mask's bounds would leave the composite showing the
    // mask that used to be. Same rule as visibility, opacity and mode.
    pending_ = Rect{0, 0, width(), height()};
    layer.mask = std::move(mask);
    dirty_ = true;
    ++revision_;
    return true;
}

bool Document::set_layer_mode(int index, BlendMode mode) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.mode == mode) {
        return true;
    }
    layer.mode = mode;
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
        // A layer mask is part of what the layer holds, so two stacks differing only in what they
        // hide are different documents: `n` for no mask, `m` plus the coverage's own hash otherwise.
        // Its extent is implicit in the byte count, as it is for the pixels above.
        if (layer.mask) {
            seed += 'm';
            seed += sha256_hex(layer.mask->plane());
        } else {
            seed += 'n';
        }
    }
    return sha256_hex(seed);
}

namespace {

// The canvas rectangle of an image that covers the canvas whole.
Rect whole(const Image& image) {
    return Rect{0, 0, image.size().w, image.size().h};
}

// The one stack whose composite *is* a layer: one visible layer at full strength over nothing has
// no second operand, and taking the blend path anyway puts a canvas-sized pass on every frame of
// the cheapest document there is.
//
// Any mode, not just `normal`: with an empty backdrop the composite reduces to the source itself
// whatever the separable arithmetic said, because there is nothing to blend with. Pinned by
// `test_document`, since a mode that broke this shortcut would be wrong in a way the shortcut
// hides.
bool blends_to_itself(const std::vector<Layer>& layers) {
    if (layers.size() != 1) {
        return false;
    }
    const Layer& only = layers.front();
    // A mask is not part of the layer's pixels, so a masked layer is *not* itself: over an empty
    // backdrop the composite would have to be the layer scaled by coverage, which is a pass.
    return only.visible && only.opacity == kLayerOpacityDefault && !only.mask;
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
        composite_.fill_rect(region, kEmptyPixel);
        for (const Layer& layer : layers_) {
            if (!layer.visible) {
                continue;
            }
            const Image* source = &layer.pixels;
            if (layer.mask) {
                scale_by_coverage(*layer.mask, region, layer.pixels, masked_);
                source = &masked_;
            }
            blend_layer(composite_, *source, region, layer.mode, layer.opacity);
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
    if (selection_) {
        restrict_to(*selection_, touched, before, touched, target);
    }
    // An edit that changed no pixel is not an edit. Painting into an area the selection does not
    // reach, or stamping a colour identical to what was there, would otherwise spend a step of the
    // undo budget restoring bytes the document never lost.
    if (unchanged(before, target, touched)) {
        return;
    }
    const std::span<const std::uint8_t> bytes = before.pixels();
    history_.record(layers_[static_cast<std::size_t>(active_)].id, LayerPlane::pixels, touched,
                    {bytes.begin(), bytes.end()});
    pending_ = unite(pending_, touched);
    dirty_ = true;
    ++revision_;
}

bool Document::paint_mask(Rect bounds, const std::function<void(Mask&)>& edit) {
    if (empty()) {
        return false;
    }
    Layer& target = layers_[static_cast<std::size_t>(active_)];
    if (!target.mask) {
        return false;
    }
    const Rect touched = intersect(bounds, Rect{0, 0, width(), height()});
    if (touched.empty()) {
        return true;
    }
    const Mask before = target.mask->region(touched);
    edit(*target.mask);
    // A mask is a write like any other, so the selection fences it too: painting coverage into the
    // part of a layer that has been selected out is not what a selection means.
    if (selection_) {
        target.mask->restrict_to(*selection_, touched, before, touched);
    }
    if (unchanged(before, *target.mask, touched)) {
        return true;
    }
    history_.record(target.id, LayerPlane::mask, touched, before.plane());
    pending_ = unite(pending_, touched);
    dirty_ = true;
    ++revision_;
    return true;
}

bool Document::set_selection(std::optional<Mask> selection) {
    if (selection && selection->size() != size()) {
        return false;
    }
    if (selection_ == selection) {
        return true;
    }
    selection_ = std::move(selection);
    dirty_ = true;
    return true;
}

bool Document::unchanged(const Image& before, const Image& after, Rect region) const {
    const std::size_t row = static_cast<std::size_t>(region.w) * kChannels;
    for (int y = 0; y < region.h; ++y) {
        if (std::memcmp(before.at(0, y), after.at(region.x, region.y + y), row) != 0) {
            return false;
        }
    }
    return true;
}

bool Document::unchanged(const Mask& before, const Mask& after, Rect region) const {
    for (int y = 0; y < region.h; ++y) {
        if (std::memcmp(before.row(y).data(), after.row(region.y + y).data() + region.x,
                        static_cast<std::size_t>(region.w)) != 0) {
            return false;
        }
    }
    return true;
}

void Document::copy_coverage_rows(Mask& dst, Rect dst_at, const Mask& src, Rect src_at,
                                  Rect area) const {
    if (area.empty()) {
        return;
    }
    const std::size_t row = static_cast<std::size_t>(area.w);
    for (int y = 0; y < area.h; ++y) {
        std::memcpy(dst.row(area.y + y).data() + (area.x - dst_at.x),
                    src.row(area.y + y).data() + (area.x - src_at.x), row);
    }
}

bool Document::begin_provisional(Rect area, LayerPlane plane) {
    if (empty()) {
        return false;
    }
    Layer& target = layers_[static_cast<std::size_t>(active_)];
    if (plane == LayerPlane::mask && !target.mask) {
        return false;
    }
    const Extent canvas = plane == LayerPlane::pixels ? target.pixels.size() : target.mask->size();
    const Rect want = intersect(area, Rect{0, 0, canvas.w, canvas.h});
    if (want.empty()) {
        return false;
    }
    if (!provisional_) {
        provisional_ = Provisional{
            target.id, plane, want,
            plane == LayerPlane::pixels ? std::variant<Image, Mask>(target.pixels.region(want))
                                        : std::variant<Image, Mask>(target.mask->region(want))};
        painted_ = Rect{};
        return true;
    }
    if (provisional_->layer != target.id || provisional_->plane != plane) {
        // One gesture, one plane of one layer: what is held aside belongs to what the button found,
        // and widening it with another plane's bytes would restore the wrong ones on commit.
        log::error(std::string("a gesture already holds ") +
                   layer_plane_name(provisional_->plane) + ", so " + layer_plane_name(plane) +
                   " was not added to it");
        return false;
    }
    const Rect grown = grown_rect(provisional_->rect, want, canvas, kBufferGrowthFactor);
    if (grown == provisional_->rect) {
        return true;
    }
    // No restore-and-resnapshot: everything outside what is already held aside has never been
    // written by this gesture, so the layer still holds its pristine bytes there. Widen from the
    // layer, then copy the held-aside part back over the overlap.
    if (plane == LayerPlane::pixels) {
        Image wider = target.pixels.region(grown);
        copy_rows(wider, grown, std::get<Image>(provisional_->held), provisional_->rect,
                  provisional_->rect);
        provisional_->held = std::move(wider);
    } else {
        Mask wider = target.mask->region(grown);
        copy_coverage_rows(wider, grown, std::get<Mask>(provisional_->held), provisional_->rect,
                           provisional_->rect);
        provisional_->held = std::move(wider);
    }
    provisional_->rect = grown;
    return true;
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
    if (provisional_->plane != LayerPlane::pixels) {
        log::error("repaint_provisional ran against a gesture holding coverage");
        return;
    }
    const Rect area = intersect(dirty, provisional_->rect);
    if (area.empty()) {
        return;
    }
    const Image& held = std::get<Image>(provisional_->held);
    copy_rows(*target, whole(*target), held, provisional_->rect, area);
    paint(*target, area);
    if (selection_) {
        // The held-aside snapshot is the `before` this write started from, so restricting costs no
        // extra copy: the gesture's own buffer is what a pixel outside the selection returns to.
        restrict_to(*selection_, area, held, provisional_->rect, *target);
    }
    painted_ = unite(painted_, area);
    pending_ = unite(pending_, area);
    ++revision_;
}

void Document::repaint_provisional_mask(Rect dirty,
                                        const std::function<void(Mask&, Rect)>& paint) {
    if (!provisional_) {
        return;
    }
    if (provisional_->plane != LayerPlane::mask) {
        log::error("repaint_provisional_mask ran against a gesture holding pixels");
        return;
    }
    Mask* target = layer_mask(provisional_->layer);
    if (!target) {
        provisional_.reset();
        return;
    }
    const Rect area = intersect(dirty, provisional_->rect);
    if (area.empty()) {
        return;
    }
    const Mask& held = std::get<Mask>(provisional_->held);
    copy_coverage_rows(*target, Rect{0, 0, target->size().w, target->size().h}, held,
                       provisional_->rect, area);
    paint(*target, area);
    if (selection_) {
        // The gesture's own buffer is what a pixel outside the selection returns to, exactly as for
        // colour, so a masked stroke costs no extra copy.
        target->restrict_to(*selection_, area, held, provisional_->rect);
    }
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
        const bool coverage = provisional_->plane == LayerPlane::mask;
        if (coverage) {
            const Mask before = held_aside_coverage(painted_);
            Mask* target = layer_mask(provisional_->layer);
            if (target == nullptr || unchanged(before, *target, painted_)) {
                provisional_.reset();
                painted_ = Rect{};
                return;
            }
            history_.record(provisional_->layer, LayerPlane::mask, painted_, before.plane());
        } else {
            const Image before = held_aside(painted_);
            Image* target = layer_pixels(provisional_->layer);
            if (target == nullptr || unchanged(before, *target, painted_)) {
                // A gesture the selection stopped, or a stroke that painted the colour that was
                // already there, has nothing to put back.
                provisional_.reset();
                painted_ = Rect{};
                return;
            }
            const std::span<const std::uint8_t> bytes = before.pixels();
            history_.record(provisional_->layer, LayerPlane::pixels, painted_,
                            {bytes.begin(), bytes.end()});
        }
        dirty_ = true;
    }
    provisional_.reset();
    painted_ = Rect{};
}

void Document::discard_provisional() {
    if (!provisional_) {
        return;
    }
    if (provisional_->plane == LayerPlane::pixels) {
        if (Image* target = layer_pixels(provisional_->layer)) {
            copy_rows(*target, whole(*target), std::get<Image>(provisional_->held),
                      provisional_->rect, painted_);
            pending_ = unite(pending_, painted_);
            ++revision_;
        }
    } else if (Mask* target = layer_mask(provisional_->layer)) {
        copy_coverage_rows(*target, Rect{0, 0, target->size().w, target->size().h},
                           std::get<Mask>(provisional_->held), provisional_->rect, painted_);
        pending_ = unite(pending_, painted_);
        ++revision_;
    }
    provisional_.reset();
    painted_ = Rect{};
}

bool Document::in_provisional() const { return provisional_.has_value(); }

Rect Document::provisional_area() const { return provisional_ ? provisional_->rect : Rect{}; }

Mask* Document::layer_mask(std::uint64_t id) {
    for (Layer& layer : layers_) {
        if (layer.id == id) {
            return layer.mask ? &*layer.mask : nullptr;
        }
    }
    return nullptr;
}

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
    copy_rows(out, absolute, std::get<Image>(provisional_->held), provisional_->rect, absolute);
    return out;
}

Mask Document::held_aside_coverage(Rect absolute) const {
    Mask out(Extent{absolute.w, absolute.h});
    // `out` sits at `absolute` in document coordinates, which is what makes it index from zero.
    copy_coverage_rows(out, absolute, std::get<Mask>(provisional_->held), provisional_->rect,
                       absolute);
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
