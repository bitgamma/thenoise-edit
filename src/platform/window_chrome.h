// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace noise::platform {

bool enable_client_titlebar(const void* native_display, const void* native_window);
void move_client_window();
void minimize_client_window();
void toggle_client_window_maximized();
int client_edge_resize_direction(float x, float y, int width, int height);
bool begin_client_edge_resize(float x, float y, int width, int height);

}
