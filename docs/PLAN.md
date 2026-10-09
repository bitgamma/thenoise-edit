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

## Fixed after the handoff (observed, not assumed)

* **A document blanked the canvas.** The transparency checker was drawn as one ImGui rect
  per visible screen square, with the loop unclipped to the viewport — so its cost grew with
  the canvas rect, not the screen. A 2400x1600 document at fit zoom in a 1280x800 window
  demanded ~124k vertices against simgui's 65536-vertex staging buffer, which rejects the
  frame's draw commands: black canvas plus a `BUFFER_OVERFLOW` line per frame. It is now one
  2x2 texel tile drawn repeat-sampled over the canvas rect (`gfx/checker.{h,cpp}`): four
  vertices at any zoom, measured 598-640 for a whole frame. `simgui.max_vertices` comes from
  `app::kUiVertexLimit` rather than the default.
* **A document opened on the command line never fitted.** `doc.open` fitted against
  `state.viewport`, which before the first laid-out frame is the framebuffer (or 1x1 on the
  frame where the dock has not sized the canvas yet): a 1600x1000 document at 900x600 fitted
  to scale 0.542 — 868 px wide inside a 612 px canvas — and one reproduced open landed on
  `scale = DBL_MIN` with non-finite origin. A fit now stays pending (`view.fit_pending`)
  until a frame publishes a canvas area that can hold the document; the host applies it via
  `app::apply_pending_fit`.
* `app.state` gained `ui_vertices`, `ui_vertex_limit` and `view.fit_pending`.
  `tools/wire_drive.py` boots the app **on a document argument** (so the startup path is the
  one under test), asserts the fitted rect lies inside the canvas, keeps the frame under
  75% of the vertex buffer, fails if the app logged `BUFFER_OVERFLOW` at all, and checks
  that x4 zoom leaves the frame's vertex count where it was (612 -> 620 now; per-element
  canvas geometry multiplies). Red-checked by building with `kUiVertexLimit = 512`:
  `frame demanded 612 of 512 UI vertices (120%)` plus `the app logged 11 BUFFER_OVERFLOW
  lines`, driver exit 1. Do not re-run a per-rect checker under llvmpipe to prove this:
  ~250k software-rasterized UI vertices per frame wedged the box for minutes.
* **Mouse pan and zoom never fired.** `draw_canvas` gated both on
  `ImGui::IsItemHovered()`, but the Canvas window submits no items - only draw-list calls -
  so that reads whatever another window submitted last and is effectively always false. Now
  `ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)`. Verified by
  injecting X events (libXtst via ctypes): 3 wheel clicks over a point take scale
  0.7250 -> 0.9650 = fit x 1.1^3 with the world point under the cursor pinned to 0.15 px,
  and a 40,-30 right-drag at scale 2 moves the origin by exactly -dx/scale, +dy/scale. With
  the old predicate both are no-ops, which is what it looked like on screen.
* `tests/golden/view_pan_zoom.png` re-recorded: the checker's rasterization changed (a
  viewport-anchored rect lattice became a canvas-anchored texture). Document pixels were
  unaffected — captured an alpha-255 copy of the fixture with the pre-fix and post-fix
  binaries and diffed them.

Still open, when convenient:

* Log line **length** is unbounded. ImGui culls whole lines outside a panel, so the Log
  panel's cost is (visible rows x longest line) rather than all 400 lines; nothing today
  logs a long line, and the frame sits at 0.5% of the buffer. If an AI response body ever
  goes to the log, cap it in `core/log::record` (one home) instead of growing the buffer.
* Frame time under Xvfb + llvmpipe with a 1600x1000 document at 900x600 measured
  99.5 -> 64.3 ms median across the checker fix. Both are far from the 5.7 ms above, which
  was a different scene; re-measure when the compositor and texture cache land.

