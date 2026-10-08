# SPDX-License-Identifier: Apache-2.0
"""Debug-wire client and viewport golden test (M0 acceptance).

Boots the real app, drives view commands over the loopback wire the same way a UI
action does, captures the canvas and diffs the capture against a committed golden.

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


def app_command(args: argparse.Namespace, width: int, height: int) -> list[str]:
    app = [
        str(args.bin),
        f"--width={width}",
        f"--height={height}",
        "--debug-wire=0",
    ]
    if shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        return ["xvfb-run", "-a", "--server-args", f"-screen 0 {width}x{height}x24", *app]
    return app


def start_app(args: argparse.Namespace, runtime_dir: Path, width: int, height: int) -> subprocess.Popen:
    env = dict(os.environ)
    env["NOISE_RUNTIME_DIR"] = str(runtime_dir)
    env.pop("NOISE_CONFIG_DIR", None)
    return subprocess.Popen(app_command(args, width, height), env=env)


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
    scale = state["view"]["scale"]
    origin_x, origin_y = state["view"]["origin"]
    image_w, image_h = state["view"]["image"]

    left = ox + (0.0 - origin_x) * scale
    top = oy + (0.0 - origin_y) * scale
    x0 = max(int(left) - CAPTURE_MARGIN, int(ox))
    y0 = max(int(top) - CAPTURE_MARGIN, int(oy))
    x1 = min(int(left + image_w * scale) + CAPTURE_MARGIN, int(ox + cw))
    y1 = min(int(top + image_h * scale) + CAPTURE_MARGIN, int(oy + ch))
    if x1 - x0 < 16 or y1 - y0 < 16:
        raise WireError(f"capture region {x0},{y0},{x1},{y1} is too small to mean anything")

    shot = wire.call("screenshot", {"region": [x0, y0, x1 - x0, y1 - y0], "name": name})
    shot["region"] = [x0, y0, x1 - x0, y1 - y0]
    return shot


def settle(wire: Wire, frames: int = FRAME_SETTLE) -> None:
    target = wire.call("app.state")["frame"] + frames
    while True:
        if wire.call("app.state")["frame"] >= target:
            return
        time.sleep(0.005)


def run(args: argparse.Namespace) -> int:
    width, height = args.width, args.height
    args.golden.parent.mkdir(parents=True, exist_ok=True)
    runtime_dir = Path(tempfile.mkdtemp(prefix="noise-golden-"))
    fixture = runtime_dir / "fixture.png"
    write_fixture_png(fixture, *FIXTURE_SIZE)

    proc = start_app(args, runtime_dir, width, height)
    wire = None
    try:
        wire = Wire(wire_port(runtime_dir, proc, args.startup_timeout))
        ids = {entry["id"] for entry in wire.call("command.list")["commands"]}
        missing = {"doc.open", "view.fit", "view.zoom", "view.pan", "screenshot"} - ids
        if missing:
            raise WireError(f"wire reached the app but the table lacks {sorted(missing)}")

        wire.call("doc.open", {"path": str(fixture)})
        settle(wire)
        fitted = capture_canvas(wire, "fitted")
        wire.call("view.zoom", {"factor": 1.21, "anchor": [width * 0.38, height * 0.42]})
        settle(wire)
        zoomed = capture_canvas(wire, "zoomed")
        wire.call("view.pan", {"dx": -23, "dy": 17})
        settle(wire)
        panned = capture_canvas(wire, "panned")
        state = wire.call("app.state")

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
        print(f"diff    differing_bytes={check['differing_bytes']} (budget {budget}) "
              f"max_channel_delta={check['max_channel_delta']} (max {TOLERANCE_MAX_DELTA})")
        if check["differing_bytes"] <= budget and check["max_channel_delta"] <= TOLERANCE_MAX_DELTA:
            print("PASS: viewport capture matches the golden")
            return 0
        print(f"FAIL: viewport capture diverged from {args.golden}")
        return 1
    finally:
        shutdown(proc, wire)
        shutil.rmtree(runtime_dir, ignore_errors=True)


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
