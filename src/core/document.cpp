// SPDX-License-Identifier: Apache-2.0
#include "core/document.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/blend.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/resample.h"

#include <cmath>
#include <limits>

namespace noise {

Document::Document() : history_(kHistoryBudgetBytes) {}

void Document::reset(std::vector<Layer> layers, std::optional<Mask> selection, std::string path) {
    layers_ = std::move(layers);
    ++geometry_revision_;
    for (Layer& layer : layers_) {
        layer.id = next_layer_id_++;
    }
    active_ = layers_.empty() ? 0 : static_cast<int>(layers_.size()) - 1;
    pending_ = Rect{0, 0, width(), height()};
    path_ = std::move(path);
    dirty_ = path_.empty();  // nothing on disk holds this document yet
    history_.clear();
    marked_gesture_ = false;
    marked_had_selection_ = false;
    marked_held_ = Mask();
    selection_ = std::move(selection);
    // Every layer here is a layer this document made just now, under an id no viewer has ever seen, so
    // nothing of theirs is cached to invalidate.
    changed_composite();
}

const std::string& Document::path() const { return path_; }

void Document::set_path(std::string path) { path_ = std::move(path); }

bool Document::dirty() const { return dirty_; }

void Document::mark_saved() { dirty_ = false; }

bool Document::empty() const { return layers_.empty(); }

int Document::width() const { return layers_.empty() ? 0 : layers_.front().pixels.size().w; }
int Document::height() const { return layers_.empty() ? 0 : layers_.front().pixels.size().h; }
Extent Document::size() const { return layers_.empty() ? Extent{0, 0} : layers_.front().pixels.size(); }
std::uint64_t Document::geometry_revision() const { return geometry_revision_; }

namespace {

Mask shifted_mask(const Mask& source, Extent target, int dx, int dy) {
    Mask moved(target);
    const Rect old{dx, dy, source.size().w, source.size().h};
    const Rect overlap = intersect(old, Rect{0, 0, target.w, target.h});
    for (int y = overlap.y; y < overlap.bottom(); ++y) {
        const auto row = source.row(y - dy);
        const int from = overlap.x - dx;
        std::copy_n(row.begin() + from, overlap.w, moved.row(y).begin() + overlap.x);
    }
    return moved;
}

Mask newly_exposed(Extent old, Extent target, int dx, int dy) {
    Mask added(target);
    for (int y = 0; y < target.h; ++y) {
        auto row = added.row(y);
        for (int x = 0; x < target.w; ++x) {
            if (x < dx || y < dy || static_cast<std::int64_t>(x) >=
                    static_cast<std::int64_t>(dx) + old.w ||
                static_cast<std::int64_t>(y) >= static_cast<std::int64_t>(dy) + old.h) {
                row[static_cast<std::size_t>(x)] = kMaskIn;
            }
        }
    }
    return added;
}

}  // namespace

bool Document::resize_canvas(Extent target, int offset_x, int offset_y, bool mark_added) {
    if (empty() || target.w <= 0 || target.h <= 0 ||
        static_cast<std::int64_t>(target.w) * target.h > kMaxCanvasPixels) {
        return false;
    }
    const Extent old_size = size();
    if (static_cast<std::int64_t>(offset_x) < -old_size.w || offset_x > target.w ||
        static_cast<std::int64_t>(offset_y) < -old_size.h || offset_y > target.h) {
        return false;
    }
    if (target == old_size && offset_x == 0 && offset_y == 0) {
        return true;
    }
    discard_provisional();
    cancel_marked();
    std::vector<Layer> transformed;
    transformed.reserve(layers_.size());
    for (const Layer& source : layers_) {
        Layer next;
        next.id = source.id;
        next.name = source.name;
        next.visible = source.visible;
        next.opacity = source.opacity;
        next.mode = source.mode;
        next.content_revision = source.content_revision;
        next.pixels = Image::filled(target, 0, 0, 0, 0);
        next.pixels.blit(source.pixels, offset_x, offset_y);
        if (source.mask) {
            next.mask = shifted_mask(*source.mask, target, offset_x, offset_y);
        }
        transformed.push_back(std::move(next));
    }
    std::optional<Mask> marked = selection_
                                     ? std::optional<Mask>(shifted_mask(*selection_, target,
                                                                        offset_x, offset_y))
                                     : std::nullopt;
    if (mark_added) {
        Mask added = newly_exposed(old_size, target, offset_x, offset_y);
        if (added.covered() > 0) {
            marked = std::move(added);
        }
    }
    history_.record_canvas_replaced(std::move(layers_), std::move(selection_));
    layers_ = std::move(transformed);
    selection_ = std::move(marked);
    ++geometry_revision_;
    pending_ = Rect{0, 0, target.w, target.h};
    dirty_ = true;
    changed_all_layers();
    return true;
}

bool Document::crop_canvas(Rect crop) {
    if (empty() || crop.empty() || !crop.inside(size())) {
        return false;
    }
    return resize_canvas(Extent{crop.w, crop.h}, -crop.x, -crop.y, false);
}

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
    const std::uint64_t id = added.id;
    layers_.insert(layers_.begin() + at, std::move(added));
    active_ = at;
    // The arrival alone is the step: undoing it hands the whole layer - whatever it is wearing by then,
    // this `fill` or a painting - to the redo side, so the fill costs no bytes of its own.
    // `add_layer_written` records beside its arrival because an answer is a write with a `before`, not
    // because a layer needs something to arrive in.
    history_.record_layer_created(id, at, size());
    pending_ = Rect{0, 0, width(), height()};
    dirty_ = true;
    changed_composite();
    return at;
}

