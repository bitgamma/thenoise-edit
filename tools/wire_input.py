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
import shutil
import os
import socket
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
        for _ in range(clicks):
            self.button(4, True)   # wheel buttons are 4 (up) and 5 (down) on X11
            self.button(4, False)

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


def free_display() -> str:
    for number in range(70, 100):
        if not os.path.exists(f"/tmp/.X{number}-lock"):
            try:
                with socket.socket(socket.AF_UNIX) as probe:
                    probe.connect(f"\0/tmp/.X11-unix/X{number}")
            except OSError:
                return f":{number}"
    raise wd.WireError("no free X display number in 70..99")


class Display:
    """The display the app and the injector share: ours only if we had to start one."""

    def __enter__(self):
        if os.environ.get("DISPLAY"):
            self.proc = None
            return os.environ["DISPLAY"]
        if not shutil_which("Xvfb"):
            raise Skip("no DISPLAY and no Xvfb to make one")
        self.number = free_display()
        self.proc = subprocess.Popen(
            ["Xvfb", self.number, "-screen", "0", f"{WIDTH}x{HEIGHT}x24"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 15.0
        while not os.path.exists(f"/tmp/.X{self.number[1:]}-lock"):
            if time.monotonic() > deadline or self.proc.poll() is not None:
                raise Skip("Xvfb did not come up")
            time.sleep(0.05)
        return self.number

    def __exit__(self, *_exc):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
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
KEY_V = 0x0076         # XK_v
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

    # Escape is bound to app.quit, so the app must actually leave: the last thing a driver
    # can check this way, and the one that proves quitting runs through the table.
    inj.key(KEY_ESCAPE)
    deadline = time.monotonic() + 10.0
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    if proc.poll() is None:
        fails.append("Escape did not quit the app")
    else:
        print("Escape: the app quit through app.quit")
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
            env.pop("NOISE_CONFIG_DIR", None)
            with (runtime / "app-stderr.log").open("w") as app_stderr:
                proc = subprocess.Popen(
                    [str(args.bin), f"--width={WIDTH}", f"--height={HEIGHT}",
                     "--debug-wire=0", str(fixture)], env=env, stderr=app_stderr)
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
