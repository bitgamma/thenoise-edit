# Paint core, measured

2026-10-09. The numbers behind M1's acceptance target (*5000-dab stroke at 2048² without
dropped frames*), because the CPU cost, the history cost and the upload cost each looked like
the bottleneck until they were measured.

Environment: this dev box, 24 cores, `g++ -O2` for the CPU bench; the app under `Xvfb` with
**Mesa llvmpipe (LLVM 23.1.1, 256 bits)** - software rasterization, the same class of GL as
the CI runner, so the GL figures are not a discrete-GPU number. Single-threaded paint: the
stamp loop is not parallelised.

## CPU: stamping, history, undo

Whole stroke through `core::Document::paint`, serpentine path across the document, hardness
0.8, spacing 0.25 of the diameter. `touched` is what the history has to store.

| document | brush | dabs | touched | paint | paint + history | undo | history |
|---|---|---|---|---|---|---|---|
| 512² | 8 px | 5214 | 506×506 | 10.3 ms | 5.1 ms | 0.1 ms | < 1 MiB |
| 2048² | 20 px | 5299 | 2048×2048 | 28.0 ms | 32.1 ms | 10.5 ms | 16 MiB |
| 2048² | 40 px | 5085 | 2048×2048 | 91.9 ms | 93.3 ms | 8.2 ms | 16 MiB |
| 2048² | 80 px | 2035 | 2048×2048 | 126.3 ms | 127.8 ms | 9.3 ms | 16 MiB |
| 4096² | 20 px | 5770 | 4096×4096 | 38.6 ms | 53.2 ms | 28.0 ms | 64 MiB |

(The 512² row's `paint` is larger than `paint + history` because they are separate runs and
the first pays for the pages; read the 2048² rows as the shape.)

* 5000 dabs at 2048² with a normal brush is ~32 ms end to end: about two frames at 60 Hz, and
  the stroke is one commit, so the acceptance target is met by the CPU path with room.
* Cost tracks **pixels evaluated** (dabs × brush area), not document size: 4096² at a 20 px
  brush is barely worse than 2048².
* A stroke that spans the canvas makes a rectangle delta as large as the image - 16 MiB at
  2048², 64 MiB at 4096² - so `kHistoryBudgetBytes` (128 MiB) buys ~8 such steps and ~2 at
  4096². Real strokes cover a band, not a canvas; if that ever stops being true, tiles are the
  answer for the same reason they are the answer for partial uploads.

## App: frame cost, and what a stroke command actually costs

A one-off A/B (smaller stroke, ~1300 dabs, size 24) around the checksum change; the
reproducible version is `tools/stroke_probe.py` below:

| | before | after removing the reply checksum |
|---|---|---|
| idle frame, 2048² | 1.21 ms median | 1.21 ms median |
| `tool.stroke` call | **166 ms** median | **10.2 ms** median |
| first frame after the commit | 7.0 ms median | 4.2 ms median |

Two things that were invisible until measured:

* **The command was hashing the document it had just painted.** `sha256` over 16 MB cost more
  than the stroke did - 130 ms of the 166 ms call. The checksum lives on `doc.get`, which a
  caller asks when it wants one; `tool.stroke` answers with the rectangle, dab count and
  revision. Same reason `app.state` never hashes pixels.
* The full `sg_image` re-upload that `gfx::Texture::sync` does when the revision moves costs
  ~3 ms at 2048² under llvmpipe, **once per stroke**. That is affordable; what it rules out is
  bumping the revision per dab, which is why live interactive painting needs the GPU stamp
  preview (`ARCHITECTURE.md`, "Painting path") rather than commit-and-upload per mouse move.

## App, at the acceptance size

`tools/stroke_probe.py`, run as `test_stroke_probe`: a 5237-dab brush stroke (20 px, spacing
0.25) over a fresh 2048² document, with `app.state` sampled from a second connection while it
runs:

```
5237 dabs over a 2048^2 document, brush 20 px: stroke 108.5 ms, touched 2030x2030,
history stored 15 MiB, peak frame_ms seen 100.0, 7 frames presented around it
```

That is **three times the bench figure, and the difference is the build**: the bench is
`-O2`, the app is built `Debug` (the default here and in CI), which is where a paint loop
earns its keep. Two consequences:

* The acceptance bound in the probe is deliberately wide (1 s for the stroke, 400 ms for a
  frame): it is there to catch a paint path that becomes tens of times slower or stops
  undoing exactly, not one that loses five percent.
* If M1's remaining work needs the Debug build to keep up with a pointer, the answer is the
  GPU stamp preview, not a `-O2`-only test build.

Undo returned the document checksum exactly at this size, and the vertex buffer stayed quiet
throughout (`BUFFER_OVERFLOW` in the app's stderr fails the probe).

## Live gesture, fed one pointer event at a time

`tool.pointer` runs the same `walk_dabs` / `scratch_stamp` / `scratch_composite` path as
`tool.stroke`, but at the rate a pointer delivers events - faster than any hand, since a wire
client sends as fast as the socket allows. Each event restores the provisional snapshot and
composites the stroke's coverage once; the frame then uploads the whole canvas texture, which
is what makes the cost grow with the **document** rather than with the stroke. Same box, Debug
build, `tools/stroke_probe.py`:

```
live gesture 640^2:  121 dabs over 120 pointer events, brush 40 px: event median 1.98 ms,
       worst 6.28 ms, commit 1.5 ms, peak frame_ms 16.8, history 0 MiB in 1 step (MiB floored)
live gesture 2048^2: 424 dabs over 120 pointer events, brush 40 px: event median 9.20 ms,
       worst 35.60 ms, commit 2.2 ms, peak frame_ms 100.0, history 6 MiB in 1 step
```

What this decided:

* **The provisional CPU repaint is enough for a live brush.** It was worth measuring because
  `ARCHITECTURE.md` originally prescribed GPU stamp quads during the drag; at these numbers
  the stamps would buy nothing that a dirty-rect upload would not buy more cheaply, and they
  add a second paint path to keep honest. They stay documented as the escape hatch.
* **The snapshot follows the stroke, not the canvas.** Holding the whole visible area aside at
  press cost the same 9 ms per event and grew history from 6 MiB to 31 MiB for a stroke that
  touches a diagonal band - the same figure for a 4 px dot. `brush_press` now lets the first
  stamp decide the area.
* The per-event median is a *command* cost, not a frame budget: a frame coalesces several
  events, and `peak frame_ms` is what the user would feel. Both are printed because the first
  is the regression guard (`MAX_GESTURE_EVENT_MS`) and the second is the experience.

Undo restores the document checksum exactly at both sizes, and one gesture is one undo step at
both - checked in `test_app` (`test_a_pointer_gesture_*`) and in the probe.