* **Keyboard shortcuts are part of a command's registration.** `CommandTable::add` takes the
  bindings and normalizes the text, refuses a malformed shortcut and refuses to bind a key
  twice; `app::shortcut_of` turns a sokol key event into the same canonical text and
  `main.cpp` dispatches through the table; `command.list` reports the keymap. Seeded: `F` fit,
  `Ctrl+S` save, `Ctrl+Z`/`Ctrl+Y` undo and redo, `Escape`/`Ctrl+Q` quit - the rest arrive with
  their tools. Two findings: `simgui_handle_event()` returns `WantCaptureKeyboard ||
  WantCaptureMouse` and the pointer is always over chrome, so the old
  `if (simgui_handle_event(ev)) return;` dropped **every** key event (which is why the
  hardcoded Escape-quit never worked); and on a window-manager-less Xvfb display keys reach
  nobody until `XSetInputFocus` picks the app window, while mouse events do not care.
  `tools/wire_input.py` does both and `test_input_gestures` covers wheel zoom-to-cursor,
  right-drag pan, `F`, a failing `Ctrl+S` reaching the log, injected `Ctrl+Z` undoing a real
  stroke, an unbound key doing nothing, and `Escape` quitting.
* **The paint path exists and undoes exactly.** `core/document.{h,cpp}` owns the document
  model (it lived in `app/state.h`, where any command could poke it), `core/history.{h,cpp}`
  undoes and redoes **rectangle deltas** budgeted by stored bytes, `core/brush.{h,cpp}` is the
  CPU stamp path, and `tool.stroke` / `edit.undo` / `edit.redo` are on the wire with
  `Ctrl+Z`/`Ctrl+Y` bound. Measured numbers and what they ruled in and out:
  `docs/probes/2026-10-09-paint-core.md`. Two findings: a one-pixel brush painted *nothing*
  while the antialiasing feather lived inside the circle, and a command that checksums the
  pixels it just painted costs more than the paint did (166 ms -> 10.2 ms per 2048² stroke).

## Next steps, in order

1. **CI waits until MVP** (decided 2026-10-09): the workflow is not going to run on a runner
   before MVP, so nothing here should spend effort on remotes or runners. The local gate
   stands in for it and is the thing to keep green - `cmake -B build -G Ninja && cmake --build
   build && xvfb-run -a ctest --output-on-failure`. `.github/workflows/ci.yml` stays as
   written and unverified; when it finally runs, expect the golden to be what differs under
   Ubuntu's llvmpipe: read `differing_bytes` / `max_channel_delta`, then either widen the two
   constants in `tools/wire_drive.py` deliberately or re-record there with `--update-golden`.
   Never widen a tolerance to hide a structural diff.
2. **Paint from the pointer, not only from the wire**: the tool state machine (press / drag /
   release) in `app/tools/`, GPU stamp quads previewed during the drag, one `Document::paint`
   on release. The bound that matters is one pointer event per frame, and
   `docs/probes/2026-10-09-paint-core.md` says a commit cannot be paid that often: a full
   texture re-upload is ~3 ms at 2048², fine once per stroke and not fine per mouse event.
   `tool.stroke` stays the test path and the recorded-session path, and the wire command that
   makes the interactive tool observable is `tool.state` (or the fields it adds to
   `app.state`): active tool, in-progress stroke, dab count, pending commit.
3. **`.noise` v0 and the content-addressed blob store** (`ARCHITECTURE.md`'s file format:
   single-file zip, `manifest.json:format`, blobs by hash): undo wants rectangles, layers want
   `BlobRef` sources, and the container is what makes both worth persisting. Round-trip test:
   save -> load -> identical checksums and identical history depth; older `format` values
   refused with a message naming the version found.
4. **Layers** (`layer.*`), then the tool keys (`B`, `E`, `[`, `]`) that select and tune the
   brush instead of naming it per stroke.
5. **Dirty-rect texture upload**, only when something measured needs it - see the note above
   about what it is and is not.
6. **M0.5 probe harness** (`tools/tn_probe.py`, `docs/probes/`) — blocked on the dev server.
   Ask the human to export `THENOISE_URL` / `THENOISE_API_KEY`; it feeds `size_policy`, role
   default order, ETA seeds and recipe wording, so it should land before M3 designs hard-code
   any of those.

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
