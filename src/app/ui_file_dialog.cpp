// SPDX-License-Identifier: Apache-2.0
#include "app/ui_file_dialog.h"

#include "app/file_commands.h"
#include "app/state.h"
#include "platform/file_dialog.h"

#include <utility>

namespace noise::app {

void draw_native_file_dialog(AppState& state) {
    if (auto request = take_native_file_pick(state)) {
        platform::PickResult result = platform::choose_native_file(*request);
        finish_native_file_pick(state, std::move(result));
    }
}

void shutdown_native_file_dialog() {
    platform::shutdown_native_file_dialog();
}

}  // namespace noise::app
