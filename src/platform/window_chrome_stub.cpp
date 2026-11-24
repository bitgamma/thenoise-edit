// SPDX-License-Identifier: Apache-2.0
#include "platform/window_chrome.h"

namespace noise::platform {

bool enable_client_titlebar(const void*, const void*) { return false; }
void move_client_window() {}
void minimize_client_window() {}
void toggle_client_window_maximized() {}
int client_edge_resize_direction(float, float, int, int) { return -1; }
bool begin_client_edge_resize(float, float, int, int) { return false; }

}
