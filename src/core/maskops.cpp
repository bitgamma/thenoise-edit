// SPDX-License-Identifier: Apache-2.0
#include "core/maskops.h"
#include "core/blur.h"

#include <deque>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace noise {
namespace {

// Sub-samples across a pixel the edge of a shape crosses. The ellipse's vertical extent at a given x
// is analytic, so one axis is integrated exactly and only the other is sampled; sixteen columns was
// picked by measuring the worst case, a circle one pixel across, where the estimate lands 0.5 % above
// the pi/4 of coverage the pixel really holds. A two-dimensional grid needed sixty-four samples per
// axis to do as well, on every boundary pixel.
constexpr int kStrips = 16;
constexpr int kLassoSubscanlines = 16;
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

struct LassoEdge {
    Vec2 from;
    Vec2 to;
    int first_scan = 0;
    int past_scan = 0;
    std::size_t order = 0;
};

struct Crossing {
    double x = 0.0;
    std::size_t edge = 0;
};

int scan_index(double y, int top, int count) {
    const double position = (y - static_cast<double>(top)) * kLassoSubscanlines - 0.5;
    if (position <= 0.0) {
        return 0;
    }
    if (position >= static_cast<double>(count)) {
        return count;
    }
    return static_cast<int>(std::ceil(position));
}

double edge_x(const LassoEdge& edge, double sample_y) {
    const double scale = std::max({std::abs(edge.from.y), std::abs(edge.to.y),
                                   std::abs(sample_y)});
    const double from = edge.from.y / scale;
    const double to = edge.to.y / scale;
    const double sample = sample_y / scale;
    const double fraction = std::clamp((sample - from) / (to - from), 0.0, 1.0);
    return std::lerp(edge.from.x, edge.to.x, fraction);
}

void accumulate_span(double from, double to, double sample_height, Rect area,
                     std::vector<double>& edge_coverage,
                     std::vector<double>& full_coverage_delta) {
    from = std::max(from, static_cast<double>(area.x));
    to = std::min(to, static_cast<double>(area.right()));
    if (to <= from) {
        return;
    }

    const int first = static_cast<int>(std::floor(from));
    const int past = static_cast<int>(std::ceil(to));
    const int first_local = first - area.x;
    const int past_local = past - area.x;
    if (past_local <= first_local + 1) {
        edge_coverage[static_cast<std::size_t>(first_local)] += (to - from) * sample_height;
        return;
    }

    edge_coverage[static_cast<std::size_t>(first_local)] +=
        (static_cast<double>(first + 1) - from) * sample_height;
    edge_coverage[static_cast<std::size_t>(past_local - 1)] +=
        (to - static_cast<double>(past - 1)) * sample_height;
    const int full_from = first_local + 1;
    const int full_to = past_local - 1;
    if (full_to > full_from) {
        full_coverage_delta[static_cast<std::size_t>(full_from)] += sample_height;
        full_coverage_delta[static_cast<std::size_t>(full_to)] -= sample_height;
    }
}

std::optional<MaskPatch> make_lasso_patch(Extent canvas, const std::vector<Vec2>& path) {
    if (canvas.w <= 0 || canvas.h <= 0 || path.size() < 3u) {
        return std::nullopt;
    }

    std::vector<Vec2> vertices;
    vertices.reserve(path.size());
    for (const Vec2 point : path) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return std::nullopt;
        }
        if (vertices.empty() || point.x != vertices.back().x || point.y != vertices.back().y) {
            vertices.push_back(point);
        }
    }
    if (vertices.size() > 1u && vertices.front().x == vertices.back().x &&
        vertices.front().y == vertices.back().y) {
        vertices.pop_back();
    }
    if (vertices.size() < 3u) {
        return std::nullopt;
    }
    const Vec2 first = vertices.front();
    std::optional<Vec2> second;
    bool has_third = false;
    for (const Vec2 point : vertices) {
        if (point.x == first.x && point.y == first.y) {
            continue;
        }
        if (!second) {
            second = point;
        } else if (point.x != second->x || point.y != second->y) {
            has_third = true;
            break;
        }
    }
    if (!has_third) {
        return std::nullopt;
    }

    double min_x = vertices.front().x;
    double min_y = vertices.front().y;
    double max_x = min_x;
    double max_y = min_y;
    for (const Vec2 point : vertices) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
    const double left = std::clamp(std::floor(min_x), 0.0, static_cast<double>(canvas.w));
    const double top = std::clamp(std::floor(min_y), 0.0, static_cast<double>(canvas.h));
    const double right = std::clamp(std::ceil(max_x), 0.0, static_cast<double>(canvas.w));
    const double bottom = std::clamp(std::ceil(max_y), 0.0, static_cast<double>(canvas.h));
    const Rect area{static_cast<int>(left), static_cast<int>(top),
                    static_cast<int>(right - left), static_cast<int>(bottom - top)};
    if (area.empty()) {
        return std::nullopt;
    }

    const int scan_count = area.h * kLassoSubscanlines;
    std::vector<LassoEdge> edges;
    edges.reserve(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        Vec2 from = vertices[i];
        Vec2 to = vertices[(i + 1u) % vertices.size()];
        if (from.y == to.y) {
            continue;
        }
        if (from.y > to.y) {
            std::swap(from, to);
        }
        const int first_scan = scan_index(from.y, area.y, scan_count);
        const int past_scan = scan_index(to.y, area.y, scan_count);
        if (past_scan > first_scan) {
            edges.push_back(LassoEdge{from, to, first_scan, past_scan, i});
        }
    }
    if (edges.empty()) {
        return std::nullopt;
    }
    std::stable_sort(edges.begin(), edges.end(), [](const LassoEdge& a, const LassoEdge& b) {
        return a.first_scan < b.first_scan;
    });

    MaskPatch patch{area, std::vector<std::uint8_t>(noise::area(area), kMaskOut)};
    const std::size_t width = static_cast<std::size_t>(area.w);
    std::vector<std::size_t> active;
    std::vector<Crossing> crossings;
    std::vector<double> edge_coverage(width, 0.0);
    std::vector<double> full_coverage_delta(width + 1u, 0.0);
    std::size_t next_edge = 0;
    bool has_coverage = false;
    for (int scan = 0; scan < scan_count; ++scan) {
        if (scan % kLassoSubscanlines == 0) {
            std::fill(edge_coverage.begin(), edge_coverage.end(), 0.0);
            std::fill(full_coverage_delta.begin(), full_coverage_delta.end(), 0.0);
        }
        while (next_edge < edges.size() && edges[next_edge].first_scan <= scan) {
            active.push_back(next_edge++);
        }
        std::erase_if(active, [&edges, scan](std::size_t edge) {
            return edges[edge].past_scan <= scan;
        });

        const double sample_y = static_cast<double>(area.y) +
                                (static_cast<double>(scan % kLassoSubscanlines) + 0.5) /
                                    kLassoSubscanlines +
                                static_cast<double>(scan / kLassoSubscanlines);
        crossings.clear();
        crossings.reserve(active.size());
        for (const std::size_t edge : active) {
            crossings.push_back(Crossing{edge_x(edges[edge], sample_y), edge});
        }
        std::sort(crossings.begin(), crossings.end(), [&edges](const Crossing& a,
                                                              const Crossing& b) {
            return a.x != b.x ? a.x < b.x : edges[a.edge].order < edges[b.edge].order;
        });
        for (std::size_t i = 1; i < crossings.size(); i += 2u) {
            accumulate_span(crossings[i - 1u].x, crossings[i].x,
                            1.0 / kLassoSubscanlines, area, edge_coverage,
                            full_coverage_delta);
        }

        if (scan % kLassoSubscanlines != kLassoSubscanlines - 1) {
            continue;
        }
        double full_coverage = 0.0;
        const std::size_t row = static_cast<std::size_t>(scan / kLassoSubscanlines) * width;
        for (std::size_t x = 0; x < width; ++x) {
            full_coverage += full_coverage_delta[x];
            const double coverage = std::clamp(edge_coverage[x] + full_coverage, 0.0, 1.0);
            const std::uint8_t value = static_cast<std::uint8_t>(
                std::lround(coverage * static_cast<double>(kMaskIn)));
            patch.coverage[row + x] = value;
            has_coverage = has_coverage || value != kMaskOut;
        }
    }
    return has_coverage ? std::optional<MaskPatch>(std::move(patch)) : std::nullopt;
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

