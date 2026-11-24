// SPDX-License-Identifier: Apache-2.0
#include "platform/window_chrome.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <array>
#include <cstdint>

namespace noise::platform {
namespace {

constexpr unsigned long kMotifDecorationsHint = 1UL << 1;
constexpr long kWmStateToggle = 2;
constexpr long kMoveDirection = 8;
constexpr long kTitlebarMouseButton = 1;
constexpr float kResizeEdgePx = 7.0f;

Display* display = nullptr;
Window window = 0;
Atom moveresize_atom = None;

bool send_wm_message(Atom type, long value0, long value1, long value2, long value3) {
    if (display == nullptr || window == 0 || type == None) return false;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = value0;
    event.xclient.data.l[1] = value1;
    event.xclient.data.l[2] = value2;
    event.xclient.data.l[3] = value3;
    event.xclient.data.l[4] = 0;
    const Status sent = XSendEvent(display, DefaultRootWindow(display), False,
                                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    return sent != 0;
}

// An atom existing on the display is not proof that a window manager implements it.
// Keep native decorations when the active WM does not advertise interactive moves.
bool wm_supports(Display* candidate, Atom feature) {
    const Atom supported = XInternAtom(candidate, "_NET_SUPPORTED", True);
    if (supported == None || feature == None) return false;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const int result = XGetWindowProperty(candidate, DefaultRootWindow(candidate), supported,
                                           0, 4096, False, XA_ATOM, &actual_type,
                                           &actual_format, &item_count, &remaining, &data);
    bool found = false;
    if (result == Success && actual_type == XA_ATOM && actual_format == 32 && data != nullptr) {
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < item_count; ++i) {
            if (atoms[i] == feature) {
                found = true;
                break;
            }
        }
    }
    if (data != nullptr) XFree(data);
    return found;
}

void begin_wm_move_resize(long direction) {
    if (display == nullptr || moveresize_atom == None) return;
    Window root = 0;
    Window child = 0;
    int root_x = 0;
    int root_y = 0;
    int win_x = 0;
    int win_y = 0;
    unsigned int keys = 0;
    if (XQueryPointer(display, window, &root, &child, &root_x, &root_y,
                      &win_x, &win_y, &keys) == False) return;
    XUngrabPointer(display, CurrentTime);
    send_wm_message(moveresize_atom, root_x, root_y, direction, kTitlebarMouseButton);
}

}

bool enable_client_titlebar(const void* native_display, const void* native_window) {
    Display* candidate = static_cast<Display*>(const_cast<void*>(native_display));
    const Window native = static_cast<Window>(reinterpret_cast<std::uintptr_t>(native_window));
    if (candidate == nullptr || native == 0) return false;
    const Atom moveresize = XInternAtom(candidate, "_NET_WM_MOVERESIZE", True);
    if (!wm_supports(candidate, moveresize)) return false;
    const Atom motif = XInternAtom(candidate, "_MOTIF_WM_HINTS", False);
    if (motif == None) return false;

    const std::array<unsigned long, 5> hints{kMotifDecorationsHint, 0, 0, 0, 0};
    XChangeProperty(candidate, native, motif, motif, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(hints.data()),
                    static_cast<int>(hints.size()));
    XUnmapWindow(candidate, native);
    XMapWindow(candidate, native);
    XFlush(candidate);
    display = candidate;
    window = native;
    moveresize_atom = moveresize;
    return true;
}

void move_client_window() {
    begin_wm_move_resize(kMoveDirection);
}

void minimize_client_window() {
    if (display == nullptr || window == 0) return;
    XIconifyWindow(display, window, DefaultScreen(display));
    XFlush(display);
}

void toggle_client_window_maximized() {
    if (display == nullptr || window == 0) return;
    const Atom state = XInternAtom(display, "_NET_WM_STATE", True);
    const Atom vertical = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", True);
    const Atom horizontal = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
    if (state != None && vertical != None && horizontal != None) {
        send_wm_message(state, kWmStateToggle, static_cast<long>(vertical),
                        static_cast<long>(horizontal), 1);
    }
}

int client_edge_resize_direction(float x, float y, int width, int height) {
    if (width <= 0 || height <= 0 || x < 0.0f || y < 0.0f ||
        x > static_cast<float>(width) || y > static_cast<float>(height)) return -1;
    const bool left = x <= kResizeEdgePx;
    const bool right = x >= static_cast<float>(width) - kResizeEdgePx;
    const bool top = y <= kResizeEdgePx;
    const bool bottom = y >= static_cast<float>(height) - kResizeEdgePx;
    long direction = -1;
    if (top && left) direction = 0;
    else if (top && right) direction = 2;
    else if (bottom && right) direction = 4;
    else if (bottom && left) direction = 6;
    else if (top) direction = 1;
    else if (right) direction = 3;
    else if (bottom) direction = 5;
    else if (left) direction = 7;
    return static_cast<int>(direction);
}

bool begin_client_edge_resize(float x, float y, int width, int height) {
    if (display == nullptr || window == 0) return false;
    const int direction = client_edge_resize_direction(x, y, width, height);
    if (direction < 0) return false;
    begin_wm_move_resize(direction);
    return true;
}

}
