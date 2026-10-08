// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace noise {

// Pixel extent: an image size or a viewport size, distinguished by parameter name.
struct Extent {
    int w = 0;
    int h = 0;
};

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};


// Half-open pixel region [x, x+w) x [y, y+h).
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && px < right() && py >= y && py < bottom(); }
};

inline bool operator==(Extent a, Extent b) { return a.w == b.w && a.h == b.h; }
inline bool operator==(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Canvas (world) -> viewport transform: view = (world - origin) * scale.
struct ViewTransform {
    Extent image{};
    double scale = 1.0;
    Vec2 origin{};  // world coordinate at the viewport's top-left corner

    Vec2 world_to_view(Vec2 p) const;
    Vec2 view_to_world(Vec2 p) const;
    double image_width() const;
    double image_height() const;
};

struct ViewLimits {
    double min_scale = 0.0;
    double max_scale = 0.0;
};

ViewLimits scale_limits(Extent image, Extent viewport, double min_fraction, double max_scale);
double clamp_scale(double scale, ViewLimits limits);

// Zooms by factor keeping the world point currently under view_anchor pinned to that
// anchor. The anchor is in viewport coordinates. Origin clamping is a separate step:
// composing it here would silently break the anchor property near the image edges.
ViewTransform zoom_at(ViewTransform view, Vec2 view_anchor, double factor, ViewLimits limits);

// Scale that places the whole image inside the viewport with the given margin, centred.
ViewTransform fit_view(Extent image, Extent viewport, int margin_px, double max_scale);

// Clamps origin so the image cannot be panned more than overpan_px (screen pixels, hence
// scale-independent) past a viewport edge. On an axis where the image is smaller than the
// viewport that bound is unsatisfiable and centring is the only sane fallback.
ViewTransform clamp_origin(ViewTransform view, Extent viewport, int overpan_px);

Rect intersect(Rect a, Rect b);

// Intersection with [0, bounds). Empty when the rect lies wholly outside; the position of
// an empty result is not meaningful, only `empty()` is.
Rect clamp_rect(Rect r, Extent bounds);

}