std::optional<MaskPatch> rasterize_lasso(Extent canvas, const std::vector<Vec2>& path) {
    return make_lasso_patch(canvas, path);
}

bool op_lasso(Mask& mask, const std::vector<Vec2>& path, MaskOp op) {
    std::optional<MaskPatch> patch = rasterize_lasso(mask.size(), path);
    return patch.has_value() && mask.op(patch->area, patch->coverage, op);
}


namespace {

std::vector<std::uint8_t> extreme_pass(const std::vector<std::uint8_t>& input, Extent size,
                                       int radius, bool grow, bool horizontal) {
    std::vector<std::uint8_t> output(input.size());
    const int lines = horizontal ? size.h : size.w;
    const int length = horizontal ? size.w : size.h;
    const int stride = horizontal ? 1 : size.w;
    const int line_stride = horizontal ? size.w : 1;
    for (int line = 0; line < lines; ++line) {
        const std::size_t start = static_cast<std::size_t>(line) *
                                  static_cast<std::size_t>(line_stride);
        std::deque<int> window;
        int added = 0;
        for (int position = 0; position < length; ++position) {
            const int right = std::min(length - 1, position + radius);
            while (added <= right) {
                const auto sample = [&](int at) {
                    return input[start + static_cast<std::size_t>(at) *
                                         static_cast<std::size_t>(stride)];
                };
                while (!window.empty() &&
                       (grow ? sample(window.back()) <= sample(added)
                             : sample(window.back()) >= sample(added))) {
                    window.pop_back();
                }
                window.push_back(added++);
            }
            while (window.front() < position - radius) {
                window.pop_front();
            }
            output[start + static_cast<std::size_t>(position) *
                           static_cast<std::size_t>(stride)] =
                input[start + static_cast<std::size_t>(window.front()) *
                               static_cast<std::size_t>(stride)];
        }
    }
    return output;
}

// The hard-edged slider produces only 0 or 255. For this case a running
// count answers the same min/max window question without allocating and
// maintaining thousands of deques for every preview frame.
std::vector<std::uint8_t> binary_extreme_pass(const std::vector<std::uint8_t>& input,
                                              Extent size, int radius, bool grow,
                                              bool horizontal) {
    std::vector<std::uint8_t> output(input.size());
    const int lines = horizontal ? size.h : size.w;
    const int length = horizontal ? size.w : size.h;
    const int stride = horizontal ? 1 : size.w;
    const int line_stride = horizontal ? size.w : 1;
    std::vector<int> prefix(static_cast<std::size_t>(length) + 1u);
    for (int line = 0; line < lines; ++line) {
        const std::size_t start = static_cast<std::size_t>(line) *
                                  static_cast<std::size_t>(line_stride);
        prefix[0] = 0;
        for (int i = 0; i < length; ++i) {
            const std::size_t index = start + static_cast<std::size_t>(i) *
                                             static_cast<std::size_t>(stride);
            prefix[static_cast<std::size_t>(i + 1)] =
                prefix[static_cast<std::size_t>(i)] + (input[index] == kMaskIn ? 1 : 0);
        }
        for (int i = 0; i < length; ++i) {
            const int left = std::max(0, i - radius);
            const int right = std::min(length - 1, i + radius);
            const int hits = prefix[static_cast<std::size_t>(right + 1)] -
                             prefix[static_cast<std::size_t>(left)];
            const bool inside = grow ? hits != 0 : hits == right - left + 1;
            const std::size_t index = start + static_cast<std::size_t>(i) *
                                             static_cast<std::size_t>(stride);
            output[index] = inside ? kMaskIn : kMaskOut;
        }
    }
    return output;
}

}  // namespace

