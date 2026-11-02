#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Injected-input gestures against the running app (keyboard and mouse).

The debug wire can invoke commands, but it cannot press a key: a gesture reaches the
command table through the host's event handling, and only a real X event exercises that.
This drives the app with synthetic key and mouse events (XTest via libXtst) and asserts
what the app reports afterwards.

Run through ctest (`test_input_gestures`) or directly:

    python3 tools/wire_input.py --bin build/noise-edit

Skips loudly (exit 0, "SKIP:" on stdout) when there is no display, no libXtst, or no Xvfb
to make one: unavailable input plumbing is not a regression.
"""

from __future__ import annotations

import argparse
import ctypes
import math
import shutil
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402  (the wire client and the fixture writer live there)

WHEEL_CLICKS = 3
DRAG = (40, -30)
DRAG_ZOOM = 2.0
PIN_TOLERANCE_PX = 1.0
ORIGIN_TOLERANCE = 1.0
FRAME_SETTLE = 0.25


class Injector:
    """XTest fake input on a display, shared by the app and this process."""

    REVERT_TO_PARENT = 2  # RevertToParent in Xlib

    def __init__(self, display: str) -> None:
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x11.XQueryTree.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
            ctypes.POINTER(ctypes.c_uint)]
        self.x11.XGetGeometry.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        self.x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                                            ctypes.c_ulong]
        self.x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.name = display
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise wd.WireError(f"cannot open X display {display}")

    def close(self) -> None:
        self.x11.XCloseDisplay(ctypes.c_void_p(self.dpy))

    def _flush(self) -> None:
        self.x11.XFlush(ctypes.c_void_p(self.dpy))
        time.sleep(FRAME_SETTLE)

    def focus_app_window(self, width: int, height: int) -> None:
        """Give the app the keyboard. There is no window manager on this display, so focus
        does not follow the pointer: a key event with no focused window reaches nobody,
        while mouse events still land on the window under the pointer. The app's window is
        the one sized like the window we asked it to open."""
        root = self.x11.XDefaultRootWindow(self.dpy)
        root_return = ctypes.c_ulong()
        parent_return = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        self.x11.XQueryTree(self.dpy, root, ctypes.byref(root_return), ctypes.byref(parent_return),
                            ctypes.byref(children), ctypes.byref(count))
        for i in range(count.value):
            other_root = ctypes.c_ulong()
            x, y = ctypes.c_int(), ctypes.c_int()
            wide, high = ctypes.c_uint(), ctypes.c_uint()
            border, depth = ctypes.c_uint(), ctypes.c_uint()
            if not self.x11.XGetGeometry(self.dpy, children[i], ctypes.byref(other_root),
                                         ctypes.byref(x), ctypes.byref(y), ctypes.byref(wide),
                                         ctypes.byref(high), ctypes.byref(border),
                                         ctypes.byref(depth)):
                continue
            if wide.value == width and high.value == height:
                self.x11.XSetInputFocus(self.dpy, children[i], self.REVERT_TO_PARENT, 0)
                self.x11.XSync(self.dpy, 0)
                return
        raise wd.WireError(f"no {width}x{height} window to focus on {self.name}")

    def move(self, x: float, y: float) -> None:
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(round(x)), int(round(y)), 0)
        self._flush()

    def button(self, number: int, down: bool) -> None:
        self.xtst.XTestFakeButtonEvent(self.dpy, number, 1 if down else 0, 0)
        self._flush()

    def wheel(self, clicks: int) -> None:
        """Scroll: positive clicks go up, negative down. X11 wheel buttons are 4 (up) and 5 (down),
        and `range()` of a negative count is empty - a helper that silently did nothing for a downward
        scroll is how a panel that does not scroll looks like a panel that does."""
        button = 4 if clicks > 0 else 5
        for _ in range(abs(clicks)):
            self.button(button, True)
            self.button(button, False)

    def key(self, keysym: int) -> None:
        """One press/release of the key whose keysym is `keysym`, modifiers untouched."""
        code = self.x11.XKeysymToKeycode(self.dpy, keysym)
        if not code:
            raise wd.WireError(f"no keycode for keysym {keysym:#x} on this display")
        self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(code), 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(code), 0, 0)
        self._flush()

    def modified_key(self, keysym: int, *modifiers: int) -> None:
        """One press/release of `keysym` held with `modifiers`, in the order given.

        Shift matters beyond the modifier flag: it selects which command a punctuation key
        reaches, so it has to be a real modifier state and not a shifted keysym.
        """
        codes = []
        for modifier in modifiers:
            code = self.x11.XKeysymToKeycode(self.dpy, modifier)
            if not code:
                raise wd.WireError(f"no keycode for modifier {modifier:#x} on this display")
            codes.append(code)
        code = self.x11.XKeysymToKeycode(self.dpy, keysym)
        if not code:
            raise wd.WireError(f"no keycode for keysym {keysym:#x} on this display")
        for modifier in codes:
            self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(modifier), 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(code), 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(code), 0, 0)
        for modifier in reversed(codes):
            self.xtst.XTestFakeKeyEvent(self.dpy, ctypes.c_uint(modifier), 0, 0)
        self._flush()


class Display:
    """The display the app and the injector share: ours only if we had to start one."""

    def __enter__(self) -> str:
        try:
            return wd.ensure_display(WIDTH, HEIGHT, 15.0)
        except wd.WireError as exc:
            raise Skip(str(exc)) from exc

    def __exit__(self, *_exc) -> bool:
        return False


class Skip(Exception):
    pass


def shutil_which(name: str) -> bool:
    from shutil import which
    return which(name) is not None


WIDTH, HEIGHT = 800, 500

KEY_ESCAPE = 0xFF1B    # XK_Escape
KEY_F = 0x66           # XK_f
KEY_S = 0x73           # XK_s
KEY_Z = 0x7A           # XK_z
KEY_J = 0x6A           # XK_j, bound to nothing today
KEY_COMMA = 0x2C       # XK_comma
KEY_PERIOD = 0x2E      # XK_period
KEY_CTRL_L = 0xFFE3    # XK_Control_L
KEY_B = 0x62           # XK_b
KEY_E = 0x65           # XK_e
KEY_V = 0x0076         # XK_v
KEY_M = 0x006D         # XK_m
KEY_D = 0x0064         # XK_d
KEY_I = 0x0069         # XK_i
KEY_L = 0x006C         # XK_l, freehand lasso
KEY_BACKSPACE = 0xFF08  # XK_BackSpace, `ai.remove` with Ctrl
# One press of a step key, as `core/brush.h` defines it: the probe asserts a step happened rather
# than recomputing the value, so this is the width of "one", not a copy of the arithmetic.
BRUSH_STEP = 0.05
KEY_SHIFT_L = 0xFFE1   # XK_Shift_L
PAINT = (200, 120)     # screen pixels of the test left-drag
PAINT_STEPS = 8
MOVE = (40, 20)        # document pixels of the test move drag


def drive(wire: wd.Wire, inj: Injector, proc: subprocess.Popen, fixture: Path) -> list[str]:
    fails: list[str] = []
    state = wd.wait_for_fit(wire)
    ox, oy, cw, ch = state["canvas"]
    base_scale = state["view"]["scale"]
    px, py = ox + cw * 0.62, oy + ch * 0.44

    # Wheel over a point: zoom, and the document point under the cursor stays put.
    inj.move(px, py)
    world = [(px - ox) / state["view"]["scale"] + state["view"]["origin"][0],
             (py - oy) / state["view"]["scale"] + state["view"]["origin"][1]]
    inj.wheel(WHEEL_CLICKS)
    wd.settle(wire)
    after = wire.call("app.state")
    scale = after["view"]["scale"]
    pinned = [(px - ox) / scale + after["view"]["origin"][0],
              (py - oy) / scale + after["view"]["origin"][1]]
    print(f"wheel x{WHEEL_CLICKS}: scale {base_scale:.4f} -> {scale:.4f}; "
          f"cursor point {world[0]:.2f},{world[1]:.2f} -> {pinned[0]:.2f},{pinned[1]:.2f}")
    if scale <= base_scale:
        fails.append("wheel over the canvas did not zoom in (is the canvas treated as hovered?)")
    if max(abs(pinned[0] - world[0]), abs(pinned[1] - world[1])) > PIN_TOLERANCE_PX:
        fails.append("wheel zoom was not about the cursor")

    # Right-drag with the document larger than the canvas on both axes: when it is
    # smaller, clamp_origin re-centres it and the pan cannot be observed at all.
    image_w, image_h = wire.call("app.state")["view"]["image"]
    wire.call("view.set", {"scale": DRAG_ZOOM * max(cw / image_w, ch / image_h),
                           "origin": [0.0, 0.0]})
    wd.settle(wire)
    before = wire.call("app.state")["view"]["origin"]
    inj.move(px, py)
    inj.button(3, True)
    inj.move(px + DRAG[0] * 0.5, py + DRAG[1] * 0.5)
    inj.move(px + DRAG[0], py + DRAG[1])
    inj.button(3, False)
    wd.settle(wire)
    view = wire.call("app.state")["view"]
    want = [before[0] - DRAG[0] / view["scale"], before[1] - DRAG[1] / view["scale"]]
    got = view["origin"]
    print(f"right-drag {DRAG[0]},{DRAG[1]} at scale {view['scale']:.2f}: origin "
          f"{before[0]:.2f},{before[1]:.2f} -> {got[0]:.2f},{got[1]:.2f} (want "
          f"{want[0]:.2f},{want[1]:.2f})")
    if max(abs(got[0] - want[0]), abs(got[1] - want[1])) > ORIGIN_TOLERANCE:
        fails.append("right-drag did not pan the canvas")

    # A key bound in the table reaches the command: zoom away, press F, get a fit.
    wire.call("view.zoom", {"factor": 4.0, "anchor": [cw * 0.5, ch * 0.5]})
    wd.settle(wire)
    inj.key(KEY_F)
    wd.settle(wire)
    state = wire.call("app.state")
    try:
        wd.check_fit_holds_document(state, "F key")
        print(f"F key: scale back to {state['view']['scale']:.4f}, document inside the canvas")
    except wd.WireError as exc:
        fails.append(str(exc))

    # A shortcut whose command fails must be reported, not swallowed. A document created
    # with doc.new has no path, so Ctrl+S cannot do anything but say so.
    wire.call("doc.new", {"width": 64, "height": 48})
    wd.settle(wire)
    inj.modified_key(KEY_S, KEY_CTRL_L)
    wd.settle(wire)
    lines = wire.call("log.tail", {"count": 40})["lines"]
    reported = [line for line in lines if "Ctrl+S" in line and "doc.save" in line]
    print(f"Ctrl+S on a pathless document: {reported[-1] if reported else 'nothing logged'}")
    if not reported:
        fails.append("Ctrl+S reached neither doc.save nor the log")

    # A key bound to an editing command must move pixels, not just appear in the table:
    # stroke, then Ctrl+Z, and the document's own checksum says whether it went back.
    before = wire.call("doc.get", {})["checksum"]
    wire.call("tool.stroke", {
        "points": [[8.0, 8.0], [48.0, 32.0]], "tool": "brush", "size": 12.0, "hardness": 0.5,
        "opacity": 1.0, "flow": 0.8, "spacing": 0.3, "color": [10, 200, 60, 255]})
    painted = wire.call("doc.get", {})["checksum"]
    inj.modified_key(KEY_Z, KEY_CTRL_L)
    wd.settle(wire)
    restored = wire.call("doc.get", {})["checksum"]
    print(f"Ctrl+Z: document {before[:12]} -> stroke {painted[:12]} -> undo {restored[:12]}")
    if painted == before:
        fails.append("tool.stroke changed nothing, so Ctrl+Z proved nothing")
    if restored != before:
        fails.append("Ctrl+Z did not undo the stroke")

    # A left-drag paints. This is the whole pointer path at once: ImGui hover and capture,
    # tool.pointer per event, one provisional stroke, one committed step, one undo.
    state = wire.call("app.state")
    doc = wd.document_screen_rect(state)
    before = wire.call("doc.get", {})["checksum"]
    if wire.call("app.state")["document"]["history"]["undo_depth"] != 0:
        fails.append("the drag needs an empty history to count its own step")
    inj.move(doc[0] + doc[2] * 0.3, doc[1] + doc[3] * 0.5)
    inj.button(1, True)
    wd.settle(wire)
    for step in range(1, PAINT_STEPS + 1):
        inj.move(doc[0] + doc[2] * 0.3 + PAINT[0] * step / PAINT_STEPS,
                 doc[1] + doc[3] * 0.5 + PAINT[1] * step / PAINT_STEPS)
        time.sleep(0.03)
    inj.button(1, False)
    wd.settle(wire)
    painted = wire.call("doc.get", {})["checksum"]
    stroke = wire.call("app.state")["stroke"]
    depth = wire.call("app.state")["document"]["history"]["undo_depth"]
    print(f"left-drag {PAINT[0]},{PAINT[1]}: document {before[:12]} -> {painted[:12]}, "
          f"stroke active {stroke['active']}, undo steps {depth}")
    if painted == before:
        fails.append("a left-drag over the document painted nothing")
    if stroke["active"]:
        fails.append("the stroke stayed active after the button was released")
    if depth != 1:
        fails.append(f"one drag must be one undo step, got {depth}")
    inj.modified_key(KEY_Z, KEY_CTRL_L)
    wd.settle(wire)
    restored = wire.call("doc.get", {})["checksum"]
    print(f"Ctrl+Z after the drag: {restored[:12]}")
    if restored != before:
        fails.append("Ctrl+Z did not undo the drag")

    # `V` selects the move tool. Checked here rather than only in `test_app` because a tool row's
    # key is registered from the table, and the table is only proven by an event that arrives as a
    # real KeyPress.
    tool_before = wire.call("app.state")["brush"]["tool"]
    inj.key(KEY_V)
    wd.settle(wire)
    tool_after = wire.call("app.state")["brush"]["tool"]
    print(f"V key: tool {tool_before} -> {tool_after}")
    if tool_after != "move":
        fails.append(f"V selected '{tool_after}', expected 'move'")

    # The move tool with a real mouse: press, drag, release. The release is the point of the check -
    # a gesture that never ends leaves the document provisional, the button held and no undo step -
    # and it is only visible through an injected button, because a wire client calls `release`
    # itself and never notices that the window dropped it.
    #
    # Something has to be there to move: the `doc.new` above (the pathless-save case) replaced the
    # fixture, and a move over an empty layer is correctly a no-op that changes nothing.
    wire.call("tool.stroke", {"points": [[10, 12], [40, 26]], "tool": "brush", "size": 16.0,
                              "hardness": 1.0, "opacity": 1.0, "flow": 1.0, "spacing": 0.25,
                              "color": [20, 120, 220, 255]})
    # `tool.stroke` carries the tool it paints with and leaves it selected, so the move tool has to
    # be chosen after the painting rather than before it.
    wire.call("tool.set", {"tool": "move"})
    state = wire.call("app.state")
    if state["brush"]["tool"] != "move":
        fails.append("the move drag ran with the wrong tool selected")
    doc = wd.document_screen_rect(state)
    scale = state["view"]["scale"]
    before = wire.call("doc.get", {})["checksum"]
    depth_before = wire.call("app.state")["document"]["history"]["undo_depth"]
    inj.move(doc[0] + doc[2] * 0.35, doc[1] + doc[3] * 0.5)
    inj.button(1, True)
    wd.settle(wire)
    for fraction in (0.25, 0.5, 1.0):
        inj.move(doc[0] + doc[2] * 0.35 + MOVE[0] * scale * fraction,
                 doc[1] + doc[3] * 0.5 + MOVE[1] * scale * fraction)
        time.sleep(0.03)
    wd.settle(wire)  # the last move is handled by a later frame than the one that read state
    mid = wire.call("app.state")
    inj.button(1, False)
    wd.settle(wire)
    after = wire.call("app.state")
    moved = wire.call("doc.get", {})["checksum"]
    print(f"move drag {MOVE[0]},{MOVE[1]} doc px: document {before[:12]} -> {moved[:12]}, "
          f"during {mid['stroke']['dx']},{mid['stroke']['dy']} over {mid['stroke']['move_steps']} "
          f"events, active {mid['stroke']['active']} -> {after['stroke']['active']}, "
          f"undo steps {depth_before} -> {after['document']['history']['undo_depth']}")
    if not mid["stroke"]["active"]:
        fails.append("the move gesture was not live while the button was down")
    if mid["stroke"]["dx"] < 1 or mid["stroke"]["dy"] < 1:
        fails.append("the move gesture reported no offset after a drag")
    if after["stroke"]["active"]:
        fails.append("the move gesture never ended: the button release never reached it")
    if moved == before:
        fails.append("a move-tool drag over the painted document changed nothing")
    if after["document"]["history"]["undo_depth"] != depth_before + 1:
        fails.append(f"a move gesture must be one undo step, got "
                     f"{after['document']['history']['undo_depth'] - depth_before}")
    inj.modified_key(KEY_Z, KEY_CTRL_L)
    wd.settle(wire)
    undone = wire.call("doc.get", {})["checksum"]
    print(f"Ctrl+Z after the move: {undone[:12]}")
    if undone != before:
        fails.append("Ctrl+Z did not undo the move")

    # The other marquee tool, through the same real key path: Shift+M has to reach the ellipse row,
    # and the drag it makes has to produce the curve rather than its box.
    inj.modified_key(KEY_M, KEY_SHIFT_L)
    wd.settle(wire)
    state = wire.call("app.state")
    if state["brush"]["tool"] != "select_ellipse":
        fails.append(f"Shift+M selected '{state['brush']['tool']}', expected 'select_ellipse'")
    doc = wd.document_screen_rect(state)
    inj.move(doc[0] + doc[2] * 0.2, doc[1] + doc[3] * 0.2)
    inj.button(1, True)
    wd.settle(wire)
    inj.move(doc[0] + doc[2] * 0.55, doc[1] + doc[3] * 0.6)
    wd.settle(wire)
    dragging = wire.call("app.state")
    inj.button(1, False)
    wd.settle(wire)
    curve = wire.call("select.get", {})
    want_curve = math.pi * (curve["bounds"][2] / 2.0) * (curve["bounds"][3] / 2.0)
    box = curve["bounds"][2] * curve["bounds"][3]
    print(f"Shift+M drag: shape {dragging['stroke']['marquee_shape']}, bounds {curve['bounds']}, "
          f"area {curve['area']:.1f} against pi*a*b {want_curve:.1f}, {curve['covered']} of {box} touched")
    if dragging["stroke"]["marquee_shape"] != "ellipse":
        fails.append(f"the ellipse tool's drag reported {dragging['stroke']['marquee_shape']}")
    if abs(curve["area"] - want_curve) > 0.02 * want_curve:
        fails.append(f"the dragged curve claims {curve['area']:.1f} px, pi*a*b is {want_curve:.1f}")
    if curve["covered"] >= box:
        fails.append("the dragged curve filled its own box")
    wire.call("screenshot", {"name": "ellipse-ants"})

    # `L` selects the Lasso row, and the outline is drawn from document coordinates at two zooms.
    # Capturing while the button is down proves the preview exists; the committed mask bounds and
    # area then prove view-to-document mapping did not scale the selection with the screen.
    lasso_points = [(8.0, 8.0), (52.0, 8.0), (44.0, 18.0), (54.0, 36.0),
                    (22.0, 39.0), (8.0, 28.0)]
    lasso_results = []
    for scale, theme in ((2.0, "dark"), (4.0, "light")):
        wire.call("select.clear", {})
        wire.call("ui.theme", {"theme": theme})
        wire.call("view.set", {"scale": scale, "origin": [0.0, 0.0]})
        wd.settle(wire)
        inj.key(KEY_L)
        wd.settle(wire)
        state = wire.call("app.state")
        if state["brush"]["tool"] != "lasso":
            fails.append(f"L selected '{state['brush']['tool']}', expected 'lasso'")
        doc = wd.document_screen_rect(state)
        actual_scale = state["view"]["scale"]
        screen_points = [(doc[0] + x * actual_scale, doc[1] + y * actual_scale)
                         for x, y in lasso_points]
        inj.move(*screen_points[0])
        inj.button(1, True)
        wd.settle(wire)
        for point in screen_points[1:]:
            inj.move(*point)
            time.sleep(0.04)
        mid = wire.call("app.state")["stroke"]
        preview = wire.call("screenshot", {"name": f"lasso-{theme}-preview"})
        inj.button(1, False)
        wd.settle(wire)
        selection = wire.call("select.get", {})
        ants = wire.call("screenshot", {"name": f"lasso-{theme}-ants"})
        print(f"Lasso {theme} at {actual_scale:.2f}x: {len(mid['lasso_points'])} preview points, "
              f"bounds {selection['bounds']}, area {selection['area']:.1f}; "
              f"captures {preview['path']} and {ants['path']}")
        if not mid["active"] or mid["marquee_shape"] != "lasso" or len(mid["lasso_points"]) < 5:
            fails.append("Lasso did not expose its freehand preview while the button was down")
        if not selection["active"] or selection["covered"] == 0:
            fails.append("Lasso release left no marked area")
        if selection["area"] >= selection["covered"]:
            fails.append("Lasso coverage has no antialiased boundary")
        lasso_results.append(selection)
    if lasso_results:
        first, last = lasso_results
        if max(abs(a - b) for a, b in zip(first["bounds"], last["bounds"])) > 1:
            fails.append(f"Lasso bounds changed with zoom: {first['bounds']} -> {last['bounds']}")
        if abs(first["area"] - last["area"]) > 0.05 * first["area"]:
            fails.append(f"Lasso area changed with zoom: {first['area']:.1f} -> {last['area']:.1f}")
    selection_before_cancel = wire.call("select.get", {})
    state = wire.call("app.state")
    doc = wd.document_screen_rect(state)
    scale = state["view"]["scale"]
    start = (doc[0] + 12.0 * scale, doc[1] + 12.0 * scale)
    finish = (doc[0] + 38.0 * scale, doc[1] + 30.0 * scale)
    inj.move(*start)
    inj.button(1, True)
    wd.settle(wire)
    inj.move(*finish)
    wd.settle(wire)
    inj.key(KEY_ESCAPE)
    wd.settle(wire)
    cancelled = wire.call("app.state")
    inj.button(1, False)
    wd.settle(wire)
    if cancelled["stroke"]["active"]:
        fails.append("Escape left the Lasso gesture active")
    if wire.call("select.get", {}) != selection_before_cancel:
        fails.append("Escape changed the previous selection instead of cancelling the Lasso")
    if proc.poll() is not None:
        fails.append("Escape quit the editor instead of cancelling the active Lasso")
    wire.call("view.fit", {})
    wd.settle(wire)

    # The select tool: `M`, a marquee drawn by the pointer, and then proof that the selection holds
    # the paint - all of it through events, because what the app does with a real drag is what the
    # canvas window decides, not what a wire client asks for.
    inj.key(KEY_M)
    wd.settle(wire)
    state = wire.call("app.state")
    if state["brush"]["tool"] != "select":
        fails.append(f"M selected '{state['brush']['tool']}', expected 'select'")
    doc = wd.document_screen_rect(state)
    scale = state["view"]["scale"]
    canvas_area = state["document"]["width"] * state["document"]["height"]
    inj.move(doc[0] + doc[2] * 0.2, doc[1] + doc[3] * 0.2)
    inj.button(1, True)
    wd.settle(wire)
    inj.move(doc[0] + doc[2] * 0.55, doc[1] + doc[3] * 0.6)
    wd.settle(wire)
    mid = wire.call("app.state")
    inj.button(1, False)
    wd.settle(wire)
    sel = wire.call("select.get", {})
    print(f"M drag: marquee during {mid['stroke']['marquee']}, after bounds {sel['bounds']} "
          f"covering {sel['covered']} of {canvas_area} px, active {sel['active']}")
    if len(mid["stroke"]["marquee"]) != 4:
        fails.append("the drag reported no marquee while the button was down")
    if not sel["active"] or sel["covered"] == 0:
        fails.append("the marquee drag left no selection")
    if sel["bounds"] != mid["stroke"]["marquee"]:
        fails.append(f"the released selection {sel['bounds']} is not the marquee that was drawn "
                     f"{mid['stroke']['marquee']}")
    wire.call("screenshot", {"name": "selection-ants"})

    # A stroke that never reaches the selection changes no byte and costs no undo step; one that
    # reaches it changes the document and puts every byte back on Ctrl+Z.
    inside = sel["bounds"]
    outside_stroke = {"points": [[state["document"]["width"] - 14, state["document"]["height"] - 6],
                                 [state["document"]["width"] - 5, state["document"]["height"] - 3]],
                      "tool": "brush", "size": 5.0, "hardness": 1.0, "opacity": 1.0, "flow": 1.0,
                      "spacing": 0.25, "color": [250, 20, 20, 255]}
    inside_stroke = {"points": [[inside[0] + 2, inside[1] + 2],
                                [inside[0] + max(2, inside[2] - 3), inside[1] + max(2, inside[3] - 3)]],
                     "tool": "brush", "size": 5.0, "hardness": 1.0, "opacity": 1.0, "flow": 1.0,
                     "spacing": 0.25, "color": [250, 20, 20, 255]}
    held = wire.call("doc.get", {})["checksum"]
    depth_held = wire.call("app.state")["document"]["history"]["undo_depth"]
    wire.call("tool.stroke", outside_stroke)
    blocked = wire.call("doc.get", {})["checksum"]
    depth_blocked = wire.call("app.state")["document"]["history"]["undo_depth"]
    wire.call("tool.stroke", inside_stroke)
    through = wire.call("doc.get", {})["checksum"]
    print(f"selection holds paint: outside {blocked[:12] == held[:12]}, through {through[:12]} "
          f"({depth_held} -> {depth_blocked} -> "
          f"{wire.call('app.state')['document']['history']['undo_depth']} steps)")
    if blocked != held:
        fails.append("a stroke outside the selection changed the document")
    if depth_blocked != depth_held:
        fails.append("a stroke the selection stopped still cost an undo step")
    if through == held:
        fails.append("a stroke through the selection changed nothing, so the check proved nothing")
    wire.call("edit.undo", {})
    if wire.call("doc.get", {})["checksum"] != held:
        fails.append("Ctrl+Z did not put the clipped stroke back")

    # Ctrl+I twice is the same selection; Ctrl+D puts it down, and the paint that was held back then
    # goes through - which is the only way to tell a working deselect from a stroke that paints
    # nothing at all.
    inj.modified_key(KEY_I, KEY_CTRL_L)
    wd.settle(wire)
    inverted = wire.call("select.get", {})
    if inverted["covered"] + sel["covered"] != canvas_area:
        fails.append(f"inverting covered {inverted['covered']} where the selection held "
                     f"{sel['covered']} of {canvas_area}")
    inj.modified_key(KEY_I, KEY_CTRL_L)
    wd.settle(wire)
    if wire.call("select.get", {})["covered"] != sel["covered"]:
        fails.append("Ctrl+I twice did not come back to the selection that was drawn")
    inj.modified_key(KEY_D, KEY_CTRL_L)
    wd.settle(wire)
    cleared = wire.call("select.get", {})
    print(f"Ctrl+D: active {sel['active']} -> {cleared['active']}, covered {cleared['covered']}")
    if cleared["active"]:
        fails.append("Ctrl+D left the selection up")
    wire.call("tool.stroke", outside_stroke)
    if wire.call("doc.get", {})["checksum"] == held:
        fails.append("with no selection the stroke outside the old rectangle still painted nothing")
    wire.call("edit.undo", {})

    # The mask conversions, after a marquee drawn by the pointer: the composite changes, the layer's
    # own bytes do not, and the plane arriving is exactly one undo step - one that undo takes off again,
    # back to the bytes the canvas was showing. The composite is only visible through a written image,
    # so that is what is compared - hashed, because "different" has to mean different bytes rather than
    # a different filename.
    # `layer.list` is the command that reports a layer's own pixel digest, which is the claim being
    # made here: the document digest counts mask bytes on purpose, so comparing that would say
    # "something changed" without saying whether the pixels moved.
    def view() -> tuple[str, str]:
        listed = wire.call("layer.list", {})
        active = listed["layers"][listed["active"]]
        return active["checksum"], listed["composite_checksum"]

    wire.call("select.rect", {"rect": [6, 4, 18, 12], "mode": "replace"})
    pixels_before, shown_before = view()
    steps_before = wire.call("app.state")["document"]["history"]["undo_depth"]
    masked = wire.call("mask.from_selection", {})
    pixels_after, shown_masked = view()
    steps_after = wire.call("app.state")["document"]["history"]["undo_depth"]
    print(f"mask.from_selection: {masked['covered']} px shown, composite "
          f"{shown_before[:8]} -> {shown_masked[:8]}, layer bytes unchanged "
          f"{pixels_after == pixels_before}, {steps_before} -> {steps_after} steps")
    if masked["covered"] == 0:
        fails.append("the mask covers nothing, so the composite check proved nothing")
    if shown_before == shown_masked:
        fails.append("the composite is unchanged by a mask, so the mask is not reaching the canvas")
    if pixels_after != pixels_before:
        fails.append("masking edited the layer's pixels; a mask is meant to hide, not to erase")
    if steps_after != steps_before + 1:
        fails.append(f"applying a layer mask cost {steps_after - steps_before} undo steps, expected one")

    # The step holds the plane rather than a pixel, so undoing it returns the composite by taking the
    # mask off - not by repainting anything - and redo hides the layer again.
    wire.call("edit.undo", {})
    undone_pixels, undone_shown = view()
    print(f"edit.undo of the mask: composite {undone_shown[:8]}, back to what was shown "
          f"{undone_shown == shown_before}, layer bytes still unchanged "
          f"{undone_pixels == pixels_before}")
    if undone_shown != shown_before:
        fails.append("undoing the mask did not return the composite it changed")
    if undone_pixels != pixels_before:
        fails.append("undoing the mask moved the layer's own bytes")
    wire.call("edit.redo", {})
    _, redone_shown = view()
    if redone_shown != shown_masked:
        fails.append(f"redoing the mask showed {redone_shown[:8]} instead of {shown_masked[:8]}")

    loaded_back = wire.call("select.from_mask", {})
    if loaded_back["bounds"] != masked["bounds"] or loaded_back["covered"] != masked["covered"]:
        fails.append(f"select.from_mask gave {loaded_back}, expected the mask's {masked}")
    print(f"select.from_mask: bounds {loaded_back['bounds']}, covered {loaded_back['covered']}")

    # A mask thumbnail selects the plane for Brush/Eraser, never Cover/Un-cover or a
    # marquee. The selection survives mask editing independently of the layer pixels.
    wire.call("select.clear", {})
    wire.call("layer.plane", {"plane": "mask"})
    wire.call("mask.fill", {"to": "hide"})

    def mask_view() -> tuple:
        listed = wire.call("layer.list", {})
        active = listed["active"]
        facts = wire.call("mask.get", {})
        return facts, listed["layers"][active]["checksum"], listed["composite_checksum"]

    plane = wire.call("app.state", {})["brush"]["edit_plane"]
    cleared, _, _ = mask_view()
    print(f"mask thumbnail selected: {plane}, initially visible {cleared['mask']['covered']}")
    if plane != "mask":
        fails.append(f"layer plane reported '{plane}' instead of mask")
    if cleared["mask"]["covered"] != 0:
        fails.append("mask.fill(hide) failed")

    # Cover always edits the blue selection, including while the mask thumbnail is active.
    wire.call("tool.set", {"tool": "cover", "size": 14.0, "hardness": 1.0})
    wire.call("tool.stroke", {"points": [[18.0, 17.0]]})
    if wire.call("select.get")["covered"] == 0:
        fails.append("Cover did not mark the AI selection while the mask was active")
    if wire.call("mask.get")["mask"]["covered"] != 0:
        fails.append("Cover changed the layer mask")
    wire.call("select.clear", {})

    wire.call("tool.set", {"tool": "brush", "size": 14.0, "hardness": 1.0})
    doc = wd.document_screen_rect(wire.call("app.state", {}))
    before_facts, layer_before, composite_before = mask_view()
    depth_before = wire.call("app.state", {})["document"]["history"]["undo_depth"]
    inj.move(doc[0] + doc[2] * 0.25, doc[1] + doc[3] * 0.35)
    inj.button(1, True)
    wd.settle(wire)
    inj.move(doc[0] + doc[2] * 0.7, doc[1] + doc[3] * 0.55)
    wd.settle(wire)
    during = wire.call("app.state", {})["stroke"]
    inj.button(1, False)
    wd.settle(wire)
    after_facts, layer_after, composite_after = mask_view()
    depth_after = wire.call("app.state", {})["document"]["history"]["undo_depth"]
    print(f"Brush on mask: plane {during['plane']}, dabs {during['dabs']}, "
          f"visible {before_facts['mask']['covered']} -> {after_facts['mask']['covered']}, "
          f"layer bytes unchanged {layer_after == layer_before}; "
          f"history {depth_before} -> {depth_after}")
    if during["plane"] != "mask":
        fails.append("mask brush gesture targeted the pixel plane")
    if during["dabs"] < 2:
        fails.append("mask brush gesture laid no drag dabs")
    if after_facts["mask"]["covered"] == 0:
        fails.append("mask brush did not reveal anything")
    if layer_after != layer_before:
        fails.append("mask brush modified colour pixels")
    if composite_before == composite_after:
        fails.append("mask brush did not change the composite")
    if depth_after != depth_before + 1:
        fails.append("mask stroke was not one undo step")

    inj.modified_key(KEY_Z, KEY_CTRL_L)
    wd.settle(wire)
    reverted, _, _ = mask_view()
    if reverted["mask"]["covered"] != 0:
        fails.append("undo did not restore the hidden mask")

    # Brush retains its meaning when selected through its keyboard shortcut, and the
    # thumbnail - not the tool - determines whether it edits colour or visibility.
    inj.key(KEY_B)
    wd.settle(wire)
    _, pixels_before_brush, _ = mask_view()
    inj.move(doc[0] + doc[2] * 0.3, doc[1] + doc[3] * 0.3)
    inj.button(1, True)
    inj.move(doc[0] + doc[2] * 0.6, doc[1] + doc[3] * 0.5)
    wd.settle(wire)
    inj.button(1, False)
    wd.settle(wire)
    painted_mask, pixels_after_brush, _ = mask_view()
    if painted_mask["mask"]["covered"] == 0:
        fails.append("Brush shortcut failed to edit the selected mask")
    if pixels_before_brush != pixels_after_brush:
        fails.append("Brush shortcut changed pixels while the mask was active")

    wire.call("layer.plane", {"plane": "pixels"})
    wire.call("select.clear", {})
    pixels_before_colour, _ = view()
    inj.move(doc[0] + doc[2] * 0.5, doc[1] + doc[3] * 0.5)
    inj.button(1, True)
    wd.settle(wire)
    inj.button(1, False)
    wd.settle(wire)
    pixels_after_colour, _ = view()
    if pixels_before_colour == pixels_after_colour:
        fails.append("Brush did not edit image pixels after selecting the image thumbnail")
    if wire.call("mask.get")["mask"]["covered"] != painted_mask["mask"]["covered"]:
        fails.append("image Brush unexpectedly changed layer mask")

    wire.call("mask.remove", {})
    try:
        wire.call("layer.plane", {"plane": "mask"})
        fails.append("a non-existent mask thumbnail was selected")
    except wd.WireError as why:
        if "has no mask" not in str(why):
            fails.append(f"missing-mask refusal was unclear: {why}")
    wire.call("tool.set", {"tool": "cover"})
    wire.call("tool.stroke", {"points": [[10.0, 10.0]]})
    if wire.call("select.get")["covered"] == 0:
        fails.append("Cover stopped working when the layer mask was removed")
    wire.call("select.clear", {})

    # The step keys, through the real key path. `,`/`.` move hardness by one step and Shift takes
    # the same two keys to opacity - which only a real modifier state can demonstrate, because the
    # keysym is the same either way and a dropped Shift would look like a working key.
    def brush_of() -> dict:
        return wire.call("app.state")["brush"]

    start = brush_of()
    inj.key(KEY_COMMA)
    wd.settle(wire)
    softened = brush_of()
    step = start["hardness"] - softened["hardness"]
    print(f"`,`: hardness {start['hardness']:.3f} -> {softened['hardness']:.3f} "
          f"(one step of {BRUSH_STEP}), opacity stayed {softened['opacity']:.3f}")
    if not 0.5 * BRUSH_STEP <= step <= 1.5 * BRUSH_STEP:
        fails.append(f"`,` moved hardness by {step:.4f}, not one step of {BRUSH_STEP}")
    if abs(softened["opacity"] - start["opacity"]) > 1e-9:
        fails.append("`,` moved opacity, which belongs to Shift+`,`")
    inj.key(KEY_PERIOD)
    wd.settle(wire)
    back = brush_of()
    if abs(back["hardness"] - start["hardness"]) > 1e-9:
        fails.append(f"`.` did not undo `,`: {softened['hardness']:.3f} -> {back['hardness']:.3f}")
    inj.modified_key(KEY_COMMA, KEY_SHIFT_L)
    wd.settle(wire)
    shifted = brush_of()
    print(f"Shift+`,`: opacity {back['opacity']:.3f} -> {shifted['opacity']:.3f}, hardness stayed "
          f"{shifted['hardness']:.3f}")
    if abs(back["opacity"] - shifted["opacity"]) < 0.5 * BRUSH_STEP:
        fails.append("Shift+`,` did not lower opacity by a step")
    if abs(shifted["hardness"] - back["hardness"]) > 1e-9:
        fails.append("Shift+`,` moved hardness: the modifier reached the wrong command")

    # And a key bound to nothing changes nothing.
    before = wire.call("app.state")["view"]
    inj.key(KEY_J)
    wd.settle(wire)
    after = wire.call("app.state")["view"]
    if [before["scale"], *before["origin"]] != [after["scale"], *after["origin"]]:
        fails.append("an unbound key moved the view")

    # The flagship key, through the host's own event handling: a marquee dragged with the mouse, then
    # Ctrl+Backspace, and the job that appears is the Remove policy - no command typed by the driver.
    # Landing is not claimed here: rule A says the finished answer should reach a new layer with nothing
    # pressed in between, and the queue has no app-level beat that does that yet (docs/PLAN.md, "What
    # rule A still owes").
    wire.call("tool.select", {})
    wd.settle(wire)
    inj.move(doc[0] + doc[2] * 0.25, doc[1] + doc[3] * 0.25)
    inj.button(1, True)
    inj.move(doc[0] + doc[2] * 0.55, doc[1] + doc[3] * 0.60)
    inj.button(1, False)
    wd.settle(wire)
    marked = wire.call("select.get")
    print(f"dragged marquee: active={marked['active']} bounds={marked['bounds']} "
          f"covered={marked['covered']}")
    if not marked["active"]:
        fails.append("the dragged marquee made no selection")

    inj.modified_key(KEY_BACKSPACE, KEY_CTRL_L)
    wd.settle(wire)
    deadline = time.monotonic() + 20.0
    jobs: list[dict] = []
    while time.monotonic() < deadline:
        jobs = wire.call("job.list")["jobs"]
        if jobs and jobs[-1]["state"] in {"succeeded", "failed", "cancelled"}:
            break
        time.sleep(0.2)
    if not jobs:
        intent = wire.call("ai.intent", {})
        print("   no job: verbs say " + repr([
            (row["id"], row["ready"], row["reason"]) for row in wire.call("ai.verbs")["verbs"]]))
        print("   intent: connected=" + repr(intent.get("connected")) +
              " configured=" + repr(intent.get("configured")) +
              " error=" + repr(intent.get("error")) + " reason=" + repr(intent.get("reason")))
        fails.append("Ctrl+Backspace started no job")
    else:
        job = jobs[-1]
        print(f"Ctrl+Backspace -> job {job['id']} recipe={job.get('recipe')} state={job['state']}")
        if job.get("recipe") != "inpaint-remove":
            fails.append(f"the flagship key ran {job.get('recipe')}, not inpaint-remove")
        if job["state"] == "failed" and "fixture" not in (job.get("failure") or "") + str(
                job.get("error", "")):
            # A recording miss is a pass here: the key reached the recipe, the region compiler and the
            # transport, which is what an injected key can prove. Any other failure is a real one.
            fails.append(f"the key's job failed for a reason other than the recording: {job.get('failure')}")
        else:
            # And rule A, end to end through real events: nothing else was pressed, and the answer is a
            # layer of its own named for the verb. `Ctrl+Z` then takes the layer and its pixels away
            # together, which is the undo a person would expect from one keystroke.
            layers = wire.call("layer.list")["layers"]
            print(f"landed without a press: {[layer['name'] for layer in layers]}")
            if len(layers) < 2:
                fails.append("the answer did not arrive as a new layer")
            elif not str(layers[-1]["name"]).startswith("Remove "):
                fails.append(f"the layer is named {layers[-1]['name']!r}, not 'Remove <job>'")
            else:
                inj.modified_key(KEY_Z, KEY_CTRL_L)
                wd.settle(wire)
                after = wire.call("layer.list")["layers"]
                print(f"Ctrl+Z: {[layer['name'] for layer in after]}")
                if len(after) != len(layers) - 1:
                    fails.append("Ctrl+Z did not take the answer's layer away")

    # Escape is bound to app.quit, and unsaved work now holds it. Three claims, in the order a person
    # meets them: the key leaves the app running with the question standing, a second Escape withdraws
    # it, and only the forced answer - what the dialog's `Discard` presses - ends the run.
    inj.key(KEY_ESCAPE)
    wd.settle(wire)
    if wire.call("app.state")["quit_pending"] is not True:
        fails.append("Escape over unsaved work did not hold the quit")
    else:
        print("Escape over unsaved work: held, quit_pending true")
    inj.key(KEY_ESCAPE)
    wd.settle(wire)
    if wire.call("app.state")["quit_pending"] is not False:
        fails.append("a second Escape left the quit pending")
    else:
        print("Escape again: withdrawn, still running")
    wire.call("app.quit", {"force": True})
    deadline = time.monotonic() + 10.0
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    if proc.poll() is None:
        fails.append("a forced app.quit did not end the app")
    else:
        print("app.quit {force}: the app left through the table")
    return fails


def run(args: argparse.Namespace) -> int:
    runtime = Path(tempfile.mkdtemp(prefix="noise-input-"))
    fixture = runtime / "fixture.png"
    wd.write_fixture_png(fixture, *wd.FIXTURE_SIZE)
    try:
        with Display() as display:
            env = dict(os.environ)
            env["DISPLAY"] = display
            env["NOISE_RUNTIME_DIR"] = str(runtime)
            config_dir = runtime / "config"
            config_dir.mkdir(parents=True, exist_ok=True)
            env["NOISE_CONFIG_DIR"] = str(config_dir)
            # The verb-key section runs the AI path, and it must run it against the recordings even on a
            # shell that exports the dev-box credentials: the recorded transport answers before a socket
            # is opened, and this test spends no requests.
            root = Path(__file__).resolve().parent.parent
            env["NOISE_EDIT_ASSETS"] = str(root / "assets")
            env["NOISE_AI_FIXTURES"] = str(root / "tests" / "fixtures" / "http")
            env.pop("THENOISE_URL", None)
            env.pop("THENOISE_API_KEY", None)
            with (runtime / "app-stderr.log").open("w") as app_stderr:
                proc = wd.register_child(subprocess.Popen(
                    [str(args.bin), f"--width={WIDTH}", f"--height={HEIGHT}",
                     "--debug-wire=0", str(fixture)], env=env, stderr=app_stderr,
                    start_new_session=True))
                wire = None
                fails: list[str] = []
                try:
                    wire = wd.Wire(wd.wire_port(runtime, proc, args.startup_timeout))
                    inj = Injector(display)
                    inj.focus_app_window(WIDTH, HEIGHT)
                    try:
                        fails = drive(wire, inj, proc, fixture)
                    finally:
                        inj.close()
                except wd.WireError as exc:
                    fails = [str(exc)]
                finally:
                    wd.shutdown(proc, wire)
            for line in fails:
                print(f"FAIL: {line}")
            return 1 if fails else 0
    except Skip as skip:
        print(f"SKIP: {skip}")
        return 0
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, help="path to noise-edit")
    parser.add_argument("--startup-timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not args.bin.exists():
        print(f"FAIL: no binary at {args.bin}")
        return 1
    try:
        ctypes.CDLL("libXtst.so.6")
    except OSError:
        print("SKIP: libXtst is not available, so input cannot be injected")
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
