# Probe: painting a mask and dragging a marquee (2026-10-09)

Reported: *painting in the mask is slower, much slower than painting in the image*. 2048² document,
brush at ordinary sizes. Xvfb + software GL on a shared box; Debug for the suite, Release for the
per-stage numbers, because a stage measured in Debug says nothing about the one next to it. Absolute
milliseconds are for shape; the ratios are the claim.

Four costs, three of them on the mask path. All four fixed; one candidate fix measured and declined.

## A. Every read of a mask's counts re-scanned the plane

The panel asks the active layer's mask `covered()`, `area()` and `bounds()` every frame it is open, and
those came from one cache that any write dropped — so a stroke paid a scan of the plane **per pointer
event**. `Mask::paint` over a 310x130 rectangle costs 0.01 ms; the numbers afterwards cost this:

```
2048x2048 plane, one 310x130 write                        before      after
covered + area + bounds after a write                      32.25 ms    0.01 ms   (Release)
covered() after one write invalidated it                   31.86 ms    0.00 ms
covered() from the cache, unchanged                         0.00 ms    0.00 ms
```

The counts are kept per row now — cells touched, cells at full coverage, the sum of the bytes, and the
first and last covered cell — and a write marks the rows it touched. `covered()` and `area()` are sums
over `h` rows and `bounds()` reads two rows' extents, so the cost follows the stroke instead of the
canvas. `test_counts_after_a_small_write_cost_the_rows_they_touched` holds the line, against a pass
over the same plane's bytes.

## B. Asking for the counts also walked the boundary

`facts()` computed the ants' boundary in the same pass, so a panel that wanted three numbers paid for
an edge scan too. The boundary is its own cache now: an overlay asks for it, a readout does not.

## C. The composite copied a masked layer even when the mask asked for nothing

A masked layer went through `scale_by_coverage` into a scratch and then blended, always. Two of the
cases need no pass at all, and both answers come from the per-row counts rather than from the pixels:

```
2048x2048, a 310x130 repair                                before      after
layer with no mask                                          0.27 ms     0.27 ms
layer under a wholly shown mask                             1.45 ms     0.36 ms
layer under a partly covered mask                           4.22 ms     4.36 ms
```

The wholly-shown case is what `mask.add` produces, so a fresh mask is now free; a single layer with a
wholly shown mask goes back to being its own composite. The partly covered case is unchanged, because
its cost is not the copy:

```
the two passes, separately                                  Release
scale_by_coverage alone                                      0.36 ms
blend of an opaque source                                    0.04 ms
blend of the same source at half coverage                    0.80 ms
```

Blending semi-transparent pixels is the expensive half and it is inherent — a layer at 50% opacity
costs the same — so fusing the multiply into `blend_layer` would re-couple `core/blend` to `Mask` to
save a third of one pass. Declined, and recorded here so the next reader measures before restructuring.

## D. A marquee event cost 18x a pixel event

Not reported; the probe found it. The boundary walk scanned the whole canvas whatever the selection
covered, and dragging a marquee replaces the selection every event:

```
tool.pointer round trip, 2048x2048, 40 events              before      after
dragging a marquee                                          87.9 ms     7.1 ms   (Debug, app)
painting the layer                                           3.5 ms     3.4 ms
painting the layer mask                                     ~35 ms       3.5 ms
```

Every edge has a covered pixel in it and every covered pixel is inside the mask's bounds, so the walk
visits the bounds and reads neighbours outside them. Same output, verified two ways: the boundary of a
2048² mask is compared against the same boundary computed the whole-plane way in
`test_the_boundary_of_a_small_shape_costs_the_shape_not_the_plane`, and `tools/ants_window_probe.py`
-calibrates the document's on-screen rect from a whole-canvas selection's own dashes and then checks
that a selection at (1200,1300,250x200) has all four sides drawn:

```
document on screen: 101,43 to 792,733 (scale 0.3374)
left 73 px / 67 px side   right 73 / 67   top 91 / 84   bottom 89 / 84
```

## What it costs now

`tools/mask_speed_probe.py`, 2048², 300 px brush, Release-built app, Debug numbers, 40 events each:

```
painting the layer          median 3.35  mean 4.05  worst 10.09 ms
painting the layer mask     median 3.51  mean 5.23  worst 15.14 ms      -> 1.29x
dragging a marquee          median 8.52  mean 8.66  worst 16.98 ms      -> 2.14x
```

A mask stroke is within frame noise of a pixel stroke. The marquee is not, and what is left is the
select tool's own work: it builds a canvas-sized `Mask` and hands a copy of it to the document on every
event, which at 2048² is a few megabytes of zeroing and copying before the outline is computed at all.
That is the next thing to make proportional, and the probe is the instrument that will show it.

## Guards

Both timing guards are relative (same pixels, cheaper work) and were mutation-checked, because a fast
path that is deleted leaves every pixel correct and every test green: 13 mutations against the row
counts and the composite's two skips, 5 against the boundary window, all red, none broken. The
absolute numbers above came from a throwaway benchmark against the Release core, kept out of the tree
because it measures stages rather than behaviour.
