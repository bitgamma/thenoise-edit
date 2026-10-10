# SPDX-License-Identifier: Apache-2.0
"""Debug-wire client and viewport golden test (M0 acceptance).

Boots the real app on a document named on its command line, checks that the startup
view fitted into the canvas area, then drives view commands over the loopback wire the
same way a UI action does, captures the canvas and diffs the capture against a
committed golden.

Run through ctest (`test_view_golden`) or directly:

    python3 tools/wire_drive.py --bin build/noise-edit --golden tests/golden/view_pan_zoom.png

Skips loudly (exit 0, "SKIP:" on stdout) when there is neither a display nor
xvfb-run: a missing GPU/display is an unavailable resource, not a regression.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

FIXTURE_SIZE = (137, 89)  # odd on purpose: resampling artefacts must show up
CAPTURE_MARGIN = 12
FRAME_SETTLE = 3
# A frame this close to simgui's vertex buffer has effectively started dropping draw
# commands (it refuses the whole overflowed buffer, so the screen loses pieces).
VERTEX_HEADROOM = 0.75
FIT_WAIT_TIMEOUT = 10.0
FIT_SLACK_PX = 1.0
# Zooming multiplies any canvas geometry that is generated per screen element (the canvas
# rect grows, so `rects = canvas_area / square^2` grows with it) while chrome and text stay
# put. Four steps of zoom is enough to separate the two without costing much either way.
GEOMETRY_ZOOM = 4.0
GEOMETRY_ZOOM_TOLERANCE = 1.5
GEOMETRY_SLACK_VERTICES = 200
# Software rasterizers round texture filtering differently from hardware ones; a
# golden capture is only portable across drivers within a small band. Anything
# structural (wrong crop, wrong scale, blank frame) is orders of magnitude worse.
TOLERANCE_MAX_DELTA = 6
TOLERANCE_DIFFERING_FRACTION = 0.005


class WireError(RuntimeError):
    pass


class Wire:
    """JSON-lines client for the debug wire: one object per request and response."""

    def __init__(self, port: int, timeout: float = 15.0) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buffer = b""
        self.next_id = 1

    def close(self) -> None:
        self.sock.close()

    def call(self, cmd: str, args: dict | None = None) -> dict:
        request: dict = {"id": self.next_id, "cmd": cmd}
        self.next_id += 1
        if args is not None:
            request["args"] = args
        self.sock.sendall((json.dumps(request) + "\n").encode())
        while True:
            reply = json.loads(self._read_frame())
            if reply.get("id") == request["id"]:
                if not reply.get("ok", False):
                    raise WireError(f"{cmd}: {reply.get('error', 'no error given')}")
                return reply.get("result", {})

    def _read_frame(self) -> str:
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise WireError("debug wire closed the connection")
            self.buffer += chunk
        frame, self.buffer = self.buffer.split(b"\n", 1)
        return frame.decode()


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def write_fixture_png(path: Path, width: int, height: int) -> None:
    """Deterministic RGBA pattern: hard edges plus transparent pixels, no randomness."""

    def pixel(x: int, y: int) -> bytes:
        block = ((x // 20) + (y // 20)) % 2
        return bytes((
            235 if block else 40,
            (x * 3) & 0xFF,
            (y * 5) & 0xFF,
            0 if (x + y) % 7 == 0 else 255,
        ))

    raw = b"".join(
        b"\x00" + b"".join(pixel(x, y) for x in range(width)) for y in range(height)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


def app_command(args: argparse.Namespace, width: int, height: int, open_path: Path | None) -> list[str]:
    app = [
        str(args.bin),
        f"--width={width}",
        f"--height={height}",
        "--debug-wire=0",
    ]
    if open_path is not None:
        app.append(str(open_path))
    if shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        return ["xvfb-run", "-a", "--server-args", f"-screen 0 {width}x{height}x24", *app]
    return app


def start_app(args: argparse.Namespace, runtime_dir: Path, width: int, height: int,
              open_path: Path | None, app_stderr) -> subprocess.Popen:
    env = dict(os.environ)
    env["NOISE_RUNTIME_DIR"] = str(runtime_dir)
    env.pop("NOISE_CONFIG_DIR", None)
    # The AI tables ship in the tree beside this script, and ctest runs probes from the build directory:
    # without this the app under probe has no `roles.json`, and an AI command would refuse for a reason
    # that is about where it was started from rather than about the thing under test.
    env["NOISE_EDIT_ASSETS"] = str(Path(__file__).resolve().parent.parent / "assets")
    return subprocess.Popen(app_command(args, width, height, open_path), env=env,
                            stderr=app_stderr)


def wire_port(runtime_dir: Path, proc: subprocess.Popen, timeout: float) -> int:
    endpoint = runtime_dir / "wire-port"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise WireError(f"app exited with {proc.returncode} before opening the wire")
        if endpoint.exists():
            text = endpoint.read_text().strip()
            if text:
                return int(text)
        time.sleep(0.02)
    raise WireError("timed out waiting for the debug wire endpoint file")


def capture_canvas(wire: Wire, name: str) -> dict:
    """Screenshot of the document's on-screen rect, so the capture is about the
    view transform and not about panel chrome or the wire port number."""
    state = wire.call("app.state")
    ox, oy, cw, ch = state["canvas"]
    left, top, right, bottom = document_screen_rect(state)

    x0 = max(int(left) - CAPTURE_MARGIN, int(ox))
    y0 = max(int(top) - CAPTURE_MARGIN, int(oy))
    x1 = min(int(right) + CAPTURE_MARGIN, int(ox + cw))
    y1 = min(int(bottom) + CAPTURE_MARGIN, int(oy + ch))
    if x1 - x0 < 16 or y1 - y0 < 16:
        raise WireError(f"capture region {x0},{y0},{x1},{y1} is too small to mean anything")

    shot = wire.call("screenshot", {"region": [x0, y0, x1 - x0, y1 - y0], "name": name})
    shot["region"] = [x0, y0, x1 - x0, y1 - y0]
    return shot


def document_screen_rect(state: dict) -> tuple[float, float, float, float]:
    """The document's rect inside the framebuffer, from the state the app reports."""
    ox, oy = state["canvas"][0], state["canvas"][1]
    scale = state["view"]["scale"]
    origin_x, origin_y = state["view"]["origin"]
    left = ox + (0.0 - origin_x) * scale
    top = oy + (0.0 - origin_y) * scale
    width = state["view"]["image"][0] * scale
    height = state["view"]["image"][1] * scale
    return left, top, left + width, top + height


