// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/command.h"
#include "dbg/wire.h"

#include <functional>

namespace noise::app {

// What only a running application can do: read its framebuffer and end its main
// loop. Injecting them keeps the command surface identical with and without a GPU
// attached, and makes a clean shutdown drivable from the wire.
struct HostHooks {
    std::function<Image(int width, int height)> capture;
    std::function<void()> quit;
};

void register_commands(AppState& state, CommandTable& table, dbg::DebugWire& wire,
                       const HostHooks& hooks);

}
