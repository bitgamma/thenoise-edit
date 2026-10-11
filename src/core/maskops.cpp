// SPDX-License-Identifier: Apache-2.0
#include "core/maskops.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace noise {
namespace {

// Sub-samples across a pixel the edge of a shape crosses. The ellipse's vertical extent at a given x
// is analytic, so one axis is integrated exactly and only the other is sampled; sixteen columns was
// picked by measuring the worst case, a circle one pixel across, where the estimate lands 0.5 % above
// the pi/4 of coverage the pixel really holds. A two-dimensional grid needed sixty-four samples per
// axis to do as well, on every boundary pixel.
constexpr int kStrips = 16;
// The distance from a point to an interval, zero when it lies inside it.
double to_interval(double point, double from, double to) {
    return std::max({from - point, 0.0, point - to});
}

// The ellipse inscribed in a box: centre at the box centre, semi-axes half of it. `claim` is the
// unit-circle test of a document point - at most 1 inside, above 1 out - and being separable in the
// axes and monotone in the distance from the centre, it settles whether a whole pixel is in or out
// without sampling it: the farthest corner decides inside, the closest point decides outside.
struct InscribedEllipse {
    double cx = 0.0;
    double cy = 0.0;
    double inverse_a = 0.0;
    double inverse_b = 0.0;

    double claim(double x, double y) const {
        const double u = (x - cx) * inverse_a;
        const double v = (y - cy) * inverse_b;
        return u * u + v * v;
    }

    static InscribedEllipse centre(double x, double y, double a, double b) {
        return InscribedEllipse{x, y, 1.0 / a, 1.0 / b};
    }

    double far_claim(int x, int y) const {
        const double u = std::max(std::abs(static_cast<double>(x) - cx),
                                  std::abs(static_cast<double>(x) + 1.0 - cx)) *
                         inverse_a;
        const double v = std::max(std::abs(static_cast<double>(y) - cy),
                                  std::abs(static_cast<double>(y) + 1.0 - cy)) *
                         inverse_b;
        return u * u + v * v;
    }