def field(container: dict, key: str) -> object:
    if key not in container:
        raise WireError(f"app.state has no {key!r}: the binary predates this driver")
    return container[key]


def check_frame_budget(state: dict, label: str) -> None:
    """Geometry the UI demanded for a frame, against the buffer it is staged into.

    Anything that grows with canvas area or content length (one draw element per
    checker square, per log line) crosses the limit and takes the frame down with it,
    which is what this catches.
    """
    vertices, limit = field(state, "ui_vertices"), field(state, "ui_vertex_limit")
    if not limit:
        raise WireError(f"{label}: app.state reported no ui_vertex_limit")
    if vertices > limit * VERTEX_HEADROOM:
        raise WireError(
            f"{label}: frame demanded {vertices} of {limit} UI vertices "
            f"({100.0 * vertices / limit:.0f}%); past the limit simgui drops draw commands"
        )


def wait_for_fit(wire: Wire) -> dict:
    """A fit is applied on the first frame whose canvas area can hold the document."""
    deadline = time.monotonic() + FIT_WAIT_TIMEOUT
    while True:
        state = wire.call("app.state")
        if not field(state["view"], "fit_pending"):
            settle(wire)
            return wire.call("app.state")
        if time.monotonic() > deadline:
            raise WireError("fit never applied: the app never published a canvas area")
        time.sleep(0.02)


