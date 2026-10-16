// SPDX-License-Identifier: Apache-2.0
#include "app/layer_thumbs.h"

#include "core/layer.h"

#include <algorithm>
#include <vector>

namespace noise::app {
namespace {

// Box average. A thumbnail is small and a layer may be enormous, and a resample that read one pixel per
// cell would show a sparse scatter of the layer instead of its picture.
//
// The source is premultiplied, so the channels average as they are and the backdrop goes in afterwards:
// averaging *unpremultiplied* colours would drag every semi-transparent edge towards whatever colour
// happened to be under it.
// The source pixel a thumbnail cell starts at.
int cell_start(int cell, int source_extent, int box_extent) {
    return static_cast<int>(static_cast<std::int64_t>(cell) * source_extent / box_extent);
}

// ...and ends at, exclusive. Never before one past the cell's own first pixel, so a thumbnail larger
// than its layer reuses cells instead of covering none of them.
int cell_end(int cell, int source_extent, int box_extent) {
    return std::max(cell_start(cell + 1, source_extent, box_extent),
                    cell_start(cell, source_extent, box_extent) + 1);
}

// A premultiplied pixel over the panel's backdrop, opaque in the result: a thumbnail is judged by how
// much of the layer is there, and that is only legible against something.
void blend_over_backdrop(const std::uint8_t* premult, std::uint8_t* out) {
    const std::uint32_t alpha = premult[3];
    for (int channel = 0; channel < 3; ++channel) {
        const std::uint32_t under = static_cast<std::uint32_t>(kThumbBackdrop) *
                                        (kMaskIn - alpha) +
                                    kMaskIn / 2;
        out[channel] = static_cast<std::uint8_t>(premult[channel] + under / kMaskIn);
    }
    out[3] = 255;
}

std::size_t cell(std::size_t x, std::size_t channel) { return x * 4 + channel; }

void average_over(const Image& source, Rect window, std::uint8_t* out) {
    const std::size_t width = static_cast<std::size_t>(source.size().w);
    std::uint32_t sum[4] = {0, 0, 0, 0};
    for (int y = window.y; y < window.y + window.h; ++y) {
        const std::uint8_t* row = source.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = window.x; x < window.x + window.w; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                sum[channel] += row[cell(static_cast<std::size_t>(x), static_cast<std::size_t>(channel))];
            }
        }
    }
    const std::uint32_t cells = static_cast<std::uint32_t>(window.w) *
                                static_cast<std::uint32_t>(window.h);
    std::uint8_t premult[4];
    for (int channel = 0; channel < 4; ++channel) {
        premult[channel] = static_cast<std::uint8_t>((sum[channel] + cells / 2) / cells);
    }
    blend_over_backdrop(premult, out);
}

// A coverage plane as a picture of its own: white where the mask claims the layer, the panel showing
// through where it does not. The same premultiplied form as the colour pass, so one rule serves both.
void coverage_pixel(std::uint8_t coverage, std::uint8_t* out) {
    const std::uint8_t premult[4] = {coverage, coverage, coverage, coverage};
    blend_over_backdrop(premult, out);
}

// `box` cells of `source`, averaged into `out`. The cell bounds are floor-divided, so a box larger than
// the source repeats cells rather than leaving holes - the direction that keeps a small layer's picture
// whole instead of speckled.
Image fit(const Image& source, Extent box) {
    if (source.empty() || box.w <= 0 || box.h <= 0) {
        return {};
    }
    const Extent size = source.size();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(box.w) *
                                       static_cast<std::size_t>(box.h) * 4,
                                   0);
    for (int y = 0; y < box.h; ++y) {
        const int y0 = cell_start(y, size.h, box.h);
        const int y1 = cell_end(y, size.h, box.h);
        for (int x = 0; x < box.w; ++x) {
            const int x0 = cell_start(x, size.w, box.w);
            const int x1 = cell_end(x, size.w, box.w);
            average_over(source, Rect{x0, y0, x1 - x0, y1 - y0},
                         &rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(box.w) * 4 +
                               static_cast<std::size_t>(x) * 4]);
        }
    }
    return Image::from_rgba(box, std::move(rgba));
}

Image fit_mask(const Mask& mask, Extent box) {
    if (!mask.sized() || box.w <= 0 || box.h <= 0) {
        return {};
    }
    const Extent size = mask.size();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(box.w) *
                                       static_cast<std::size_t>(box.h) * 4,
                                   0);
    for (int y = 0; y < box.h; ++y) {
        const int y0 = cell_start(y, size.h, box.h);
        const int y1 = cell_end(y, size.h, box.h);
        for (int x = 0; x < box.w; ++x) {
            const int x0 = cell_start(x, size.w, box.w);
            const int x1 = cell_end(x, size.w, box.w);
            std::uint32_t sum = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const std::span<const std::uint8_t> row = mask.row(sy);
                for (int sx = x0; sx < x1; ++sx) {
                    sum += row[static_cast<std::size_t>(sx)];
                }
            }
            const std::uint32_t cells = static_cast<std::uint32_t>(y1 - y0) *
                                        static_cast<std::uint32_t>(x1 - x0);
            coverage_pixel(static_cast<std::uint8_t>((sum + cells / 2) / cells),
                           &rgba[static_cast<std::size_t>(y) *
                                     static_cast<std::size_t>(box.w) * 4 +
                                 static_cast<std::size_t>(x) * 4]);
        }
    }
    return Image::from_rgba(box, std::move(rgba));
}

}  // namespace

Extent thumb_box(Extent size) {
    if (size.w <= 0 || size.h <= 0) {
        return Extent{0, 0};
    }
    const int long_edge = std::max(size.w, size.h);
    if (long_edge <= kLayerThumbLongEdge) {
        return size;
    }
    return Extent{std::max(1, static_cast<int>(static_cast<std::int64_t>(size.w) *
                                                   kLayerThumbLongEdge / long_edge)),
                  std::max(1, static_cast<int>(static_cast<std::int64_t>(size.h) *
                                                   kLayerThumbLongEdge / long_edge))};
}

LayerThumbs::Card& LayerThumbs::card(std::size_t index) { return cards_[index]; }

void LayerThumbs::resize(std::size_t count) {
    for (std::size_t index = count; index < cards_.size(); ++index) {
        cards_[index].pixels.discard();
        cards_[index].mask.discard();
    }
    cards_.resize(count);
}

void LayerThumbs::sync(std::size_t index, const Layer& layer, bool live) {
    if (index >= cards_.size()) {
        return;
    }
    Card& card = cards_[index];
    if (card.layer == layer.id && card.content == layer.content_revision) {
        return;
    }
    if (live) {
        // A gesture moves the counter on every dab. The picture waits for the hand to lift; a card
        // rebuilt per dab would resample the whole layer that many times.
        if (card.layer != layer.id) {
            // A card this frame is showing a different layer's picture, so it must not be drawn at all.
            card = Card{};
        }
        return;
    }
    const Extent box = thumb_box(layer.pixels.size());
    card.layer = layer.id;
    card.content = layer.content_revision;
    card.pixels.sync(fit(layer.pixels, box), layer.content_revision);
    card.has_mask = layer.mask.has_value();
    if (card.has_mask) {
        card.mask.sync(fit_mask(*layer.mask, thumb_box(layer.mask->size())),
                       layer.content_revision);
    } else {
        card.mask.discard();
    }
    ++rebuilds;
}

}  // namespace noise::app
