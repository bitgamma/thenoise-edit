#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove that the native-dialog probe never focuses an unmapped X11 window.

GTK names a file chooser before it is mapped. XSetInputFocus on that unviewable
window raises BadMatch and can strand the actual picker until the CTest timeout.
This test drives a real X server without starting the C++ editor.
"""

import ctypes

import file_dialog_probe as picker
import wire_drive as wd


def main() -> int:
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0
    display = wd.ensure_display(640, 480)
    windows = picker.NativeWindows(display)
    x11 = windows.x11
    x11.XCreateSimpleWindow.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
        ctypes.c_ulong, ctypes.c_ulong]
    x11.XCreateSimpleWindow.restype = ctypes.c_ulong
    x11.XStoreName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p]
    x11.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XUnmapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XDestroyWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XGetInputFocus.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int)]
    name = b"noise-edit unmapped chooser probe"
    window = x11.XCreateSimpleWindow(windows.dpy, windows.root, 15, 15, 240, 120, 0, 0, 0)
    try:
        x11.XStoreName(windows.dpy, window, name)
        x11.XSync(windows.dpy, 0)
        assert windows.find(name.decode()) is None, "unmapped dialog was offered for focus"
        try:
            windows.focus(window)
        except wd.WireError:
            pass
        else:
            raise AssertionError("an unmapped window was focused, provoking BadMatch")

        x11.XMapWindow(windows.dpy, window)
        x11.XSync(windows.dpy, 0)
        assert windows.find(name.decode()) == window, "visible dialog not discovered"
        windows.focus(window)
        focused = ctypes.c_ulong()
        revert = ctypes.c_int()
        x11.XGetInputFocus(windows.dpy, ctypes.byref(focused), ctypes.byref(revert))
        assert focused.value == window, "visible dialog did not receive keyboard focus"

        x11.XUnmapWindow(windows.dpy, window)
        x11.XSync(windows.dpy, 0)
        assert windows.find(name.decode()) is None, "closed dialog still discovered"
        print("PASS: hidden chooser ignored, visible chooser focusable, unmapped again ignored")
        return 0
    finally:
        x11.XDestroyWindow(windows.dpy, window)
        windows.close()


if __name__ == "__main__":
    raise SystemExit(main())