def check_fit_holds_document(state: dict, label: str) -> None:
    left, top, right, bottom = document_screen_rect(state)
    ox, oy, cw, ch = state["canvas"]
    if (left < ox - FIT_SLACK_PX or top < oy - FIT_SLACK_PX
            or right > ox + cw + FIT_SLACK_PX or bottom > oy + ch + FIT_SLACK_PX):
        raise WireError(
            f"{label}: fitted document rect {left:.1f},{top:.1f},{right:.1f},{bottom:.1f} "
            f"overflows the canvas area {ox:.1f},{oy:.1f},{cw},{ch}"
        )


def check_zoom_does_not_grow_geometry(wire: Wire) -> None:
    """How big the document is on screen must not change what the frame asks for.

    This is the shape of the bug that blanked the canvas: the checker was one rect per
    screen square, so the canvas rect growing with zoom grew the vertex count with it,
    until simgui rejected the frame and nothing was drawn.
    """
    state = wire.call("app.state")
    base = field(state, "ui_vertices")
    wire.call("view.set", {"scale": state["view"]["scale"] * GEOMETRY_ZOOM,
                           "origin": [0.0, 0.0]})
    settle(wire)
    grown_state = wire.call("app.state")
    grown = field(grown_state, "ui_vertices")
    check_frame_budget(grown_state, f"zoom x{GEOMETRY_ZOOM}")
    if grown > base * GEOMETRY_ZOOM_TOLERANCE + GEOMETRY_SLACK_VERTICES:
        raise WireError(
            f"zooming x{GEOMETRY_ZOOM} took the frame from {base} to {grown} UI vertices: "
            "canvas geometry must not be generated per screen element"
        )
    print(f"geometry {base} -> {grown} vertices across x{GEOMETRY_ZOOM} zoom")


def logged_overflows(path: Path) -> int:
    """sokol reports a dropped frame on stderr only, so this is the direct read of the
    symptom the vertex budget exists to keep away."""
    return path.read_text(errors="replace").count("BUFFER_OVERFLOW")


def settle(wire: Wire, frames: int = FRAME_SETTLE) -> None:
    target = wire.call("app.state")["frame"] + frames
    while True:
        if wire.call("app.state")["frame"] >= target:
            return
        time.sleep(0.005)


def drive(wire: Wire, args: argparse.Namespace, width: int, height: int, fixture: Path) -> int:
    ids = {entry["id"] for entry in wire.call("command.list")["commands"]}
    missing = {"doc.open", "view.fit", "view.zoom", "view.pan", "screenshot"} - ids
    if missing:
        raise WireError(f"wire reached the app but the table lacks {sorted(missing)}")

    # The document named on the command line is loaded before the first frame, when the
    # only extent on hand is the framebuffer: its fit must wait for the canvas area.
    startup = wait_for_fit(wire)
    check_frame_budget(startup, "startup open")
    check_fit_holds_document(startup, "startup open")

    wire.call("doc.open", {"path": str(fixture)})
    opened = wait_for_fit(wire)
    check_frame_budget(opened, "doc.open")
    check_fit_holds_document(opened, "doc.open")
    fitted = capture_canvas(wire, "fitted")
    wire.call("view.zoom", {"factor": 1.21, "anchor": [width * 0.38, height * 0.42]})
    settle(wire)
    zoomed = capture_canvas(wire, "zoomed")
    check_frame_budget(wire.call("app.state"), "zoom")

    wire.call("view.pan", {"dx": -23, "dy": 17})
    settle(wire)
    panned = capture_canvas(wire, "panned")
    state = wire.call("app.state")
    check_frame_budget(state, "pan")

    # After the captures: it moves the view around.
    check_zoom_does_not_grow_geometry(wire)

    if fitted["sha256"] == zoomed["sha256"] or zoomed["sha256"] == panned["sha256"]:
        raise WireError(
            "pan/zoom did not change the captured pixels: "
            f"{fitted['sha256'][:12]} {zoomed['sha256'][:12]} {panned['sha256'][:12]}"
        )

    if not args.golden.exists() or args.update_golden:
        shutil.copyfile(panned["path"], args.golden)
        print(f"SKIP: golden written to {args.golden} "
              f"(sha256 {panned['sha256'][:12]}); run again to compare")
        return 0

    check = wire.call("pixels.compare", {"a": panned["path"], "b": str(args.golden)})
    if "differing_bytes" not in check:
        raise WireError(f"cannot compare with the golden: {check.get('reason', check)}")
    budget = int(check["width"] * check["height"] * 4 * TOLERANCE_DIFFERING_FRACTION)
    print(f"capture {panned['path']}")
    print(f"golden  {args.golden}")
    print(f"view    scale {state['view']['scale']:.6f} "
          f"origin {state['view']['origin'][0]:.3f},{state['view']['origin'][1]:.3f}")
    print(f"ui      {state['ui_vertices']} vertices of {state['ui_vertex_limit']} "
          f"({100.0 * state['ui_vertices'] / state['ui_vertex_limit']:.1f}%)")
    print(f"diff    differing_bytes={check['differing_bytes']} (budget {budget}) "
          f"max_channel_delta={check['max_channel_delta']} (max {TOLERANCE_MAX_DELTA})")
    if check["differing_bytes"] <= budget and check["max_channel_delta"] <= TOLERANCE_MAX_DELTA:
        print("PASS: viewport capture matches the golden")
        return 0
    print(f"FAIL: viewport capture diverged from {args.golden}")
    return 1