int Document::add_layer_written(std::string name, Rect bounds,
                                const std::function<bool(Layer&)>& write) {
    if (empty()) {
        return -1;
    }
    const int at = active_ + 1;
    Layer added;
    added.id = next_layer_id_++;
    added.name = name.empty() ? default_layer_name(layers_.size()) : std::move(name);
    added.pixels = Image::filled(size(), 0, 0, 0, 0);

    const Rect touched = intersect(bounds, Rect{0, 0, width(), height()});
    const Image before = touched.empty() ? Image{} : added.pixels.region(touched);
    if (!write(added)) {
        return -1;  // nothing was added, and nothing can be recorded against a layer that does not exist
    }

    layers_.insert(layers_.begin() + at, std::move(added));
    active_ = at;
    const std::uint64_t id = layers_[static_cast<std::size_t>(at)].id;
    history_.begin_step();
    history_.record_layer_created(id, at, size());
    if (!touched.empty()) {
        history_.record(id, LayerPlane::pixels, touched,
                        {before.pixels().begin(), before.pixels().end()});
    }
    history_.end_step();
    pending_ = unite(pending_, touched);
    dirty_ = true;
    // The layer this call made holds the bytes it was given and its counter says so from its first
    // picture; no other layer's content moved by adding one.
    changed_composite();
    return at;
}

void Document::retarget_active(std::uint64_t preferred) {
    if (layers_.empty()) {
        active_ = 0;
        return;
    }
    for (std::size_t at = 0; at < layers_.size(); ++at) {
        if (layers_[at].id == preferred) {
            active_ = static_cast<int>(at);
            return;
        }
    }
    if (active_ >= static_cast<int>(layers_.size())) {
        active_ = static_cast<int>(layers_.size()) - 1;
    }
}

bool Document::move_layer(int index, int to) {
    const std::size_t count = layers_.size();
    if (index < 0 || to < 0 || static_cast<std::size_t>(index) >= count ||
        static_cast<std::size_t>(to) >= count) {
        return false;
    }
    if (index == to) {
        return true;
    }
    std::vector<std::uint64_t> before;
    before.reserve(count);
    for (const Layer& layer : layers_) {
        before.push_back(layer.id);
    }
    Layer moved = std::move(layers_[static_cast<std::size_t>(index)]);
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(index));
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    active_ = to;
    history_.begin_step();
    history_.record_layer_order(std::move(before));
    history_.end_step();
    pending_ = Rect{0, 0, width(), height()};
    dirty_ = true;
    // The order is not content: each layer still holds exactly what it did, so a viewer of one layer -
    // a thumbnail - has nothing to redo, while the composite is different everywhere.
    changed_composite();
    return true;
}

