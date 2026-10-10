// SPDX-License-Identifier: Apache-2.0
#include "core/brush.h"

#include "core/blend.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

namespace noise {

const std::vector<ToolInfo> kTools{
    {"brush", ToolAction::paint, PaintMode::Over, std::nullopt, "B", "Paint with the brush tool"},
    {"eraser", ToolAction::paint, PaintMode::Erase, std::nullopt, "E",
     "Paint with the eraser tool"},
    {"move", ToolAction::move, std::nullopt, std::nullopt, "V",
     "Move the active layer with the pointer"},
    {"select", ToolAction::select, std::nullopt, MaskShape::rect, "M",
     "Select a rectangle with the pointer"},
    {"select_ellipse", ToolAction::select, std::nullopt, MaskShape::ellipse, "Shift+M",
     "Select an ellipse with the pointer"},
};

std::string tool_names() {
    std::string names;
    for (const ToolInfo& tool : kTools) {
        if (!names.empty()) {
            names += ", ";
        }
        names += tool.id;
    }
    return names;
}

std::string paintable_tool_names() {
    std::string names;
    for (const ToolInfo& tool : kTools) {
        if (!tool.paint) {
            continue;
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += tool.id;
    }
    return names;
}

double clamp_brush_size(double size) {
    return std::clamp(size, kMinBrushSize, kMaxBrushSize);
}

double step_brush_fraction(double value, int direction, double low, double high) {
    const double steps = std::round((value - low) / kBrushFractionStep) +
                         static_cast<double>(direction);
    return std::clamp(low + steps * kBrushFractionStep, low, high);
}

std::optional<MaskShape> tool_marquee(const std::string& id) {
    for (const ToolInfo& tool : kTools) {
        if (id == tool.id) {
            return tool.marquee;
        }
    }
    return std::nullopt;
}

std::optional<PaintMode> tool_mode(const std::string& id) {
    for (const ToolInfo& tool : kTools) {
        if (id == tool.id) {
            return tool.paint;
        }
    }
    return std::nullopt;
}

std::optional<ToolAction> tool_action(const std::string& id) {
    for (const ToolInfo& tool : kTools) {
        if (id == tool.id) {
            return tool.action;
        }
    }
    return std::nullopt;
}

namespace {

// Below this a dab cannot change a byte, so the pixel is left alone.
constexpr double kCoverageEpsilon = 1e-4;
// Dabs closer than a pixel would sample the same pixels repeatedly.
constexpr double kMinimumDabSpacing = 1.0;
// The edge band, in document pixels: half inside the rim, half outside. Anything past it is
// fully uncovered, and the bounds below leave room for it.
constexpr double kRimBand = 0.5;

std::size_t index(Rect rect, int x, int y) {
    return static_cast<std::size_t>(y - rect.y) * static_cast<std::size_t>(rect.w) +
           static_cast<std::size_t>(x - rect.x);
}

// The rectangle a set of dabs can affect, unclipped.
Rect dabs_bounds(const std::vector<Vec2>& dabs, double reach) {
    if (dabs.empty()) {
        return Rect{};
    }
    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    for (const Vec2& dab : dabs) {
        left = std::min(left, static_cast<int>(std::floor(dab.x - reach)));
        top = std::min(top, static_cast<int>(std::floor(dab.y - reach)));
        right = std::max(right, static_cast<int>(std::ceil(dab.x + reach)));
        bottom = std::max(bottom, static_cast<int>(std::ceil(dab.y + reach)));
    }
    return Rect{left, top, right - left, bottom - top};
}

}  // namespace

std::vector<Vec2> walk_dabs(const std::vector<Vec2>& path, const Brush& brush, DabWalk& walk) {
    std::vector<Vec2> dabs;
    if (path.empty()) {
        return dabs;
    }
    const double step = std::max(brush.spacing * brush.size, kMinimumDabSpacing);
    if (!walk.started) {
        dabs.push_back(path.front());
        walk.started = true;
        walk.to_next = step;
    }
    double to_next = walk.to_next;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const double dx = path[i].x - path[i - 1].x;
        const double dy = path[i].y - path[i - 1].y;
        const double segment = std::hypot(dx, dy);
        if (segment <= 0.0) {
            continue;
        }
        double travelled = 0.0;
        while (travelled + to_next <= segment) {
            travelled += to_next;
            const double t = travelled / segment;
            dabs.push_back({path[i - 1].x + dx * t, path[i - 1].y + dy * t});
            to_next = step;
        }
        to_next -= segment - travelled;
    }
    walk.to_next = to_next;
    return dabs;
}

std::vector<Vec2> stroke_dabs(const std::vector<Vec2>& path, const Brush& brush) {
    DabWalk walk{0.0, false};
    return walk_dabs(path, brush, walk);
}

double stamp_coverage(double distance_px, double hardness, double radius) {
    const double outer = radius + kRimBand;
    if (distance_px >= outer) {
        return 0.0;
    }
    const double anti_alias = std::clamp(outer - distance_px, 0.0, 1.0);
    const double solid = hardness * radius;
    if (distance_px <= solid) {
        return anti_alias;
    }
    const double span = std::max(outer - solid, 1.0);
    const double t = std::clamp((distance_px - solid) / span, 0.0, 1.0);
    return (1.0 - t * t * (3.0 - 2.0 * t)) * anti_alias;
}

Rect stroke_bounds(const std::vector<Vec2>& path, const Brush& brush, Extent bounds) {
    return intersect(dabs_bounds(stroke_dabs(path, brush), brush.size * 0.5 + kRimBand),
                     Rect{0, 0, bounds.w, bounds.h});
}

void StrokeScratch::grow_to(Rect want, Extent bounds) {
    // Union, not replacement: a later dab's bounds can easily be smaller than, or disjoint
    // from, what is already accumulated, and the coverage of the earlier dabs has to survive.
    // Growing by a factor is what makes that survival cheap - growing to fit copies every
    // accumulated pixel on every event of a drag.
    const Rect grown = grown_rect(rect, want, bounds, kBufferGrowthFactor);
    if (grown == rect) {
        return;
    }
    std::vector<double> next(area(grown), 0.0);
    // `rect` is positioned, not sized from the origin: iterating 0..w would index outside
    // both buffers unless the rect happened to start at the corner.
    const Rect keep = intersect(grown, rect);
    for (int y = keep.y; y < keep.bottom(); ++y) {
        std::copy_n(coverage.begin() + static_cast<std::ptrdiff_t>(index(rect, keep.x, y)),
                    static_cast<std::size_t>(keep.w),
                    next.begin() + static_cast<std::ptrdiff_t>(index(grown, keep.x, y)));
    }
    rect = grown;
    coverage = std::move(next);
}

Rect scratch_stamp(StrokeScratch& scratch, const std::vector<Vec2>& dabs, const Brush& brush,
                   Extent bounds) {
    if (dabs.empty()) {
        return Rect{};
    }
    const double radius = brush.size * 0.5;
    const double reach = radius + kRimBand;
    const Rect footprint = intersect(dabs_bounds(dabs, reach), Rect{0, 0, bounds.w, bounds.h});
    scratch.grow_to(footprint, bounds);
    // What the dabs reached, not what the buffer now covers: the buffer is deliberately bigger,
    // and a caller that repainted the buffer would pay for the growth on every event.
    const Rect touched = intersect(footprint, scratch.rect);
    for (const Vec2& dab : dabs) {
        const int x_from = std::max(touched.x, static_cast<int>(std::floor(dab.x - reach)));
        const int y_from = std::max(touched.y, static_cast<int>(std::floor(dab.y - reach)));
        const int x_to = std::min(touched.right(), static_cast<int>(std::ceil(dab.x + reach)));
        const int y_to = std::min(touched.bottom(), static_cast<int>(std::ceil(dab.y + reach)));
        for (int y = y_from; y < y_to; ++y) {
            for (int x = x_from; x < x_to; ++x) {
                const double distance = std::hypot(x + 0.5 - dab.x, y + 0.5 - dab.y);
                const double alpha = stamp_coverage(distance, brush.hardness, radius) * brush.flow;
                double& here = scratch.coverage[index(scratch.rect, x, y)];
                // Dabs composite over each other, so a stroke saturates towards opaque
                // instead of every pass adding to the last.
                here = alpha + here * (1.0 - alpha);
            }
        }
    }
    return touched;
}

void scratch_composite(Image& image, const StrokeScratch& scratch, Rgba8 color,
                       const Brush& brush, PaintMode mode, Rect limit) {
    const Rect touched =
        intersect(intersect(scratch.rect, limit), Rect{0, 0, image.size().w, image.size().h});
    const double opacity = brush.opacity * (color.a / 255.0);
    const std::uint8_t source_rgb[4] = {color.r, color.g, color.b, 0};
    for (int y = touched.y; y < touched.bottom(); ++y) {
        for (int x = touched.x; x < touched.right(); ++x) {
            const double source = scratch.coverage[index(scratch.rect, x, y)] * opacity;
            if (source <= kCoverageEpsilon) {
                continue;
            }
            std::uint8_t* pixel = image.at(x, y);
            if (mode == PaintMode::Erase) {
                blend_erase(pixel, source);
                continue;
            }
            blend_over(pixel, source_rgb, source);
        }
    }
}

std::optional<Rect> paint_stroke(Image& image,
                                 const std::vector<Vec2>& path,
                                 const Brush& brush,
                                 Rgba8 color,
                                 PaintMode mode) {
    const std::vector<Vec2> dabs = stroke_dabs(path, brush);
    const Rect touched = stroke_bounds(path, brush, image.size());
    if (image.empty() || dabs.empty() || touched.empty()) {
        return std::nullopt;
    }
    StrokeScratch scratch;
    const Rect stamped = scratch_stamp(scratch, dabs, brush, image.size());
    scratch_composite(image, scratch, color, brush, mode, stamped);
    return touched;
}

}  // namespace noise