def app_stderr_tail(path: Path, lines: int = 20) -> str:
    if not path.exists():
        return ""
    kept = path.read_text(errors="replace").splitlines()[-lines:]
    return "\n".join(kept)


def run(args: argparse.Namespace) -> int:
    width, height = args.width, args.height
    args.golden.parent.mkdir(parents=True, exist_ok=True)
    runtime_dir = Path(tempfile.mkdtemp(prefix="noise-golden-"))
    fixture = runtime_dir / "fixture.png"
    write_fixture_png(fixture, *FIXTURE_SIZE)
    err_log = runtime_dir / "app-stderr.log"

    with err_log.open("w") as app_stderr:
        proc = start_app(args, runtime_dir, width, height, fixture, app_stderr)
        wire = None
        status = 1
        try:
            wire = Wire(wire_port(runtime_dir, proc, args.startup_timeout))
            status = drive(wire, args, width, height, fixture)
        except WireError as exc:
            print(f"FAIL: {exc}")
            status = 1
        finally:
            shutdown(proc, wire)
    overflow = logged_overflows(err_log)
    if overflow:
        print(f"FAIL: the app logged {overflow} BUFFER_OVERFLOW lines")
        print(app_stderr_tail(err_log))
        status = 1
    shutil.rmtree(runtime_dir, ignore_errors=True)
    return status


def shutdown(proc: subprocess.Popen, wire: Wire | None) -> None:
    """Quit through the wire so the app runs its own cleanup, then fall back to signals."""
    if wire is not None:
        try:
            wire.call("app.quit")
        except (WireError, OSError):
            pass
        wire.close()
    if proc.poll() is None:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, help="path to noise-edit")
    parser.add_argument("--golden", type=Path, required=True, help="golden PNG to compare with")
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=600)
    parser.add_argument("--startup-timeout", type=float, default=30.0)
    parser.add_argument("--update-golden", action="store_true")
    args = parser.parse_args()

    if not args.bin.exists():
        print(f"FAIL: no binary at {args.bin}")
        return 1
    if not os.environ.get("DISPLAY") and not shutil.which("xvfb-run"):
        print("SKIP: no DISPLAY and no xvfb-run, cannot render a viewport capture")
        return 0
    try:
        return run(args)
    except (WireError, OSError, socket.timeout, TimeoutError) as failure:
        print(f"FAIL: {failure}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
