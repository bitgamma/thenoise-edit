# Plan — where M0 ended and what comes next

Working note for the next session. Facts below were observed, not assumed; re-check any
that matter before building on them. Refresh or delete this file as milestones close.

## State at the M0 handoff

`cmake -B build -G Ninja && cmake --build build && ctest --test-dir build` → 7/7 green,
zero warnings with `-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion`.

| Suite | Covers |
|---|---|
| `test_hash` | sha256, base64 encode |
| `test_geom` | view round-trip, anchor-fixed zoom, reversibility, fit containment, origin clamp reachability, rect clip |
| `test_image` | PNG round-trip incl. alpha, decode rejection, region/blit/fill clipping |
| `test_command` | table registration, dispatch, unknown-id error |
| `test_wire` | request/response, multi-frame writes, malformed input, throw mapping, frame bound, token gate, bind errors, stop |
| `test_app` | command surface vs `ARCHITECTURE.md`, `command.invoke`, arg validation, doc disk round-trip, `image.get` pixels, `pixels.compare` single byte, `screenshot` vs capture, view invariants |
| `test_view_golden` | live app over the wire: fit → zoom → pan → canvas capture vs `tests/golden/view_pan_zoom.png` |

Wire commands live now: `app.state` `app.quit` `command.list` `command.invoke` `doc.new`
`doc.open` `doc.save` `doc.get` `image.get` `screenshot` `pixels.compare` `log.tail`
`log.level` `view.fit` `view.set` `view.pan` `view.zoom` `wire.info`.

Measured on this box (Xvfb + llvmpipe, no GPU): 79 fps at 1200×760 with a 1600×1000
document, median frame 5.7 ms, p95 7.8 ms. The golden is byte-exact across runs
(`differing_bytes=0`); tolerance constants in `tools/wire_drive.py` exist only for
cross-driver rasterization, not for slop.

## Next steps, in order

1. **Run CI once for real.** The workflow has never executed (nothing pushed). Expect the
   golden to be the thing that fails on Ubuntu's llvmpipe. If it does: read the reported
   `differing_bytes` / `max_channel_delta`; if the diff is rasterization noise, either widen
   the two constants in `tools/wire_drive.py` deliberately or re-record with
   `python3 tools/wire_drive.py --bin build/noise-edit --golden tests/golden/view_pan_zoom.png --update-golden`
   on the runner and commit that golden. Never widen the tolerance to hide a structural diff.
2. **Keyboard shortcuts into the command table** (`AGENTS.md` §4 puts commands *and* shortcuts
   in one home; there is no shortcut field yet). Add the field to `CommandTable::add`, dispatch
   from `main.cpp`'s `event()` through the table, seed MVP.md's key list (`V B E M L W G I [ ] , . Space Enter Ctrl+D/I/J/E/Z/Y/S F`),
   and expose the mapping on the wire via `command.list` so it is checkable.
3. **M1 paint core.** Acceptance (MVP.md): *5000-dab stroke at 2048² without dropped frames,
   20× undo*. Slices, each with its own wire command and test:
   a. **Blob store + undo**: content-addressed by the existing `sha256_hex`, undo/redo as pointer
      swaps. Test: undo after N mutations returns byte-identical images, whole-image diff.
   b. **Dirty-rect texture upload**: `gfx::Texture::sync` currently recreates the whole image on
      any revision change — fine for load, unusable for painting (MVP risk 9). Put the upload
      path behind an accessor taking dirty rects, and keep the whole-image path as one case.
      Test: a 1-px edit uploads one rect, checksum still matches CPU pixels.
   c. **`tool.stroke {points, tool, size, hardness, opacity, flow}`** driving the real painting
      path (spec'd in `ARCHITECTURE.md`'s wire table), so painting is testable without a mouse.
      Brush math (hardness/flow falloff, stroke interpolation) lives in `core/`, CPU-side, with a
      reference implementation a test can compare against.
   d. **Stroke timing test**: 5000 dabs at 2048² through `tool.stroke`, assert no dropped frames
      using `app.state.frame` / `frame_ms` — the observation primitive already exists.
   e. **`.noise` v0**: read `ARCHITECTURE.md`'s file-format section first (single-file zip,
      `manifest.json:format`, blobs by hash). Round-trip test: save → load → identical checksums
      and identical undo history length. Older `format` values must be refused with a message
      naming the version found.
   f. Status bar already reports zoom/size/path/frame-ms/wire; add tool + brush readout.
4. **Close the small honest gaps** left in M0, when convenient:
   * `--headless` (`SG_BACKEND_NULL` + CPU composite) from the wire spec is not implemented; CI
     uses Xvfb + llvmpipe instead. Until it exists, keep the `ARCHITECTURE.md` note honest.
   * The wire token (`WireConfig::token`) is honoured by the server but the app never generates
     one or writes the 0600 file. Loopback-only for now.
   * `src/app/commands.cpp` is ~470 lines: split it by group (`doc.*`, `view.*`, pixels, app/log)
     when `layer.*` lands, not before.
5. **M0.5 probe harness** (`tools/tn_probe.py`, `docs/probes/`) — blocked on the dev server.
   Ask the human to export `THENOISE_URL` / `THENOISE_API_KEY`; it feeds `size_policy`,
   role default order, ETA seeds and recipe wording, so it should land before M3 designs
   hard-code any of those.

## Environment notes for this machine

* No `DISPLAY`; a Wayland var may be set with no compositor. Render through `xvfb-run -a` —
  `tools/wire_drive.py` does this itself and skips with a loud reason if neither is available.
* `XDG_RUNTIME_DIR=/run/user/1000` is **not writable** here (owned by `nobody`, mode `------`).
  Anything writing runtime files needs `NOISE_RUNTIME_DIR` pointed somewhere writable;
  `test_app` injects its own dir and `wire_drive.py` uses a temp dir, so the suites are immune.
* Killed runs can leave Xvfb processes holding a ctest output pipe, which looks like a test
  hang. `pkill -x Xvfb` (the `-x` matters: `pkill -f Xvfb` matches your own shell).
* Useful one-liners:
  ```
  cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
  ./build/tests/test_app                       # windowless suites run standalone
  NOISE_RUNTIME_DIR=/tmp/nr ./build/noise-edit --width=1000 --height=620 some.png   # under xvfb-run
  ```
