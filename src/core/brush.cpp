// SPDX-License-Identifier: Apache-2.0
#include "core/brush.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace noise {

namespace {

// Below this a dab cannot change a byte, so the pixel is left alone.
constexpr double kCoverageEpsilon = 1e-4;
// Dabs closer than a pixel would sample the same pixels repeatedly.
constexpr double kMinimumDabSpacing = 1.0;
// The edge band, in document pixels: half inside the rim, half outside. Anything past it is
// fully uncovered, and stroke_bounds leaves room for it.
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

}  // namespace

std::vector<Vec2> stroke_dabs(const std::vector<Vec2>& path, const Brush& brush) {
    std::vector<Vec2> dabs;
    if (path.empty()) {
        return dabs;
    }
    const double step = std::max(brush.spacing * brush.size, kMinimumDabSpacing);
    dabs.push_back(path.front());
    double to_next = step;
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
    return dabs;
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
    const std::vector<Vec2> dabs = stroke_dabs(path, brush);
    if (dabs.empty()) {
        return Rect{};
    }
    const double reach = brush.size * 0.5 + kRimBand;
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
    return intersect(Rect{left, top, right - left, bottom - top}, Rect{0, 0, bounds.w, bounds.h});
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
    const double radius = brush.size * 0.5;
    const double reach = radius + kRimBand;

    std::vector<double> coverage(area(touched), 0.0);
    const double dab_alpha_scale = brush.flow * (color.a / 255.0);
    for (const Vec2& dab : dabs) {
        const int x_from = std::max(touched.x, static_cast<int>(std::floor(dab.x - reach)));
        const int y_from = std::max(touched.y, static_cast<int>(std::floor(dab.y - reach)));
        const int x_to = std::min(touched.right(), static_cast<int>(std::ceil(dab.x + reach)));
        const int y_to = std::min(touched.bottom(), static_cast<int>(std::ceil(dab.y + reach)));
        for (int y = y_from; y < y_to; ++y) {
            for (int x = x_from; x < x_to; ++x) {
                const double distance = std::hypot(x + 0.5 - dab.x, y + 0.5 - dab.y);
                const double alpha = stamp_coverage(distance, brush.hardness, radius) * dab_alpha_scale;
                double& here = coverage[index(touched, x, y)];
                // Dabs composite over each other, so a stroke saturates towards opaque
                // instead of every pass adding to the last.
                here = alpha + here * (1.0 - alpha);
            }
        }
    }

    const double opacity = brush.opacity;
    for (int y = touched.y; y < touched.bottom(); ++y) {
        for (int x = touched.x; x < touched.right(); ++x) {
            const double source = coverage[index(touched, x, y)] * opacity;
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
    return touched;
}

}  // namespace noise