bool Document::flatten_layers() {
    if (layers_.size() < 2) {
        return false;
    }
    // A copy, because what follows empties the stack the composite was read out of.
    Image shown = composite();
    const Extent canvas = size();

    // One entry holding the whole stack. No layer of the old stack survives to carry this undo, and the
    // swap it records needs no positions, so every layer comes back exactly where it was - which is not
    // something an arrival-plus-departure entry per layer can promise.
    history_.record_stack_replaced(std::move(layers_));

    Layer flat;
    flat.id = next_layer_id_++;
    flat.name = kFlattenedLayerName;
    flat.pixels = std::move(shown);
    layers_.push_back(std::move(flat));
    active_ = 0;
    pending_ = Rect{0, 0, canvas.w, canvas.h};
    dirty_ = true;
    changed_composite();
    return true;
}

bool Document::merge_down(int index) {
    if (index < 1 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    const std::size_t upper = static_cast<std::size_t>(index);
    const std::size_t lower = upper - 1;
    const Rect area{0, 0, width(), height()};
    const Layer& top = layers_[upper];
    const Image before = layers_[lower].pixels.region(area);
    if (top.visible) {
        const Image* source = &top.pixels;
        if (top.mask && !top.mask->covers_all(area)) {
            scale_by_coverage(*top.mask, area, top.pixels, masked_);
            source = &masked_;
        }
        blend_layer(layers_[lower].pixels, *source, area, top.mode, top.opacity);
    }
    Layer gone = std::move(layers_[upper]);
    history_.begin_step();
    const std::span<const std::uint8_t> bytes = before.pixels();
    history_.record(layers_[lower].id, LayerPlane::pixels, area,
                    {bytes.begin(), bytes.end()});
    history_.record_layer_removed(std::move(gone), static_cast<int>(upper));
    history_.end_step();
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(upper));
    active_ = static_cast<int>(lower);
    pending_ = area;
    dirty_ = true;
    // Exactly one layer's content moved: the one that took the other's picture. The layer that went away
    // needs no invalidation - a viewer of it has nothing left to look at, and `keep` releases its card.
    changed_layer(layers_[lower]);
    return true;
}

