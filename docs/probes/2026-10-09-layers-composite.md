# Layers, the composite, and what a stack costs on the frame path

Measured 2026-10-09 on the dev box: Debug build, Xvfb + llvmpipe, driven over the debug wire by
`tools/stroke_probe.py` (ctest `test_stroke_probe`) and `tools/container_probe.py`
(`test_container_interop`). Numbers are records; the asserted bounds are wide regression guards.

## A live pointer stroke, 120 events, brush 40 px

| document | event median | event worst | peak `frame_ms` |
|---|---|---|---|
| 640², 1 layer | 2.0 ms | 6.0 ms | 17.1 |
| 2048², 1 layer | 10.9 ms | 30.5 ms | 100.0 |
| 2048², 3 layers — composite rebuilt whole | **71 ms** | 99 ms | 100.0 |
| 2048², 3 layers — composite repaired per rectangle | 33.7 ms | 122 ms | 100.0 |
| 2048², 3 layers, a short local drag | 9.4 ms | 25 ms | 25.2 |

The last row is the same code as the row above it with a path that stays in one corner. The cost
tracks **the area a stroke has reached**, not the number of dabs: a live event restores the
provisional snapshot over the stroke's bounding box, repaints the accumulated coverage into it,
and repairs the composite over that same box. A gesture that spans a 2048² document therefore
costs what it costs whether it is drawing a dot or a line.

## Save and open, which costs PNG encodes

| document | on disk | `doc.save` | `doc.open` |
|---|---|---|---|
| 2048², 1 layer, one stroke | 293 KiB | 589 ms | 222 ms |
| 2048², 3 layers, one stroke each | 734 KiB | 1395 ms | 630 ms |

One PNG per layer plus the composite, so a save is linear in layers, and the composite encode is
the reason `mergedimage.png` is not simply the top layer. Saving is explicit, so this sits off the
frame path; when the layer count gets large the encoding is what to make incremental (per-layer
PNGs only for layers that changed, which needs the same dirty set the composite now keeps).

## What the numbers ruled in and out

* **The whole-stack composite went, and the probe's own bound is what caught it.** The first
  layer-aware `composite()` rebuilt the flattened image on every revision bump, which put a
  full-image blend on every frame of a live stroke: 71 ms per event at 2048² with three layers
  where the single-layer case costs 10.9 ms. The fix is two rules, not a new subsystem: a stack
  of one is returned as that layer (blending one layer has no second operand), and a repair
  covers only the rectangles that changed since the last time the composite was asked for.
  `test_stroke_probe` now runs the three-layer gesture against the same bound, so this cannot
  come back quietly.
* **Dirty-rect texture upload is still not forced.** After the fix, three layers at 2048² cost
  33.7 ms per event for the pessimistic canvas-spanning gesture and 9.4 ms for a local one, and
  the whole-texture upload is a smaller term than the paint plus repair. If a fourth measurement
  says otherwise, the composite already tracks the rectangles — the missing half is a backend
  that can take a partial upload, or tiles.
* **Structure is cheap, encoding is not.** Adding and removing layers costs one full composite
  repair, which shows up as a single frame rather than a stall; nothing about a stack change is
  on the per-event path.

## The finding that was not about layers

The three-layer case first reported `peak frame_ms 0.0` next to events of 34 ms. Zero frames
during a stroke that was measurably running is not a measurement, and the cause was in the debug
wire: a client that hung up was read as "nothing to read yet" (`recv` returns 0 for end of
stream), so it kept its slot, and once `max_clients` slots were held no new connection was ever
serviced. A driver that reconnects — which every Python test does — eventually just hangs.
`platform::read_some` now distinguishes end of stream from an empty read, and
`test_a_hung_up_client_gives_its_slot_back` connects and hangs up well past the client bound.

The lesson is about observability: a missing number is itself a result, and worth chasing harder
than a bad one.