    double near_claim(int x, int y) const {
        const double u = to_interval(cx, static_cast<double>(x), static_cast<double>(x) + 1.0) *
                         inverse_a;
        const double v = to_interval(cy, static_cast<double>(y), static_cast<double>(y) + 1.0) *
                         inverse_b;
        return u * u + v * v;
    }
};

std::uint8_t ellipse_pixel_coverage(const InscribedEllipse& ellipse, int x, int y) {
    if (ellipse.far_claim(x, y) <= 1.0) {
        return kMaskIn;
    }
    if (ellipse.near_claim(x, y) > 1.0) {
        return kMaskOut;
    }
    const double left = static_cast<double>(x);
    const double top = static_cast<double>(y);
    const double bottom = top + 1.0;
    const double semi_y = 1.0 / ellipse.inverse_b;
    double covered = 0.0;
    for (int strip = 0; strip < kStrips; ++strip) {
        const double sample_x = left + (strip + 0.5) / kStrips;
        const double v = (sample_x - ellipse.cx) * ellipse.inverse_a;
        if (std::abs(v) >= 1.0) {
            continue;
        }
        const double half = semi_y * std::sqrt(1.0 - v * v);
        const double from = std::max(top, ellipse.cy - half);
        const double to = std::min(bottom, ellipse.cy + half);
        covered += std::max(0.0, to - from);
    }
    return static_cast<std::uint8_t>(
        std::lround(covered * kMaskIn / static_cast<double>(kStrips)));
}

// The half-width of the ellipse in x at `distance` from its centre in y, or nullopt where the
// centre's distance has already left the shape.
std::optional<double> half_width_x(const InscribedEllipse& ellipse, double distance) {
    const double v = distance * ellipse.inverse_b;
    if (v >= 1.0) {
        return std::nullopt;
    }
    return std::sqrt(1.0 - v * v) / ellipse.inverse_a;
}

// One pixel row of the ellipse, written into `row`, which covers document columns [from, to). Both
// whole-pixel tests are separable, so a row answers two questions in closed form - which pixels it
// holds entirely, and how far any coverage reaches - and only the band between them costs a per-pixel
// test. Without this, a shape the size of the canvas charges for every pixel inside it: measured
// 79 ms for a 2048^2 ellipse against 8.6 ms for the same rectangle.
void ellipse_row(const InscribedEllipse& ellipse, int y, int from, int to, std::uint8_t* row) {
    const double top = static_cast<double>(y);
    const double bottom = top + 1.0;

    // A pixel is held wholly when its farthest corner is in: farthest in x, which is half a pixel
    // out from its centre, and farthest in y, which is this row's edge away from the centre.
    const double far_y = std::max(std::abs(top - ellipse.cy), std::abs(bottom - ellipse.cy));
    int whole_from = from;
    int whole_to = from;
    if (const std::optional<double> half = half_width_x(ellipse, far_y)) {
        whole_from = std::max(from, static_cast<int>(std::ceil(ellipse.cx - *half)));
        whole_to = std::min(to, static_cast<int>(std::floor(ellipse.cx + *half - 1.0)) + 1);
        if (whole_to > whole_from) {
            std::fill(row + (whole_from - from), row + (whole_to - from), kMaskIn);
        }
    }

    // And a pixel holds nothing once the closest point of it to the centre is out, so the band that
    // needs testing is that wide and no wider.
    const double near_y = to_interval(ellipse.cy, top, bottom);
    const std::optional<double> reach = half_width_x(ellipse, near_y);
    if (!reach) {
        return;
    }
    const int band_from = std::max(from, static_cast<int>(std::ceil(ellipse.cx - *reach - 1.0)));
    const int band_to = std::min(to, static_cast<int>(std::floor(ellipse.cx + *reach)) + 2);
    for (int x = band_from; x < band_to; ++x) {
        if (x >= whole_from && x < whole_to) {
            continue;
        }
        row[x - from] = ellipse_pixel_coverage(ellipse, x, y);
    }
}

std::vector<std::uint8_t> ellipse_patch(Rect box, Rect area) {
    const InscribedEllipse ellipse = InscribedEllipse::centre(
        box.x + box.w / 2.0, box.y + box.h / 2.0, box.w / 2.0, box.h / 2.0);
    std::vector<std::uint8_t> patch(static_cast<std::size_t>(area.w) *
                                        static_cast<std::size_t>(area.h),
                                    kMaskOut);
    for (int y = area.y; y < area.bottom(); ++y) {
        std::uint8_t* row =
            patch.data() + static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.w);
        ellipse_row(ellipse, y, area.x, area.right(), row);
    }
    return patch;
}

}  // namespace

std::optional<MaskShape> mask_shape_from_name(const std::string& name) {
    for (const MaskShapeInfo& info : kMaskShapes) {
        if (name == info.name) {
            return info.shape;
        }
    }
    return std::nullopt;
}

const char* mask_shape_name(MaskShape shape) {
    for (const MaskShapeInfo& info : kMaskShapes) {
        if (info.shape == shape) {
            return info.name;
        }
    }
    return "?";
}

std::string mask_shape_names() {
    std::string names;
    for (const MaskShapeInfo& info : kMaskShapes) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.name;
    }
    return names;
}

void op_shape(Mask& mask, MaskShape shape, Rect box, MaskOp op) {
    if (box.w <= 0 || box.h <= 0) {
        return;  // nothing to write, so nothing derived from the plane went stale
    }
    const Rect area = intersect(box, Rect{0, 0, mask.size().w, mask.size().h});
    if (area.empty()) {
        return;
    }
    if (shape == MaskShape::rect) {
        // The patch of a rectangle is full coverage, which is exactly what `Mask::op(rect, op)`
        // applies, so the shape path and the rectangle path are the same code rather than the same
        // arithmetic written twice.
        mask.op(area, op);
        return;
    }
    mask.op(area, ellipse_patch(box, area), op);
}


}  // namespace noise
