// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/brush.h"
#include "core/geom.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace noise {
class CommandTable;
struct AppState;

namespace app {

// What the pointer is telling the person this frame, decided from state rather than from whatever the
// OS cursor happens to be. `footprint_px` is the diameter of the ring the canvas drew, in screen
// pixels: a brush's size has to be seen where the paint lands, because the dab moves with the zoom and
// no OS cursor can follow that. `core_px` is the diameter inside the hardness falloff.
struct PointerView {
    bool over_canvas = false;
    PointerShape shape = PointerShape::arrow;
    double footprint_px = 0.0;
    double core_px = 0.0;
    bool drawn = false;
    std::optional<Vec2> at{};  // document pixels under the pointer
};

// `footprint_forced` is `pointer.footprint {show}`: nullopt follows the tool, true draws the ring
// whatever the tool is, false never draws it. A capture needs the ring to exist to measure it, and
// needs to be able to make it not exist to prove the tool alone would not have drawn it.
PointerView resolve_pointer(const AppState& state, std::optional<Vec2> over_canvas,
                            std::optional<bool> footprint_forced);

enum class PointerRequest {
    leave_alone,
    arrow,
    crosshair,
    move,
    hidden,
};

// The one statement of when the OS cursor stands down: only while the ring is on screen to replace it.
// A paint tool whose ring is switched off still gets a crosshair, because the landing point is the
// whole reason it is a paint tool.
PointerRequest pointer_request(const PointerView& view);

nlohmann::json pointer_json(const AppState& state);

}  // namespace noise::app
}  // namespace noise