namespace {

// A pixel edge from a real-valued one: half up, not half away from zero. `lround` breaks the tie the
// other way for negative values, which is enough to make a factor that fits by the bound below not fit.
int edge_of(double value) {
    return static_cast<int>(std::floor(value + 0.5));
}

// A scale is one map: every point moves away from `anchor` by `factor`, and a box goes through it by its
// two corners, so the box it becomes is the box its corners say rather than two roundings apart.
Rect scale_box(Rect box, Vec2 anchor, double factor) {
    const int x0 = edge_of(anchor.x + (static_cast<double>(box.x) - anchor.x) * factor);
    const int y0 = edge_of(anchor.y + (static_cast<double>(box.y) - anchor.y) * factor);
    const int x1 = edge_of(anchor.x + (static_cast<double>(box.right()) - anchor.x) * factor);
    const int y1 = edge_of(anchor.y + (static_cast<double>(box.bottom()) - anchor.y) * factor);
    return Rect{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

// The largest factor that keeps `box` inside the canvas under that map. The bound comes from the four
// edges and is then *verified*: what is handed back is a factor that fits, not the supremum of an
// algebra that rounds the wrong way at a half. The walk steps by a fraction of an output pixel, so the
// answer is within a hair of the maximum rather than being the maximum - which is the property a refusal
// needs, since it is a number to try next.
double largest_factor_for(Rect box, Vec2 anchor, Extent canvas) {
    const double left = anchor.x - static_cast<double>(box.x);
    const double right = static_cast<double>(box.right()) - anchor.x;
    const double up = anchor.y - static_cast<double>(box.y);
    const double down = static_cast<double>(box.bottom()) - anchor.y;
    double best = std::numeric_limits<double>::max();
    if (left > 0.0) {
        best = std::min(best, (anchor.x + 0.5) / left);
    }
    if (right > 0.0) {
        best = std::min(best, (static_cast<double>(canvas.w) + 0.5 - anchor.x) / right);
    }
    if (up > 0.0) {
        best = std::min(best, (anchor.y + 0.5) / up);
    }
    if (down > 0.0) {
        best = std::min(best, (static_cast<double>(canvas.h) + 0.5 - anchor.y) / down);
    }
    const double step =
        0.5 / static_cast<double>(std::max(1, std::min(box.w, box.h)));
    while (best > 0.0 && !scale_box(box, anchor, best).inside(canvas)) {
        best -= step;
    }
    return std::max(best, 0.0);
}

// A crop of `plane` widened by `margin`, with whatever the canvas does not hold left at *nothing* rather
// than at the edge pixel repeated. Outside a document there are no pixels, and a filter that sampled the
// border twice would invent a picture that fades into a wall.
Image padded_crop(const Image& plane, Rect box, Rect canvas_rect) {
    Image out = Image::filled(Extent{box.w, box.h}, 0, 0, 0, 0);
    const Rect clip = intersect(box, canvas_rect);
    if (!clip.empty()) {
        out.blit(plane.region(clip), clip.x - box.x, clip.y - box.y);
    }
    return out;
}

Mask padded_crop(const Mask& plane, Rect box, Rect canvas_rect) {
    Mask out(Extent{box.w, box.h});
    const Rect clip = intersect(box, canvas_rect);
    for (int y = clip.y; y < clip.bottom(); ++y) {
        const std::span<const std::uint8_t> from = plane.row(y);
        std::copy_n(from.begin() + clip.x, static_cast<std::size_t>(clip.w),
                    out.row(y - box.y).begin() + (clip.x - box.x));
    }
    return out;
}

}  // namespace

LayerScale Document::scale_layer(int index, double factor) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return LayerScale{};
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Extent canvas = size();
    const Rect canvas_rect{0, 0, canvas.w, canvas.h};
    const Rect picture = layer.pixels.content_bounds();
    if (picture.empty()) {
        log::info("scale: layer '" + layer.name + "' holds nothing to scale");
        return LayerScale{false, 0.0, picture};
    }
    if (!std::isfinite(factor) || factor <= 0.0) {
        log::error("scale: layer '" + layer.name + "' was given factor " + std::to_string(factor) +
                   ", which is no size at all");
        return LayerScale{false, 0.0, picture};
    }

    const Vec2 anchor{static_cast<double>(picture.x) + picture.w / 2.0,
                      static_cast<double>(picture.y) + picture.h / 2.0};
    // Coverage that hides something travels with the picture under the same map, so its own edges bound
    // the factor too. A mask that covers the whole canvas hides nothing anywhere and is the same mask at
    // any size; filtering it would spend a pass putting ringing where there was none.
    Rect coverage{};
    bool scale_mask = false;
    if (layer.mask && layer.mask->sized()) {
        coverage = layer.mask->bounds();
        scale_mask = !coverage.empty() && !layer.mask->covers_all(canvas_rect);
    }

    // The decision is the map itself: does this factor land the picture - and the coverage over it -
    // inside the canvas. The largest factor that does is computed for the answer either way, because a
    // refusal without a number to try next is a second job for the person asking.
    const double max_factor = [&] {
        double limit = largest_factor_for(picture, anchor, canvas);
        if (scale_mask) {
            limit = std::min(limit, largest_factor_for(coverage, anchor, canvas));
        }
        return limit;
    }();
    const bool fits = scale_box(picture, anchor, factor).inside(canvas) &&
                      (!scale_mask || scale_box(coverage, anchor, factor).inside(canvas));
    if (!fits) {
        return LayerScale{false, max_factor, picture};
    }

    if (factor == 1.0) {
        // The picture lands exactly where it stood: the filter is zero at every integer offset but its
        // own, so this is a copy. Nothing changed, so there is no step, no revision and no dirty flag -
        // and no fit question either, because a copy of what already fits fits.
        return LayerScale{true, max_factor, picture};
    }

    const Rect to = scale_box(picture, anchor, factor);

    const int margin = resample_margin_px(factor);
    const int out_margin = std::max(1, static_cast<int>(std::lround(static_cast<double>(margin) * factor)));
    const Rect src_box{picture.x - margin, picture.y - margin, picture.w + 2 * margin,
                       picture.h + 2 * margin};
    const Rect dst_box{to.x - out_margin, to.y - out_margin, to.w + 2 * out_margin,
                       to.h + 2 * out_margin};

    const Image source = padded_crop(layer.pixels, src_box, canvas_rect);
    const Image scaled = resample_lanczos3(source, Extent{dst_box.w, dst_box.h});
    // Cut at the picture's own edge. The margin is input the filter's window needs, not output the layer
    // keeps: Lanczos reaches a few pixels past an edge, and a halo outside the picture would be read as
    // part of the picture by the next scale, so every scale would add a ring and the box reported would
    // stop being the box the pixels agree with.
    const Image result = scaled.region(Rect{to.x - dst_box.x, to.y - dst_box.y, to.w, to.h});
    const Rect dirty = intersect(unite(picture, to), canvas_rect);
    const Image before = layer.pixels.region(dirty);
    // The vacated part of the layer holds nothing rather than what happened to be there: a picture that
    // shrank would otherwise leave a ghost of its old size behind it.
    layer.pixels.fill_rect(dirty, kEmptyPixel);
    layer.pixels.blit(result, to.x, to.y);
    const bool pixels_moved = !unchanged(before, layer.pixels, dirty);

    Rect mask_dirty{};
    Mask before_mask{};
    bool mask_moved = false;
    if (scale_mask) {
        const Rect mask_src_box{coverage.x - margin, coverage.y - margin, coverage.w + 2 * margin,
                                coverage.h + 2 * margin};
        const Rect coverage_to = scale_box(coverage, anchor, factor);
        const Rect mask_dst_box{coverage_to.x - out_margin, coverage_to.y - out_margin,
                                coverage_to.w + 2 * out_margin, coverage_to.h + 2 * out_margin};
        const Mask mask_source = padded_crop(*layer.mask, mask_src_box, canvas_rect);
        const Mask scaled_mask =
            resample_coverage_lanczos3(mask_source, Extent{mask_dst_box.w, mask_dst_box.h});
        const Mask mask_result = scaled_mask.region(Rect{coverage_to.x - mask_dst_box.x,
                                                         coverage_to.y - mask_dst_box.y, coverage_to.w,
                                                         coverage_to.h});
        mask_dirty = intersect(unite(coverage, coverage_to), canvas_rect);
        before_mask = layer.mask->region(mask_dirty);
        layer.mask->op(mask_dirty, MaskOp::subtract);
        const Rect patch_area = intersect(coverage_to, canvas_rect);
        if (!patch_area.empty()) {
            const Mask patch =
                mask_result.region(Rect{patch_area.x - coverage_to.x, patch_area.y - coverage_to.y,
                                        patch_area.w, patch_area.h});
            layer.mask->op(patch_area, patch.plane(), MaskOp::replace);
        }
        mask_moved = !unchanged(before_mask, *layer.mask, mask_dirty);
    }

    if (!pixels_moved && !mask_moved) {
        // A scale that changed no byte is not an edit, exactly as a stroke that landed on the colour it
        // was already painted with records nothing.
        return LayerScale{true, max_factor, to};
    }

    // Both planes of one layer under one map is one thing a person did, so it comes back as one step.
    history_.begin_step();
    if (pixels_moved) {
        const std::span<const std::uint8_t> bytes = before.pixels();
        history_.record(layer.id, LayerPlane::pixels, dirty, {bytes.begin(), bytes.end()});
    }
    if (mask_moved) {
        history_.record(layer.id, LayerPlane::mask, mask_dirty, before_mask.plane());
    }
    history_.end_step();

    pending_ = unite(pending_, mask_moved ? unite(dirty, mask_dirty) : dirty);
    dirty_ = true;
    changed_layer(layer);
    log::info("scale: layer '" + layer.name + "' picture " + std::to_string(picture.w) + "x" +
              std::to_string(picture.h) + " to " + std::to_string(to.w) + "x" + std::to_string(to.h) +
              (mask_moved ? ", its coverage with it" : "") + " at factor " + std::to_string(factor));
    return LayerScale{true, max_factor, to};
}

bool Document::remove_layer(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    if (layers_.size() == kMinLayers) {
        return false;
    }
    // The whole layer goes into the step, the way a merge down files the layer it took: undo puts back
    // the pixels, mask, name, mode and opacity rather than an empty layer wearing the right name. This is
    // why `History` no longer invalidates anything by layer id - with every departure recorded, a step
    // naming a layer that is not there for the moment is one step away from being applicable, not a lie.
    Layer gone = std::move(layers_[static_cast<std::size_t>(index)]);
    history_.record_layer_removed(std::move(gone), index);
    layers_.erase(layers_.begin() + index);
    pending_ = Rect{0, 0, width(), height()};
    if (index < active_) {
        --active_;
    }
    // Removing the top layer leaves the one below it active, at the index the removed one had.
    active_ = std::min(active_, static_cast<int>(layers_.size()) - 1);
    dirty_ = true;
    changed_composite();
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
    changed_composite();
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
    // A mask plane arriving or going is the edit, and what undo puts back is the plane this replaces -
    // `nullopt` at either end meaning no mask at all. Coverage written *into* a mask is a step of its
    // own (`paint_mask`), and the steps recorded before this one stay in the timeline: they are reached
    // by undoing this swap first, which returns the mask with the coverage it was hidden by, so they
    // land on the state they were recorded against.
    history_.record_layer_mask(layer.id, layer.mask);
    // The whole canvas, not a box: a mask hides what is *outside* its bounds as much as what is
    // inside, so the pixels that changed are where two coverages differ - which is not a rectangle,
    // and a pending region taken from either mask's bounds would leave the composite showing the
    // mask that used to be. Same rule as visibility, opacity and mode.
    pending_ = Rect{0, 0, width(), height()};
    layer.mask = std::move(mask);
    dirty_ = true;
    changed_layer(layer);
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
    changed_composite();
    return true;
}

bool Document::rename_layer(int index, std::string name) {
    if (index < 0 || static_cast<std::size_t>(index) >= layers_.size()) {
        return false;
    }
    const std::optional<std::string> wanted = normalize_layer_name(name);
    if (!wanted) {
        return false;
    }
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.name == *wanted) {
        return true;
    }
    history_.record_layer_renamed(layer.id, std::move(layer.name));
    layer.name = std::move(*wanted);
    // The name is in the file and in the panel, and in nothing the renderer draws: no revision, no
    // pending rectangle and no content revision, so no card resamples its picture over a rename.
    dirty_ = true;
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
    changed_composite();
    return true;
}

