// SPDX-License-Identifier: Apache-2.0
#include "core/geom.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace noise {

Vec2 ViewTransform::world_to_view(Vec2 p) const {
    return {(p.x - origin.x) * scale, (p.y - origin.y) * scale};
}

Vec2 ViewTransform::view_to_world(Vec2 p) const {
    return {p.x / scale + origin.x, p.y / scale + origin.y};
}

double ViewTransform::image_width() const {
    return static_cast<double>(image.w) * scale;
}

double ViewTransform::image_height() const {
    return static_cast<double>(image.h) * scale;
}

ViewLimits scale_limits(Extent image, Extent viewport, double min_fraction, double max_scale) {
    const double fit_w = static_cast<double>(viewport.w) / static_cast<double>(image.w);
    const double fit_h = static_cast<double>(viewport.h) / static_cast<double>(image.h);
    const double min_scale = std::min(fit_w, fit_h) * min_fraction;
    return {std::min(min_scale, max_scale), max_scale};
}

double clamp_scale(double scale, ViewLimits limits) {
    return std::max(limits.min_scale, std::min(limits.max_scale, scale));
}

ViewTransform zoom_at(ViewTransform view, Vec2 view_anchor, double factor, ViewLimits limits) {
    const Vec2 world = view.view_to_world(view_anchor);
    view.scale = clamp_scale(view.scale * factor, limits);
    view.origin = {world.x - view_anchor.x / view.scale, world.y - view_anchor.y / view.scale};
    return view;
}

ViewTransform fit_view(Extent image, Extent viewport, int margin_px, double max_scale) {
    const Extent usable{viewport.w - 2 * margin_px, viewport.h - 2 * margin_px};
    const double scale = clamp_scale(
        std::min(static_cast<double>(usable.w) / static_cast<double>(image.w),
                 static_cast<double>(usable.h) / static_cast<double>(image.h)),
        ViewLimits{std::numeric_limits<double>::min(), max_scale});
    ViewTransform view;
    view.image = image;
    view.scale = scale;
    // Centred inside the usable box, which itself starts at margin_px in viewport space.
    const auto margin = static_cast<double>(margin_px);
    view.origin = {(image.w - static_cast<double>(usable.w) / scale) * 0.5 - margin / scale,
                   (image.h - static_cast<double>(usable.h) / scale) * 0.5 - margin / scale};
    return view;
}

ViewTransform clamp_origin(ViewTransform view, Extent viewport, int overpan_px) {
    const double overpan = static_cast<double>(overpan_px) / view.scale;
    const double image_w = static_cast<double>(view.image.w);
    const double image_h = static_cast<double>(view.image.h);
    const double view_w = static_cast<double>(viewport.w) / view.scale;
    const double view_h = static_cast<double>(viewport.h) / view.scale;

    const double lo_x = -overpan;
    const double hi_x = image_w - view_w + overpan;
    view.origin.x = (lo_x <= hi_x) ? std::clamp(view.origin.x, lo_x, hi_x)
                                   : (image_w - view_w) * 0.5;
    const double lo_y = -overpan;
    const double hi_y = image_h - view_h + overpan;
    view.origin.y = (lo_y <= hi_y) ? std::clamp(view.origin.y, lo_y, hi_y)
                                   : (image_h - view_h) * 0.5;
    return view;
}

Rect unite(Rect a, Rect b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const int x = a.x < b.x ? a.x : b.x;
    const int y = a.y < b.y ? a.y : b.y;
    const int right = a.right() > b.right() ? a.right() : b.right();
    const int bottom = a.bottom() > b.bottom() ? a.bottom() : b.bottom();
    return Rect{x, y, right - x, bottom - y};
}

Rect intersect(Rect a, Rect b) {
    const int x0 = std::max(a.x, b.x);
    const int y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.right(), b.right());
    const int y1 = std::min(a.bottom(), b.bottom());
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect clamp_rect(Rect r, Extent bounds) {
    return intersect(r, Rect{0, 0, bounds.w, bounds.h});
}

Rect grown_rect(Rect current, Rect want, Extent bounds, double factor) {
    const Rect legal = clamp_rect(want, bounds);
    if (legal.empty()) {
        return current;
    }
    const Rect both = unite(current, legal);
    if (both == current) {
        return current;
    }
    if (current.empty()) {
        return legal;
    }
    const double have = static_cast<double>(area(both));
    const double needed = factor * static_cast<double>(area(current));
    if (have >= needed) {
        return both;
    }
    // Expand both about its centre by the scale that reaches `needed`; expanding only right and
    // down would push the buffer off the canvas on the first growth of a stroke that started in a
    // corner, and the clamp would then hand back something no bigger than the union.
    const double scale = std::sqrt(needed / have);
    const int w = static_cast<int>(std::ceil(both.w * scale));
    const int h = static_cast<int>(std::ceil(both.h * scale));
    const Rect grown{both.x - (w - both.w) / 2, both.y - (h - both.h) / 2, w, h};
    return clamp_rect(grown, bounds);
}

}
