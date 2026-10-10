// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/geom.h"
#include "core/image.h"
#include "core/maskops.h"

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

// What the pointer does while a tool has the button. A new tool adds a row to `kTools` and a case to
// the dispatch, and nothing anywhere compares a tool's name.
enum class ToolAction {
    paint,   // coverage into the active layer
    move,    // the active layer's pixels slide
    select,  // the marquee becomes the document's selection
};

// One tool per row: the id a request carries, what it does to the pixels, and the key
// that selects it. The command table, the pointer and the panels all read this, so a tool
// that is not here does not exist.
struct ToolInfo {
    const char* id;
    ToolAction action;
    // Set only by a row that paints. A tool that moves pixels has no paint mode, and nullopt is
    // how the row says so rather than a mode it happens to ignore.
    std::optional<PaintMode> paint;
    // Set only by a row whose action is `select`: the shape its marquee puts down. Two marquee
    // tools are two rows over this one field, so the pointer never has to ask which tool is active.
    std::optional<MaskShape> marquee;
    const char* select_key;
    const char* title;
};
extern const std::vector<ToolInfo> kTools;

// nullopt for a tool that paints nothing, which is also the answer for a name no row defines -
// callers that must tell those apart use `tool_action`, which reports the unknown name as such.
std::optional<PaintMode> tool_mode(const std::string& id);

// nullopt only for an id with no row, which is a request to refuse rather than a tool to guess.
std::optional<ToolAction> tool_action(const std::string& id);

// The shape a select tool draws, or nullopt for a tool that draws no marquee.
std::optional<MaskShape> tool_marquee(const std::string& id);

// "brush, eraser" - the tool list as an error message wants it, from the same table.
std::string tool_names();

// Only the rows that carry a paint mode. A command that paints refuses a tool that is in
// `tool_names` but not here, so a tool added for some other purpose cannot be used to paint by
// accident - which is what happened before this existed.
std::string paintable_tool_names();

// The size the brush actually takes, never the one a caller asked for past the ends of the
// slider: kMinBrushSize is the smallest stamp that still paints its own edge.
double clamp_brush_size(double size);

// One step of the size keys: multiplicative, so the same keystroke means the same thing at
// 4 px and at 400 px.
inline constexpr double kBrushSizeStep = 1.25;

// One step of the keys that nudge a 0..1 brush field (hardness, opacity): 5 % of the range, so the
// same number of presses crosses any of them.
inline constexpr double kBrushFractionStep = 0.05;

// The field after one press of such a key, clamped to `low`..`high`.
//
// The result lands on the step grid rather than on `value + step`: from a slider dragged to 0.73,
// "one step softer" means 0.70, and a value that accumulated a binary rounding error per press
// would leave the app reporting a setting nobody typed.
double step_brush_fraction(double value, int direction, double low, double high);

// A stroke's accumulated coverage over one rectangle, kept between calls so a live gesture
// composites once with the stroke's opacity - and with the same result as one stroke painted
// in a single call - instead of compounding a little more each frame.
//
// Coverage is per pixel, and a pixel's composited value depends only on its own coverage, which
// is what lets a live gesture repaint just the rectangle its new dabs reached: whatever the
// earlier dabs contributed to a pixel is already in here.
struct StrokeScratch {
    Rect rect;
    std::vector<double> coverage;

    // Takes in `want`, clipped to `bounds`, keeping the coverage already accumulated. The buffer
    // grows by `kBufferGrowthFactor` rather than to fit, because growing to fit copies every
    // accumulated pixel on every event of a drag.
    void grow_to(Rect want, Extent bounds);
};

// Adds these dabs to `scratch`, growing over `bounds` when they fall outside it, and returns the
// rectangle they touched: what a caller has to repaint, which is not the whole scratch.
Rect scratch_stamp(StrokeScratch& scratch, const std::vector<Vec2>& dabs, const Brush& brush,
                   Extent bounds);

// Composites the accumulated coverage onto `image`, limited to `limit` - the same arithmetic
// `paint_stroke` uses, which passes the rectangle the dabs reached.
void scratch_composite(Image& image, const StrokeScratch& scratch, Rgba8 color,
                       const Brush& brush, PaintMode mode, Rect limit);

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
