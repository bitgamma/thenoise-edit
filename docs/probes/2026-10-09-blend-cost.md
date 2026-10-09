# Probe: what a composite costs (2026-10-09)

Reported: with a 2048² document and a few layers, the opacity slider arrives in jumps and hiding a
layer takes a visible beat. Measured on the running app over the debug wire (Xvfb + software GL,
shared box, Release), and separately headless through `Document::composite` so the arithmetic could
be separated from the frame. Ratios matter more here than milliseconds; before and after runs are at
different times of day.

## A. The frame after a layer-state change was all blend arithmetic

Toggling a layer's opacity or visibility dirties the whole canvas, so the next frame rebuilds every
pixel of every layer. Nothing else in the frame was responsible: idle frames measured 2.8 ms at
2048² with three painted layers and a steady revision (~430 fps), shrinking the window from
1400x900 to 420x300 changed nothing - which rules out fill rate - and the texture upload is
revision-gated, so it did not appear at all.

```
peak frame after a whole-canvas change, 2048², 3 painted layers (Release)

before   opacity slider step   100.0 ms     after   22.3 ms
         visible -> off        100.0 ms             16.8 ms
         visible -> on         100.0 ms             29.3 ms
```

`frame_ms` was cross-checked by counting frames, because `PLAN.md` already warns that
`sapp_frame_duration()` can read a fixed value when a whole-document command lands in the sampled
frame. Dragging the opacity - one step every 8 ms, faster than any hand - and dividing wall time by
the frames actually drawn:

```
before   200.1 ms per frame   (5 fps)        after   20.4 ms per frame   (49 fps)
```

Headless, one `Document::composite()` over the same three layers, so the repair alone:

```
3 x 2048², one layer at 55 %   Release   82.5 ms -> 14.6 ms     Debug   806 ms -> 269 ms
3 x 512²,  one layer at 55 %   Release    5.9 ms ->  1.8 ms
```

## B. Where the pixels went

One `blend_layer` pass over 2048² (4.2 M pixels), Release, `memcpy` of the same buffer for scale.
`before` is the single general path; `after` is the same code with the paths in section C in front
of it.

| source                                        | before  | after  |
|-----------------------------------------------|--------:|-------:|
| 12 % painted, rest empty, `normal`            |  6.7 ms |  1.1 ms |
| 12 % painted, `multiply` at 50 % strength     |  6.6 ms |  2.0 ms |
| opaque, `normal`                              | 66.7 ms |  2.5 ms |
| opaque, `multiply`                            | 65.8 ms |  7.1 ms |
| opaque, `color-burn`                          | 67.8 ms |  7.1 ms |
| opaque, `multiply` at 50 % strength           | 59.4 ms |  8.8 ms |
| uniform a = 200 (a feathered layer), `normal` | 66.6 ms |  8.6 ms |
| alpha different every pixel, `normal`         | 67.3 ms | 73.5 ms |
| alpha different every pixel, `color-burn`     | 79.6 ms | 76.5 ms |
| the `memcpy` every repair also pays           | 0.47 ms | 0.49 ms |

The general path was 16 ns a pixel - about fourteen divides and four roundings, with the mode picked
by a switch inside the loop - against 0.1 ns for a copy. So the shape of the fix came from which
pixels a document actually contains, not from shaving the arithmetic.

The last two rows are the honest cost of this change: a layer whose alpha differs from pixel to
pixel presents a new alpha pair per pixel, gets no table, and pays the branch tests the other cases
earn their speed from. It is 9 % slower, and it is what a full-canvas soft gradient looks like. The
give-up rule below keeps that from being worse; anything that fixes it properly is in section D.

## C. What the tables are

Three paths in front of the reference arithmetic, all in `core/blend`, all producing *the same
bytes* - not a closer approximation:

- **`alpha == 0` is absent, not black.** Skip the pixel. A document-sized layer with a stroke on it
  is >99 % this branch, which is most of the sparse-row win.
- **Both operands opaque at full strength.** With `ab = as = 1` the composite collapses to `B`, so a
  per-mode 64 KB table over (backdrop byte, source byte) is exactly what the doubles compute.
  `test_blend` checks all 65536 entries of all eight modes against `blend_separable`.
- **Anything else.** Once the two alphas are fixed, one channel of the result is a function of two
  bytes, so a partial-strength layer over a backdrop of one alpha is a 64 KB table per
  (source alpha, backdrop alpha) pair. A pair earns its table after 40 000 pixels of its arithmetic
  (building 65536 entries measures 0.48 ms against 15 ns a pixel, so the two cross near 30 000 and
  the constant sits just past it); 64 pairs are tracked, 4 get tables, a call over a region smaller
  than 65536 pixels does not allocate the machinery at all, and a call that has run out of pair slots
  without ever earning a table stops asking.

Pairs are indexed directly rather than searched: a list scan looked reasonable and cost 14 % on the
varying-alpha fixture, which is the one place where every pixel asks a different question.

`test_blend` pins both halves: 35.7 M checks compare a wide partial-strength blend against the
specification written independently at the top of the file, and compare one wide call against the
same pixels blended in 32x32 tiles that keep the machinery off. A correctness test cannot see a fast
path, so one test compares the wide call's cost against tiling the same pixels and requires it to be
cheaper - relative, because a slower machine is not a failure.

Red-checked: swapping the operands in the table builder (3.5 M failures), writing a wrong result
alpha into a table (146 k failures), and switching tables off entirely - caught only by the cost
comparison, which is the intended split: with tables off the pixels stay exactly right.

## D. Still open

- A layer whose alpha varies per pixel is 9 % slower than before this change and gets no table. Both
  real fixes are bigger than a fast path: tables beyond the cap with an eviction policy, or a
  premultiplied float backing store, which is a different composite and a different `.noise`.
  Nothing measured so far asks for either - a soft brush edge is thousands of pixels, not millions.
- `Document::composite` repairs the union of everything dirty, so a document with one dirty corner and
  eight layers still costs one pass per layer over that corner. Correct and cheap; content rects per
  layer would make it cheaper still, and is the next item on the list in `PLAN.md` if a real document
  ever says otherwise.
- The composite is uploaded to GL whole when its revision changes: 16 MB at 2048². It hid behind the
  repair in every number above, so it has not been measured on its own. Dirty-rect upload is the
  `PLAN.md` item that would make a small change cheap on a machine where 16 MB is not free.
- No GPU blending exists, so there is still no shader pair to check this reference against. The
  reference stays the only implementation until something measures a reason for a second.
