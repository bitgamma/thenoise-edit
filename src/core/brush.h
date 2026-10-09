// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

// What a fresh session paints with, until the user says otherwise.
inline constexpr Rgba8 kDefaultColor{0, 0, 0, 255};

enum class PaintMode {
    Over,   // src-over
    Erase,  // src-out: takes alpha away, leaves the colour alone
};

// One tool per row: the id a request carries, what it does to the pixels, and the bare key
// that selects it. The command table, the pointer and the panels all read this, so a tool
// that is not here does not exist.
struct ToolInfo {
    const char* id;
    PaintMode mode;
    const char* select_key;
};
extern const std::vector<ToolInfo> kTools;

std::optional<PaintMode> tool_mode(const std::string& id);

// "brush, eraser" - the tool list as an error message wants it, from the same table.
std::string tool_names();

// The size the brush actually takes, never the one a caller asked for past the ends of the
// slider: kMinBrushSize is the smallest stamp that still paints its own edge.
double clamp_brush_size(double size);

// One step of the size keys: multiplicative, so the same keystroke means the same thing at
// 4 px and at 400 px.
inline constexpr double kBrushSizeStep = 1.25;

// A stroke's accumulated coverage over one rectangle, kept between calls so a live gesture
// composites once with the stroke's opacity - and with the same result as one stroke painted
// in a single call - instead of compounding a little more each frame.
struct StrokeScratch {
    Rect rect;
    std::vector<double> coverage;

    // Takes in `want`, clipped to `bounds`, keeping the coverage already accumulated.
    void grow_to(Rect want, Extent bounds);
};

// Adds these dabs to `scratch`, growing it over `bounds` when they fall outside it.
void scratch_stamp(StrokeScratch& scratch, const std::vector<Vec2>& dabs, const Brush& brush,
                   Extent bounds);

// Composites the accumulated coverage onto `image`: the same arithmetic `paint_stroke` uses.
void scratch_composite(Image& image, const StrokeScratch& scratch, Rgba8 color,
                       const Brush& brush, PaintMode mode);

// The distance still owed before the next dab, so a gesture fed in point by point emits the
// same dabs as the same polyline handed over all at once.
struct DabWalk {
    double to_next;
    bool started;
};

// Dab centres along `path`, continuing `walk`: the first point, then one every
// `spacing * size`. Nothing is emitted for a repeat of the point the walk ended on.
std::vector<Vec2> walk_dabs(const std::vector<Vec2>& path, const Brush& brush, DabWalk& walk);

// The whole path in one call, from the start of its walk.
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
