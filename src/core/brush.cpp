// SPDX-License-Identifier: Apache-2.0
#include "core/brush.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

namespace noise {

const std::vector<ToolInfo> kTools{
    {"brush", PaintMode::Over, "B"},
    {"eraser", PaintMode::Erase, "E"},
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

double clamp_brush_size(double size) {
    return std::clamp(size, kMinBrushSize, kMaxBrushSize);
}

std::optional<PaintMode> tool_mode(const std::string& id) {
    for (const ToolInfo& tool : kTools) {
        if (id == tool.id) {
            return tool.mode;
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

std::size_t area(Rect rect) {
    return static_cast<std::size_t>(rect.w) * static_cast<std::size_t>(rect.h);
}

std::size_t index(Rect rect, int x, int y) {
    return static_cast<std::size_t>(y - rect.y) * static_cast<std::size_t>(rect.w) +
           static_cast<std::size_t>(x - rect.x);
}

std::uint8_t channel_from_unit(double unit) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
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
    const Rect grown = intersect(unite(rect, want), Rect{0, 0, bounds.w, bounds.h});
    if (grown == rect) {
        return;
    }
    std::vector<double> next(area(grown), 0.0);
    // `rect` is positioned, not sized from the origin: iterating 0..w would index outside
    // both buffers unless the rect happened to start at the corner.
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (grown.contains(x, y)) {
                next[index(grown, x, y)] = coverage[index(rect, x, y)];
            }
        }
    }
    rect = grown;
    coverage = std::move(next);
}

void scratch_stamp(StrokeScratch& scratch, const std::vector<Vec2>& dabs, const Brush& brush,
                   Extent bounds) {
    if (dabs.empty()) {
        return;
    }
    const double radius = brush.size * 0.5;
    const double reach = radius + kRimBand;
    scratch.grow_to(dabs_bounds(dabs, reach), bounds);
    for (const Vec2& dab : dabs) {
        const int x_from = std::max(scratch.rect.x, static_cast<int>(std::floor(dab.x - reach)));
        const int y_from = std::max(scratch.rect.y, static_cast<int>(std::floor(dab.y - reach)));
        const int x_to = std::min(scratch.rect.right(), static_cast<int>(std::ceil(dab.x + reach)));
        const int y_to = std::min(scratch.rect.bottom(), static_cast<int>(std::ceil(dab.y + reach)));
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
}

void scratch_composite(Image& image, const StrokeScratch& scratch, Rgba8 color,
                       const Brush& brush, PaintMode mode) {
    const Rect touched = intersect(scratch.rect, Rect{0, 0, image.size().w, image.size().h});
    const double opacity = brush.opacity * (color.a / 255.0);
    for (int y = touched.y; y < touched.bottom(); ++y) {
        for (int x = touched.x; x < touched.right(); ++x) {
            const double source = scratch.coverage[index(scratch.rect, x, y)] * opacity;
            if (source <= kCoverageEpsilon) {
                continue;
            }
            std::uint8_t* pixel = image.at(x, y);
            const double dest_alpha = pixel[3] / 255.0;
            if (mode == PaintMode::Erase) {
                pixel[3] = channel_from_unit(dest_alpha * (1.0 - source));
                if (pixel[3] == 0) {
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 0;
                }
                continue;
            }
            const double out_alpha = source + dest_alpha * (1.0 - source);
            const double kept = 1.0 - source;
            pixel[0] = channel_from_unit(((color.r / 255.0) * source +
                                          (pixel[0] / 255.0) * dest_alpha * kept) / out_alpha);
            pixel[1] = channel_from_unit(((color.g / 255.0) * source +
                                          (pixel[1] / 255.0) * dest_alpha * kept) / out_alpha);
            pixel[2] = channel_from_unit(((color.b / 255.0) * source +
                                          (pixel[2] / 255.0) * dest_alpha * kept) / out_alpha);
            pixel[3] = channel_from_unit(out_alpha);
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
    scratch_stamp(scratch, dabs, brush, image.size());
    scratch_composite(image, scratch, color, brush, mode);
    return touched;
}

}  // namespace noise
