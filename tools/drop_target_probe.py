#!/usr/bin/env python3
"""Does the desktop know this window takes files, and can a driver do what a drop does?

The drop itself is XDND, which needs a second program dragging at a real pointer, so this checks the two
halves that are checkable headless: the window advertises `XdndAware` (without it a file manager refuses
the drag before the app is ever asked), and `layer.import` is in the command table, so the wire can land
the same picture the pointer dropped.
"""

# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_drive as wd  # noqa: E402

WIDTH, HEIGHT = 900, 620


class X11:
    """Just enough libX11 to read one property off the editor's window."""

    def __init__(self, display: str) -> None:
        self.lib = ctypes.CDLL("libX11.so.6")
        self.lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.lib.XOpenDisplay.restype = ctypes.c_void_p
        self.lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.lib.XDefaultRootWindow.restype = ctypes.c_ulong
        self.lib.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                        ctypes.POINTER(ctypes.c_ulong),
                                        ctypes.POINTER(ctypes.c_ulong),
                                        ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                                        ctypes.POINTER(ctypes.c_uint)]
        self.lib.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                        ctypes.POINTER(ctypes.c_char_p)]
        self.lib.XFetchName.restype = ctypes.c_int
        self.lib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        self.lib.XInternAtom.restype = ctypes.c_ulong
        self.lib.XGetAtomName.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.lib.XGetAtomName.restype = ctypes.c_char_p
        self.lib.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                                               ctypes.c_long, ctypes.c_long, ctypes.c_int,
                                               ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                               ctypes.POINTER(ctypes.c_int),
                                               ctypes.POINTER(ctypes.c_ulong),
                                               ctypes.POINTER(ctypes.c_ulong),
                                               ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
        self.lib.XFree.argtypes = [ctypes.c_void_p]
        self.dpy = self.lib.XOpenDisplay(display.encode() if display else None)
        if not self.dpy:
            raise wd.WireError(f"cannot open display {display!r}")

    def window_ending_with(self, suffix: str) -> int:
        found = []

        def walk(window: int, depth: int) -> None:
            if depth > 4 or found:
                return
            root = ctypes.c_ulong()
            parent = ctypes.c_ulong()
            kids = ctypes.POINTER(ctypes.c_ulong)()
            count = ctypes.c_uint()
            if not self.lib.XQueryTree(self.dpy, window, ctypes.byref(root), ctypes.byref(parent),
                                       ctypes.byref(kids), ctypes.byref(count)):
                return
            names = []
            for i in range(count.value):
                name = ctypes.c_char_p()
                if self.lib.XFetchName(self.dpy, kids[i], ctypes.byref(name)) and name.value:
                    names.append(name.value.decode(errors="replace"))
                    if name.value.decode(errors="replace").endswith(suffix):
                        found.append(kids[i])
                walk(kids[i], depth + 1)
            if kids:
                self.lib.XFree(kids)

        walk(self.lib.XDefaultRootWindow(self.dpy), 0)
        if not found:
            raise wd.WireError(f"no window whose title ends in {suffix!r} on this display")
        return int(found[0])

    def longs(self, window: int, property_name: str) -> list[int]:
        """The 32-bit property `property_name`, as numbers: `XdndAware` holds a version, not an atom."""
        actual_type = ctypes.c_ulong()
        fmt = ctypes.c_int()
        items = ctypes.c_ulong()
        after = ctypes.c_ulong()
        data = ctypes.POINTER(ctypes.c_ubyte)()
        any_type = self.lib.XInternAtom(self.dpy, b"ANY", 1)
        got = self.lib.XGetWindowProperty(
            self.dpy, window, self.lib.XInternAtom(self.dpy, property_name.encode(), 0),
            0, 64, 0, any_type, ctypes.byref(actual_type), ctypes.byref(fmt),
            ctypes.byref(items), ctypes.byref(after), ctypes.byref(data))
        if got or not data or fmt.value != 32:
            return []
        # A format-32 property is packed one `long` per item, whatever the client's wordsize.
        values = [int(ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[i])
                  for i in range(items.value)]
        self.lib.XFree(data)
        return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="build/noise-edit")
    args = parser.parse_args()
    args.bin = pathlib.Path(args.bin)
    reason = None if args.bin.exists() else f"no binary at {args.bin}"
    if not reason:
        reason = wd.display_skip()
    if reason:
        print(f"SKIP: {reason}")
        return 0
    try:
        display = wd.ensure_display(WIDTH, HEIGHT)
    except wd.WireError as exc:
        print(f"SKIP: {exc}")
        return 0

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="drop-target-"))
    runtime = tmp / "runtime"
    runtime.mkdir()
    print("the window as the desktop sees it: an Xdnd target, and a command for the same landing")
    proc = wd.start_app(args, runtime, WIDTH, HEIGHT, None,
                        open(tmp / "app.stderr", "wb"), None)
    failed = []

    def check(ok: bool, claim: str, detail: str = "") -> None:
        print(f"  {'ok  ' if ok else 'FAIL'} {claim}" + (f"  {detail}" if detail else ""))
        if not ok:
            failed.append(claim)

    wire = None
    try:
        wire = wd.Wire(wd.wire_port(runtime, proc, 60.0))
        wd.wait_for_fit(wire)
        names = {c["id"] for c in wire.call("command.list")["commands"]}
        check("layer.import" in names, "a driver can land the picture a drop lands")

        x11 = X11(display)
        # The title names the document (`untitled - noise-edit`, `first.noise * - noise-edit`), so the
        # search is on the part that never changes.
        window = x11.window_ending_with("noise-edit")
        aware = x11.longs(window, "XdndAware")
        check(aware == [5], "the window advertises itself to the desktop as an XDND 5 target",
              f"XdndAware {aware}")
    except wd.WireError as exc:
        check(False, "the editor answered", f"{exc}")
    finally:
        wd.shutdown(proc, wire)
    print("FAILURES" if failed else "all claims held", f"({tmp})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
