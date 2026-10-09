# Probe: what one edit costs (2026-10-09)

Reported: with a 2048² document, clicking another layer in the panel and moving the opacity slider
are not instant; earlier the same slowness was reported for big brushes. Measured on the running app
over the wire — Debug unless stated, Release for the one comparison that needed it — Xvfb + software
GL on a shared box. Absolute numbers are for shape, not for speed; pre-fix and post-fix runs are at
different times, so the ratios matter more than the milliseconds.

Two defects, both structural, both fixed here.

## A. Every layer command hashed the whole document

`layer.select` and `layer.set` answered with the same row builder as `layer.list`, and that builder
computed a SHA-256 over every layer, always. So each click — and **every step of the opacity
slider** — paid for hashing the entire document.

```
before   layer.select        min   492 ms  median   503 ms      (2048^2, 3 layers)
         layer.set opacity   min   484 ms  median   504 ms
after    layer.select        min     1.5 ms  median   1.7 ms
         layer.set opacity   min     1.3 ms  median   1.6 ms
         layer.set visible   min     1.5 ms  median   1.6 ms
```

`doc.get` (492 ms Debug, 132 ms Release) and `layer.list` (494 / 176) still hash, deliberately: they
are the named diagnostics, and `USER-GUIDE` says the live row views carry no checksums because those
cost a whole-document pass.

## B. A pointer event cost what the stroke had already covered

`app.state.stroke.provisional_area` is the rectangle a gesture holds pixels aside for, and it is the
cost signal. One straight diagonal drag, brush 24 px, 2048², 40 events — measured before any code
change, and after:

```
event            0      12      24      40
before          2.9    11.9    39.4   108.5 ms      area    676 -> 3.26 M px
after           0.7     0.8     1.5     1.6 ms      area    676 -> 4.19 M px (capped at canvas)
```

The covered area still grows — the buffer has to hold it — but it no longer sets the price of an
event. Three causes, all the same shape:

* `repaint_provisional` restored the held-aside snapshot and re-blended **everything the stroke had
  reached**. A pixel's composited value depends only on its own coverage, and the scratch already
  holds every earlier dab that reached that pixel, so restoring and re-blending only the rectangle
  the new dabs touched is exact rather than approximate. `test_brush` pins it: piecewise against
  whole-scratch, byte for byte.
* `StrokeScratch::grow_to` grew to *fit*, copying every accumulated pixel on every growth. It grows
  by `kBufferGrowthFactor` now (`grown_rect`, `core/geom.h`), which `test_geom` pins: coverage
  preserved, growth count and total pixels moved both bounded.
* the held-aside snapshot had the same growth problem, and its row copies were per pixel; they are
  `memcpy` between two origins now.

Isolating that cause, same build with only the repaint scope widened back to the whole buffer,
against the committed code — Debug, one layer, 40-event drags, 4 ms between events, and **the brush
size read back out of `app.state` after every change** (see the measurement defect below):

```
brush  drag |   before: med   p95  worst |    after: med   p95  worst
    24  span |     8.9   24.6    33.8   |     1.0   13.1    15.5
    64  span |    13.2   30.4    38.7   |     1.6   12.5    20.7
   200  span |    21.8   45.6    48.1   |     3.6   12.7    18.4
   600  span |     1.6   84.9   100.5   |     1.2   33.7    35.4
  1500  span |     0.8  175.8   181.6   |     0.9  125.6   149.5
    64 local |     0.7    1.6     1.8   |     0.7    1.5     1.9
   600 local |     0.9   32.9    34.1   |     0.6   22.8    32.5
  1500 local |     0.8  110.3   181.1   |     0.8  109.0   161.5
```

Read the p95 and worst, not the median, once the brush gets large: a 1500 px brush at spacing 0.25
emits a dab every 375 px while these events advance 47 px, so most events stamp nothing and their
median is the cost of an empty event. What the table says:

* Up to a 200 px brush the repaint scope is the whole story — 24.6 → 13.1, 30.4 → 12.5, 45.6 → 12.7
  at p95, and the median for a normal brush 8.9 → 1.0.