Mask adjusted_edge(const Mask& source, int shift_px, bool hard, int feather_px) {
    if (!source.sized()) {
        return source;
    }
    const Rect whole{0, 0, source.size().w, source.size().h};
    if (source.covers_all(whole) || source.covers_nothing(whole)) {
        return source;
    }
    std::vector<std::uint8_t> plane = source.plane();
    if (hard) {
        for (std::uint8_t& sample : plane) {
            sample = sample >= 128 ? kMaskIn : kMaskOut;
        }
    }
    if (shift_px != 0) {
        const bool grow = shift_px > 0;
        const int radius = std::abs(shift_px);
        if (hard) {
            plane = binary_extreme_pass(plane, source.size(), radius, grow, true);
            plane = binary_extreme_pass(plane, source.size(), radius, grow, false);
        } else {
            plane = extreme_pass(plane, source.size(), radius, grow, true);
            plane = extreme_pass(plane, source.size(), radius, grow, false);
        }
    }
    Mask adjusted = *Mask::from_plane(source.size(), std::move(plane));
    if (feather_px > 0) {
        adjusted = feathered_mask(adjusted, feather_px);
    }
    return adjusted;
}

Rect adjusted_edge_bounds(const Mask& source, int shift_px, int feather_px) {
    if (!source.sized()) {
        return {};
    }
    const Rect bounds = source.bounds();
    if (bounds.empty()) {
        return {};
    }
    // A box blur has kGaussianBoxPasses passes of radius feather_px. This
    // conservative halo also leaves a zero row at an internal crop boundary,
    // so replicated edge samples cannot change the cropped computation.
    const std::int64_t halo = static_cast<std::int64_t>(std::abs(shift_px)) +
                              static_cast<std::int64_t>(kGaussianBoxPasses) * feather_px + 1;
    const std::int64_t left = std::max<std::int64_t>(0, static_cast<std::int64_t>(bounds.x) - halo);
    const std::int64_t top = std::max<std::int64_t>(0, static_cast<std::int64_t>(bounds.y) - halo);
    const std::int64_t right = std::min<std::int64_t>(source.size().w,
                                                      static_cast<std::int64_t>(bounds.right()) + halo);
    const std::int64_t bottom = std::min<std::int64_t>(source.size().h,
                                                       static_cast<std::int64_t>(bounds.bottom()) + halo);
    return Rect{static_cast<int>(left), static_cast<int>(top),
                static_cast<int>(right - left), static_cast<int>(bottom - top)};
}

Mask adjusted_edge_region(const Mask& source, int shift_px, bool hard, int feather_px,
                          Rect area) {
    if (!source.sized() || area.empty() || !area.inside(source.size())) {
        return {};
    }
    Mask result(Extent{area.w, area.h});
    const Rect whole{0, 0, source.size().w, source.size().h};
    if (source.covers_all(whole)) {
        return source.region(area);
    }
    if (source.covers_nothing(whole)) {
        return result;
    }
    const Rect support = adjusted_edge_bounds(source, shift_px, feather_px);
    const Rect overlap = intersect(area, support);
    if (overlap.empty()) {
        return result;
    }
    if (shift_px == 0 && !hard && feather_px == 0) {
        return source.region(area);
    }
    const Mask cropped = source.region(support);
    const Mask calculated = adjusted_edge(cropped, shift_px, hard, feather_px);
    for (int y = overlap.y; y < overlap.bottom(); ++y) {
        const auto source_row = calculated.row(y - support.y);
        auto destination_row = result.row(y - area.y);
        std::copy_n(source_row.begin() + (overlap.x - support.x),
                    static_cast<std::size_t>(overlap.w),
                    destination_row.begin() + (overlap.x - area.x));
    }
    return result;
}

}  // namespace noise
