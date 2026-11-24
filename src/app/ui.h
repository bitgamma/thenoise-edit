// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/coverage_tint.h"
#include "app/layer_thumbs.h"
#include "app/reference_thumbs.h"
#include "app/state.h"
#include "core/command.h"
#include "gfx/checker.h"
#include "gfx/renderer.h"
#include <string_view>

namespace noise::app {

// simgui stages a whole frame of UI geometry into a fixed vertex buffer and drops the
// frame's draw commands when it overflows. Canvas chrome measured ~600 vertices on this
// box; the rest of this capacity is text, whose cost is (visible rows x longest line)
// because ImGui culls whole lines outside a panel. `app.state` reports it beside the
// frame's real demand, and the golden driver fails well before it.
inline constexpr int kUiVertexLimit = 131072;


// The UI owns no behaviour of its own: widgets dispatch through `commands`, the
// same table the debug wire drives. The canvas texture is GPU state the caller
// owns, so that `AppState` stays windowless and testable.
void register_ui_commands(AppState& state, CommandTable& commands);
void register_ai_workspace_settings(bool& open, bool& placement_initialized);
void register_layer_panel_settings(double& upper_ratio);
void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
             const gfx::Checker& checker, CoverageTint& tint, CoverageTint& mask_tint,
             LayerThumbs& thumbs,
             ReferenceThumbs& reference_thumbs, const gfx::Texture& brand_logo,
             bool client_titlebar, std::string_view document_name);

}
