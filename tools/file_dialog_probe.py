#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the detached native picker, cancellation, and a real image import."""

from __future__ import annotations

import argparse
import ctypes
import os
import pathlib
import socket
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

WIDTH, HEIGHT = 1280, 800
SCREEN_WIDTH, SCREEN_HEIGHT = 2000, 1200
KEY_ESCAPE = 0xFF1B
KEY_RETURN = 0xFF0D


def check(condition: bool, message: str) -> None:
    if not condition:
        raise wd.WireError(message)


class NativeWindows:
    """Find and move top-level X11 windows, including windows reparented by a WM."""

    REVERT_TO_PARENT = 2

    def __init__(self, display: str) -> None:
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x11.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
        self.x11.XDefaultScreen.restype = ctypes.c_int
        self.x11.XDisplayWidth.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x11.XDisplayWidth.restype = ctypes.c_int
        self.x11.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x11.XDisplayHeight.restype = ctypes.c_int
        self.x11.XQueryTree.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
            ctypes.POINTER(ctypes.c_uint)]
        self.x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                        ctypes.POINTER(ctypes.c_void_p)]
        self.x11.XFetchName.restype = ctypes.c_int
        self.x11.XFree.argtypes = [ctypes.c_void_p]
        self.x11.XTranslateCoordinates.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong)]
        self.x11.XMoveWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                         ctypes.c_int, ctypes.c_int]
        self.x11.XGetGeometry.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        self.x11.XGetGeometry.restype = ctypes.c_int
        self.x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                                            ctypes.c_ulong]
        self.x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint
        self.xtst.XTestFakeKeyEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise wd.WireError(f"cannot open X display {display}")
        self.root = int(self.x11.XDefaultRootWindow(self.dpy))
        self.screen = self.x11.XDefaultScreen(self.dpy)
        self.width = int(self.x11.XDisplayWidth(self.dpy, self.screen))
        self.height = int(self.x11.XDisplayHeight(self.dpy, self.screen))

    def close(self) -> None:
        self.x11.XCloseDisplay(ctypes.c_void_p(self.dpy))

    def _children(self, parent: int) -> list[int]:
        root = ctypes.c_ulong()
        parent_return = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        if not self.x11.XQueryTree(self.dpy, parent, ctypes.byref(root),
                                   ctypes.byref(parent_return), ctypes.byref(children),
                                   ctypes.byref(count)):
            return []
        values = [int(children[i]) for i in range(count.value)]
        if children:
            self.x11.XFree(children)
        return values

    def _title(self, window: int) -> str:
        value = ctypes.c_void_p()
        if not self.x11.XFetchName(self.dpy, window, ctypes.byref(value)) or not value:
            return ""
        title = ctypes.string_at(value).decode(errors="replace")
        self.x11.XFree(value)
        return title

    def find(self, title: str) -> int | None:
        pending = [self.root]
        while pending:
            parent = pending.pop()
            for window in self._children(parent):
                if title in self._title(window):
                    return window
                pending.append(window)
        return None

    def geometry(self, window: int) -> tuple[int, int]:
        x, y = ctypes.c_int(), ctypes.c_int()
        child = ctypes.c_ulong()
        self.x11.XTranslateCoordinates(self.dpy, window, self.root, 0, 0,
                                       ctypes.byref(x), ctypes.byref(y), ctypes.byref(child))
        return x.value, y.value

    def top_level(self, window: int) -> int:
        current = window
        while True:
            root = ctypes.c_ulong()
            parent = ctypes.c_ulong()
            children = ctypes.POINTER(ctypes.c_ulong)()
            count = ctypes.c_uint()
            if not self.x11.XQueryTree(self.dpy, current, ctypes.byref(root),
                                       ctypes.byref(parent), ctypes.byref(children),
                                       ctypes.byref(count)):
                return current
            if children:
                self.x11.XFree(children)
            if parent.value == self.root or parent.value == 0:
                return current
            current = int(parent.value)

    def move_outside(self, window: int, app_window: int) -> bool | None:
        app_x, app_y = self.geometry(app_window)
        del app_y
        target_x = app_x + WIDTH + 24
        if self.width <= target_x + 24:
            return None
        self.x11.XMoveWindow(self.dpy, self.top_level(window), target_x, 24)
        self.x11.XSync(self.dpy, 0)
        moved_x, _ = self.geometry(window)
        return moved_x >= app_x + WIDTH

    def key(self, keysym: int) -> None:
        code = self.x11.XKeysymToKeycode(self.dpy, keysym)
        if not code:
            raise wd.WireError(f"no X keycode for keysym {keysym:#x}")
        self.xtst.XTestFakeKeyEvent(self.dpy, code, 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, code, 0, 0)
        self.x11.XFlush(self.dpy)
        time.sleep(0.05)

    def click(self, window: int, x_offset: int, y_offset: int) -> None:
        root = ctypes.c_ulong()
        x, y = ctypes.c_int(), ctypes.c_int()
        width, height = ctypes.c_uint(), ctypes.c_uint()
        border, depth = ctypes.c_uint(), ctypes.c_uint()
        if not self.x11.XGetGeometry(self.dpy, window, ctypes.byref(root),
                                     ctypes.byref(x), ctypes.byref(y),
                                     ctypes.byref(width), ctypes.byref(height),
                                     ctypes.byref(border), ctypes.byref(depth)):
            raise wd.WireError("cannot read native dialog geometry")
        screen_x, screen_y = self.geometry(window)
        target_x = screen_x + min(x_offset, width.value - 1)
        target_y = screen_y + min(y_offset, height.value - 1)
        self.xtst.XTestFakeMotionEvent(self.dpy, self.screen, target_x, target_y, 0)
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 1, 0)
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 0, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.1)

    def focus(self, window: int) -> None:
        self.x11.XSetInputFocus(self.dpy, window, self.REVERT_TO_PARENT, 0)
        self.x11.XSync(self.dpy, 0)


