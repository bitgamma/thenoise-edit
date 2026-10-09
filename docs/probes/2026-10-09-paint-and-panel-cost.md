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

Isolating the first cause, same build with only the repaint scope widened back to the whole buffer
(one layer, 40-event span strokes, run immediately after the post-fix sweep):

```
brush   scope=buffer med   scope=dabs' rect med
    20            8.8 ms              1.4 ms
    64            9.8 ms              1.3 ms
   200            9.4 ms              1.3 ms
   600           10.6 ms              1.2 ms
  1500           10.1 ms              1.5 ms
```

Brush size was never the cause, only the accelerator — the full sweep after the fix, Debug:

```
brush   drag  event med   p95   worst   frame med  frame max
    20   span     1.4    10.6   15.0        3.0       18.0
    64   span     1.3    10.1   20.1        3.3       20.3
   200   span     1.3     9.6   16.8        3.2       18.7
   600   span     1.2    11.7   16.4        3.1       17.3
  1500   span     1.5     9.4   16.5        3.2       19.3
   200  local     0.8     1.5    1.8        3.2        5.2
   600  local     0.8     2.1    3.2        2.2        2.5
  1500  local     1.1     4.6   10.7        2.4       12.2
```

A local circle never degraded, which is what pointed at covered-area-proportional cost rather than
brush size: before the fix it was 1.9 ms locally against 26 ms for a spanning stroke at the same
brush.

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
