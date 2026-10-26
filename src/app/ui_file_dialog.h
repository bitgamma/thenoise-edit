// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace noise {
struct AppState;
}

namespace noise::app {
// Consumes pending file.pick requests on the host thread and publishes their native-dialog answers.
void draw_native_file_dialog(AppState& state);
void shutdown_native_file_dialog();
}  // namespace noise::app