def wait_for_dialog(windows: NativeWindows, proc, title: str) -> int:
    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise wd.WireError(f"editor exited while opening {title!r}")
        window = windows.find(title)
        if window is not None:
            return window
        time.sleep(0.05)
    raise wd.WireError(f"timed out waiting for native dialog {title!r}")


def wait_for_pick(wire: wd.Wire, running: bool) -> dict:
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        state = wire.call("file.pick.state")
        if state["running"] == running:
            return state
        time.sleep(0.05)
    raise wd.WireError(f"native picker did not reach running={running}: {state}")


def open_and_cancel(wire: wd.Wire, windows: NativeWindows, proc, request: dict,
                    theme: str, move_check: bool) -> None:
    wire.call("ui.theme", {"theme": theme})
    wd.settle(wire)
    wire.call("file.pick", request)
    dialog = wait_for_dialog(windows, proc, request["title"])
    app = windows.find(" - noise-edit")
    check(app is not None and dialog != app, "native picker did not open as a separate window")
    if move_check:
        moved = windows.move_outside(dialog, app)
        if moved is None:
            print("display is too narrow for the detached-window movement check")
        else:
            check(moved, "native picker could not be moved beyond the editor window bounds")
    windows.focus(dialog)
    windows.key(KEY_ESCAPE)
    state = wait_for_pick(wire, False)
    check(state.get("cancelled") is True,
          f"Escape did not cancel the {theme} picker: {state}")
    wd.settle(wire)


def choose_file(wire: wd.Wire, windows: NativeWindows, proc, image: pathlib.Path) -> dict:
    request = {"for": "image", "path": str(image), "title": "Insert image"}
    wire.call("file.pick", request)
    dialog = wait_for_dialog(windows, proc, request["title"])
    print("native file chooser opened", flush=True)
    windows.click(dialog, 280, 84)
    windows.click(dialog, 1046, 798)
    return wait_for_pick(wire, False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if reason := wd.display_skip():
        print(f"SKIP: {reason}")
        return 0

    with tempfile.TemporaryDirectory(prefix="noise-file-dialog-") as temp:
        root = pathlib.Path(temp)
        runtime = root / "runtime"
        runtime.mkdir()
        image = root / "reference.png"
        wd.write_fixture_png(image, 16, 12)
        stderr_path = root / "app.stderr"
        with stderr_path.open("w") as stderr:
            display = wd.ensure_display(SCREEN_WIDTH, SCREEN_HEIGHT)
            proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None, stderr,
                                {"THENOISE_URL": None, "THENOISE_API_KEY": None},
                                start_dialog=False)
            wire = None
            windows = None
            phase = "starting the editor"
            try:
                wire = wd.Wire(wd.wire_port(runtime, proc, 30.0))
                windows = NativeWindows(display)
                phase = "creating the probe document"
                wire.call("doc.new", {"width": 64, "height": 64})
                wd.settle(wire)

                phase = "opening and cancelling the dark themed picker"
                open_and_cancel(wire, windows, proc,
                                {"for": "open", "title": "Choose a document"}, "dark", True)
                phase = "opening and cancelling the multi-select picker"
                open_and_cancel(wire, windows, proc,
                                {"for": "reference", "title": "Add AI reference images",
                                 "multiple": True}, "light", False)

                phase = "selecting an image through the native picker"
                answer = choose_file(wire, windows, proc, image)
                phase = "checking the selected image import"
                wd.settle(wire)
                document = wire.call("doc.get")
                check(not answer["cancelled"] and answer["paths"] == [str(image)],
                      f"selected image did not reach file.pick.state: {answer}")
                check(any(layer["name"] == "reference" for layer in document["layers"]),
                      "selected image was not inserted into the document")
                check(proc.poll() is None, "editor exited after importing the selected image")
                print("native picker opened in a separate window, moved outside the editor, cancelled, "
                      "and imported a selected image")
                return 0
            except (wd.WireError, OSError, socket.timeout, TimeoutError) as exc:
                stderr.flush()
                print(f"FAIL while {phase}: {type(exc).__name__}: {exc!r}\n"
                      f"{wd.app_stderr_tail(stderr_path)}", file=sys.stderr)
                return 1
            finally:
                if windows is not None:
                    windows.close()
                wd.shutdown(proc, wire)


if __name__ == "__main__":
    sys.exit(main())
