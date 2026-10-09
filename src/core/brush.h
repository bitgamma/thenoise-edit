// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/geom.h"
#include "core/image.h"

namespace noise {

// Brush geometry in document pixels. `spacing` is a fraction of the diameter, the same unit
// the UI slider uses; `opacity` belongs to the whole stroke and `flow` to each dab, so a
// slower stroke does not become darker than a fast one by accident.
struct Brush {
    double size;      // diameter, at least 1
    double hardness;  // 0..1, fraction of the radius painted at full strength
    double opacity;   // 0..1
    double flow;      // 0..1
    double spacing;   // 0..1 of the diameter between dab centres
};

// The brush a fresh session paints with. Adding a brush means adding a row here, not a
// branch in the caller.
inline constexpr Brush kBrushDefaults{24.0, 0.8, 1.0, 1.0, 0.25};
inline constexpr double kMinBrushSize = 1.0;
inline constexpr double kMaxBrushSize = 4096.0;
inline constexpr const char* kDefaultTool = "brush";

struct Rgba8 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

enum class PaintMode {
    Over,   // src-over
    Erase,  // src-out: takes alpha away, leaves the colour alone
};

// Dab centres: the first point, then one every `spacing * size` along the path.
std::vector<Vec2> stroke_dabs(const std::vector<Vec2>& path, const Brush& brush);

// Distance is in document pixels from the dab centre, radius is the dab radius. Full
// strength out to `hardness` of the radius, a smoothstep to the rim, and a half-pixel band
// straddling the rim so a brush of any size paints its own edge rather than nothing.
double stamp_coverage(double distance_px, double hardness, double radius);

// The rectangle `paint_stroke` will touch inside `bounds`, empty if it misses. Kept apart
// so the document can capture the pixels a stroke is about to change.
Rect stroke_bounds(const std::vector<Vec2>& path, const Brush& brush, Extent bounds);

// Stamps the path and returns the rectangle it touched, or nullopt if it missed. Dabs
// accumulate into a coverage buffer over the stroke's own bounds and are composited once,
// which is what keeps opacity per stroke and flow per dab.
std::optional<Rect> paint_stroke(Image& image,
                                 const std::vector<Vec2>& path,
                                 const Brush& brush,
                                 Rgba8 color,
                                 PaintMode mode);

}  // namespace noise