std::string Document::checksum() const {
    std::string seed = std::to_string(width()) + "x" + std::to_string(height()) + ":" +
                       std::to_string(layers_.size());
    for (const Layer& layer : layers_) {
        // The layer's state as well as its pixels: two documents that show different things are
        // different documents, whatever their flattened pixels happen to look like, and a
        // round-trip check that could not see a lost `visible` flag would pass on data loss.
        seed += layer.visible ? 'v' : 'h';
        // The name too, and its length first: names are free text, so without a bound between them a
        // rename that dropped a character, or moved one into the field after it, would hash the same.
        seed += std::to_string(layer.name.size());
        seed += ':';
        seed += layer.name;
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
bool blends_to_itself(const std::vector<Layer>& layers, Extent canvas) {
    if (layers.size() != 1) {
        return false;
    }
    const Layer& only = layers.front();
    // A mask that reaches every pixel at full coverage leaves the bytes exactly as they are, so the
    // composite is still the layer. Any other mask scales the layer over an empty backdrop, which is a
    // pass and cannot be skipped.
    return only.visible && only.opacity == kLayerOpacityDefault &&
           (!only.mask || only.mask->covers_all(Rect{0, 0, canvas.w, canvas.h}));
}

}  // namespace

const Image& Document::composite() const {
    const Extent canvas = size();
    if (blends_to_itself(layers_, canvas)) {
        return layers_.front().pixels;
    }
    if (empty()) {
        composite_ = Image{};
        pending_ = Rect{};
        return composite_;
    }
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
                // Both questions are answered from the per-row counts, so neither costs a pass, and
                // each one saves two: a mask that reaches nowhere in the region makes no contribution
                // to it, and a wholly shown one asks for nothing but the blend itself. Measured on a
                // 2048² canvas over a 310x130 repair, a wholly masked layer costs what an unmasked one
                // costs instead of 5x, and a partly masked one 5x instead of 15x.
                if (layer.mask->covers_nothing(region)) {
                    continue;
                }
                if (!layer.mask->covers_all(region)) {
                    scale_by_coverage(*layer.mask, region, layer.pixels, masked_);
                    source = &masked_;
                }
            }
            blend_layer(composite_, *source, region, layer.mode, layer.opacity);
        }
    }
    pending_ = Rect{};
    return composite_;
}

