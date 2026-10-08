// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/command.h"
#include "gfx/renderer.h"

namespace noise::app {

// The UI owns no behaviour of its own: widgets dispatch through `commands`, the
// same table the debug wire drives. The canvas texture is GPU state the caller
// owns, so that `AppState` stays windowless and testable.
void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture);

}