* Above that a dab *is* the work: a 1500 px brush covers 1.7 M pixels, and 125 ms per stamping event
  is Θ(dab area) on the CPU. The scope fix takes the edge off (175.8 → 125.6) and cannot remove it;
  GPU stamping, still deferred, is what does.
* A local circular drag never degraded before or after at small brushes (0.7 ms at 64 px), which is
  what pointed at covered-area-proportional cost rather than brush size in the first place.

## A measurement defect found on the way, worth more than the fix

`tool.brush` **selects a tool and reads no fields**; brush fields belong to `tool.set`. The wire
ignores an argument its command does not read, without a word. So `tool.brush {size: 48}` succeeded,
did nothing, and the first version of the sweep above reported timings for brushes it was never
running — the rows labelled 20 to 1500 were one default-24 px drag repeated five times. `stroke_probe`
had the same hole: it printed "brush 40 px" for a gesture running at the default.

Fixed in the probe: the brush is set through `tool.set` and **read back from `app.state`, which the
probe now fails on if it disagrees** with what it means to measure.

```
FAIL: the probe means a 40 px brush, the app is on {'size': 20.0, 'spacing': 0.25, ...}
```

The protocol question is open and belongs to the human, because it changes the wire contract rather
than a measurement: should the command table reject an argument its command does not declare, naming
the command and the arguments it does? Every command declares its accepted keys already, so the check
is central and small; the risk is a caller that today passes extra fields and gets away with it. Per
`AGENTS.md`, "errors name the thing" argues for yes — silently ignoring `size` cost an hour and a
published table of wrong numbers.

With the brush genuinely at 40 px, the guard `stroke_probe` runs per gesture (120 events, first third
against last third) measures:

```
640^2  x 1 layer   median 0.87 ms  worst  5.70 ms  commit 2.4 ms  x1.7
2048^2 x 1 layer   median 1.13 ms  worst 21.70 ms  commit 4.4 ms  x1.2
2048^2 x 3 layers  median 0.73 ms  worst 16.29 ms  commit 3.3 ms  x2.0
```

## Frames

`app.state.frame_ms` is `sapp_frame_duration()`, which includes the wait for the swap — so a frame in
which a whole-document command ran reads enormous, and that is the metric, not the paint. Sampled
during a 60-event gesture on 2048² × 3 layers at three drive rates:

```
idle, no input                          median  1.1 ms   max   1.2 ms
gesture, wire flooded (no sleep)        median  2.6 ms   max  11.8 ms
gesture, 4 ms between events            median  2.2 ms   max   6.1 ms
gesture, 20 ms between events           median  1.5 ms   max   2.6 ms
gesture, wire flooded, Release          median  3.5 ms   max  11.2 ms
```

The ~100 ms peaks `stroke_probe` prints come from whole-document diagnostics (`doc.get`,
`pixels.compare`) landing in the sampled frame, not from painting.

## Guard

`stroke_probe` now compares the first third of a drag's events with its last
(`MAX_GESTURE_DEGRADATION = 4.0`): an event must cost what it added, not what the stroke has covered.
Observed after the fix on 120-event gestures: `x0.8`–`x0.9`. Tightened to `1.0` the check goes red, so
it answers to the shape of a drag rather than only its level.

## What is left

* **The composite itself.** A uniform change — opacity, visibility — re-blends the whole canvas on
  the CPU: 75 ms at 3 × 2048² in Release, 245 ms in Debug. This is precisely the case the design puts
  on the GPU, where it costs no pixel pass. Cheap steps before that: exact fast paths in `blend_layer`
  (opaque source over anything, transparent destination) and per-layer content rectangles so an empty
  layer contributes nothing to blend.
* **Whole-texture upload.** `sg_update_texture` replaces a whole texture; sokol has no partial
  update. The dirty-rect upload item, unchanged, and now the floor under a pointer event.
* **`doc.get` / `layer.list` hashing** — kept, off every interactive path.