Image Document::flatten_over(Rect bounds, std::size_t top_layer) const {
    // The same pass `composite()` runs, restricted to the layers at or below `top_layer` and to the
    // rectangle a request covers. It runs on a canvas-sized buffer rather than a crop-sized one because
    // `blend_layer` works in canvas coordinates; the alternative is a second blend, which is exactly how
    // a compositor and a canvas stop agreeing. A request is compiled once per job, not once per frame.
    const Rect canvas_rect{0, 0, size().w, size().h};
    const Rect region = intersect(bounds, canvas_rect);
    if (region.empty()) {
        return Image{};
    }
    Image canvas = Image::filled(size(), 0, 0, 0, 0);
    Image masked;
    const std::size_t limit = std::min(top_layer + 1, layers_.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const Layer& layer = layers_[i];
        if (!layer.visible) {
            continue;
        }
        const Image* source = &layer.pixels;
        if (layer.mask) {
            if (layer.mask->covers_nothing(region)) {
                continue;
            }
            if (!layer.mask->covers_all(region)) {
                scale_by_coverage(*layer.mask, region, layer.pixels, masked);
                source = &masked;
            }
        }
        blend_layer(canvas, *source, region, layer.mode, layer.opacity);
    }
    return canvas.region(region);
}

void Document::changed_all_layers() {
    ++revision_;
    for (Layer& layer : layers_) {
        ++layer.content_revision;
    }
}

