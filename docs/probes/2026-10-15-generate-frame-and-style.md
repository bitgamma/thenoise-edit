# The aspect presets and the style family, against the recordings and the dev box

Run 2026-10-15. `ai/`, `app/`, the recipe table and the Tool panel at `e618fee` plus the fixes below.
Live against the dev box (`$THENOISE_URL` / `$THENOISE_API_KEY`), hermetic against
`tests/fixtures/http`. 5 asks spent, small sizes, the model the box was running put back afterwards.

## What ran

```
python3 tools/ai_job_probe.py --bin build/noise-edit --generate
python3 tools/ai_job_probe.py --bin build/noise-edit --live --only generate --generate --max-images 5
python3 tools/panel_aspect_probe.py --bin build/noise-edit
ctest --test-dir build                    # 34/34, 282 s
```

The generate cell takes the document at 420x300, then asks `generate` at every frame `new-image` offers
(`1:1`, `3:4`, `4:3`, `16:9`) plus no choice at all, and ends with the style the panel would run
(`new-image-anime`).

## Box

`Flux-2-Klein-9B-TheNoise` resident before and after. `generate.photo` resolves to it,
`generate.illustration` to `Flux-2-Klein-4B-TheNoise`, so the style cell is a real cross-model family and
`ai.intent`'s `family` block reports `swap: true` for it. No pinning, no eviction: 5 asks at ~2.8 s each,
the same band the marked-area verbs pay on this box.

## Contract and invariants, both tiers

- The frame holds the ratio to half a pixel of its short side and fills the canvas on one axis; the crop
  covers the frame; the requested size is the crop on the policy's 16 px grid; the status line quotes that
  size. `16:9` on 420x300: frame `[0, 32, 420, 236]`, crop `[0, 32, 420, 240]`, asked `432x240`.
- Nothing in the request names an aspect: `prompt`, `size`, `steps`, `seed`, `model`, and for the
  reference styles `init_image`/`strength`. A frame is geometry, not a field, so the field policy was not
  touched.
- The answer obeys `size_policy` (`432x240`, `400x304`, `304x300`), lands as a new layer named
  `Generate <job>`, and a re-ask adds no layer.
- Pixel-lock, whole-image: nothing outside the frame changed, and the inside of the frame did -
  `124564 of 124564` pixels at full canvas, `92660 of 92660` at 16:9. Undo returns the document byte for
  byte.
- The landing report names the frame, not the grid-grown crop.

## Three things the cell found, and what they became

- **A generation feathered past its own frame.** `new-image` inherited `feather: auto`, and `compose`
  softens the coverage boundary in both directions, so an answer bled ~8 px beyond the frame into canvas
  that was never in the upload: `1680` pixels outside a `420x236` frame at `16:9`, `3000` at `3:4`. A
  generation is not a repair of what surrounds it, so the three generate recipes now say
  `feather: none` (which is what `reimagine`, the other whole-document recipe, already said) and a table
  test refuses a document-scope `from_words` recipe that feathers.
- **The report lied about the box.** `Landing` stored the crop and `plan_of_landing` filled `mask_bounds`
  from it, so every artefact a person checks - the saved mask, the `job.get` region - was drawn on the
  crop. `Landing` carries the mask bounds now.
- **A live `job.wait` outran its socket.** The probe's wire connection had the default 15 s read timeout
  while `job.wait` was given 1800, so a cold model load killed the run mid-flight and skipped the
  model restore. The connection now outlasts the wait by design.

`tools/panel_aspect_probe.py` covers the panel's side of the same three facts: both dropdowns drawn over
the `Generate` key at any window height, the status line carrying ratio and style, a chosen style reported
as the recipe the key would run, and the click landing a layer named for the job.

## Timings, recorded not asserted

| ask | size asked | seconds |
|---|---|---|
| canvas | 432x304 | 3.1 |
| 1:1 | 304x304 | 2.8 |
| 3:4 | 240x304 | 2.8 |
| 4:3 | 400x304 | 2.9 |
| 16:9 | 432x240 | 2.8 |

The style cell was skipped by `--max-images 5`. Its request is identical but for `prompt` and `model`, and
the model it would swap to is reported on the row before anything is sent; a live run with a budget of 6
pays for it.