void Document::changed_layer(Layer& layer) {
    ++revision_;
    ++layer.content_revision;
}

void Document::changed_composite() {
    ++revision_;
}

std::uint64_t Document::revision() const { return revision_; }

void Document::paint(Rect bounds, PaintFence fence, const std::function<void(Image&)>& edit) {
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
    if (fence == PaintFence::selection && selection_) {
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
    changed_layer(layers_[static_cast<std::size_t>(active_)]);
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
    changed_layer(target);
    return true;
}

bool Document::set_selection(std::optional<Mask> selection) {
    if (selection && selection->size() != size()) {
        return false;
    }
    if (selection_ == selection) {
        return true;
    }
    // Outside a gesture this write *is* the edit, and what it replaces is what undo hands back. Inside
    // one the gesture holds the single step, so recording here would price a marquee per pixel it moved
    // over.
    if (!marked_gesture_) {
        history_.record_coverage(selection_);
    }
    selection_ = std::move(selection);
    dirty_ = true;
    return true;
}

bool Document::begin_marked() {
    if (empty()) {
        return false;
    }
    if (provisional_) {
        log::error(std::string("a gesture already holds ") +
                   layer_plane_name(provisional_->plane) +
                   ", so the marked area was not added to it");
        return false;
    }
    if (marked_gesture_) {
        return true;
    }
    marked_gesture_ = true;
    marked_had_selection_ = selection_.has_value();
    // Materialised even when the document had no marked area, because a stroke needs a plane to write
    // into, and whether there was one at all is remembered apart from it - the two are different
    // documents and the gesture has to put back whichever it found.
    marked_held_ = marked_had_selection_ ? *selection_ : Mask(size());
    return true;
}

void Document::repaint_marked(Rect dirty, const std::function<void(Mask&, Rect)>& paint) {
    if (!marked_gesture_) {
        log::error("repaint_marked ran with no marked gesture open");
        return;
    }
    const Rect canvas{0, 0, width(), height()};
    const Rect area = intersect(dirty, canvas);
    if (area.empty()) {
        return;
    }
    Mask next = selection_.has_value() ? *selection_ : Mask(size());
    copy_coverage_rows(next, canvas, marked_held_, canvas, area);
    paint(next, area);
    set_selection(std::move(next));
}

void Document::commit_marked() {
    if (!marked_gesture_) {
        return;
    }
    marked_gesture_ = false;
    const std::optional<Mask> found =
        marked_had_selection_ ? std::optional<Mask>(marked_held_) : std::nullopt;
    marked_held_ = Mask();
    // A gesture that moved no coverage - a click that marked nothing over nothing - is not an edit, the
    // rule every other write follows.
    if (selection_ == found) {
        return;
    }
    history_.record_coverage(std::move(found));
}

void Document::cancel_marked() {
    if (!marked_gesture_) {
        return;
    }
    marked_gesture_ = false;
    selection_ = marked_had_selection_ ? std::optional<Mask>(marked_held_) : std::nullopt;
    marked_held_ = Mask();
}

bool Document::in_marked() const { return marked_gesture_; }

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
        // Each side is addressed from its own rectangle's corner, exactly as `copy_rows` does for
        // colour: a gesture rectangle that does not start at the top-left has to subtract it twice
        // over, once for the held-aside copy's origin and once for the plane's.
        std::memcpy(dst.row(area.y + y - dst_at.y).data() + (area.x - dst_at.x),
                    src.row(area.y + y - src_at.y).data() + (area.x - src_at.x), row);
    }
}

bool Document::begin_provisional(Rect area, LayerPlane plane) {
    if (empty()) {
        return false;
    }
    if (marked_gesture_) {
        // Symmetric with `begin_marked`: the bytes held aside are one plane's, and a gesture that
        // wrote two of them could restore neither on commit.
        log::error("a gesture already holds the marked area, so " +
                   std::string(layer_plane_name(plane)) + " was not added to it");
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
    changed_layer(*layer_of(provisional_->layer));
}

void Document::repaint_provisional_mask(Rect dirty, PaintFence fence,
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
    if (fence == PaintFence::selection && selection_) {
        target->restrict_to(*selection_, area, held, provisional_->rect);
    }
    painted_ = unite(painted_, area);
    pending_ = unite(pending_, area);
    changed_layer(*layer_of(provisional_->layer));
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
            changed_layer(*layer_of(provisional_->layer));
        }
    } else if (Mask* target = layer_mask(provisional_->layer)) {
        copy_coverage_rows(*target, Rect{0, 0, target->size().w, target->size().h},
                           std::get<Mask>(provisional_->held), provisional_->rect, painted_);
        pending_ = unite(pending_, painted_);
        changed_layer(*layer_of(provisional_->layer));
    }
    provisional_.reset();
    painted_ = Rect{};
}

bool Document::in_provisional() const { return provisional_.has_value(); }

Rect Document::provisional_area() const { return provisional_ ? provisional_->rect : Rect{}; }

Mask* Document::layer_mask(std::uint64_t id) {
    Layer* const layer = layer_of(id);
    return layer && layer->mask ? &*layer->mask : nullptr;
}

Layer* Document::layer_of(std::uint64_t id) {
    for (Layer& layer : layers_) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

Image* Document::layer_pixels(std::uint64_t id) {
    Layer* const layer = layer_of(id);
    return layer ? &layer->pixels : nullptr;
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
    cancel_marked();
    const std::uint64_t was =
        layers_.empty() ? 0 : layers_[static_cast<std::size_t>(active_)].id;
    const auto moved = history_.undo(layers_, selection_);
    if (!moved) {
        return false;
    }
    // Stepping back is a change: what the document holds now is not what the file holds, and a quit guard
    // that read this as clean would discard the step it just took.
    dirty_ = true;
    if (moved->canvas_changed) {
        ++geometry_revision_;
    }
    retarget_active(was);
    pending_ = unite(pending_, moved->rect);
    if (moved->structure) {
        pending_ = Rect{0, 0, width(), height()};
    }
    // A step that painted no rectangle and moved no layer - the mark shifting, a name going back - left
    // the canvas byte-identical and every card showing the picture it showed. Bumping the stack's
    // revision for it would re-upload a texture nobody changed and resample every thumbnail to prove
    // nothing happened; the overlay reads the coverage plane itself, per frame.
    if (!moved->structure && moved->rect.empty()) {
        return true;
    }
    changed_all_layers();
    return true;
}

bool Document::redo() {
    discard_provisional();
    cancel_marked();
    const std::uint64_t was =
        layers_.empty() ? 0 : layers_[static_cast<std::size_t>(active_)].id;
    const auto moved = history_.redo(layers_, selection_);
    if (!moved) {
        return false;
    }
    dirty_ = true;  // as for undo: the document has moved away from the file
    if (moved->canvas_changed) {
        ++geometry_revision_;
    }
    retarget_active(was);
    pending_ = unite(pending_, moved->rect);
    if (moved->structure) {
        pending_ = Rect{0, 0, width(), height()};
    }
    if (!moved->structure && moved->rect.empty()) {
        return true;
    }
    changed_all_layers();
    return true;
}
bool Document::can_undo() const { return history_.can_undo(); }
bool Document::can_redo() const { return history_.can_redo(); }
HistoryStats Document::history() const { return history_.stats(); }

}  // namespace noise
