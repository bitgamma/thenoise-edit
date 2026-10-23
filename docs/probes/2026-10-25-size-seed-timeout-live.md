# 2026-10-25 — what the region is asked at, what a seed may be, and how long to wait

Live probes on the dev box (`$THENOISE_URL`, `$THENOISE_API_KEY` exported by the human; nothing here
names it). Every number below is a wall-clock measurement or an HTTP answer, not an assertion the suite
makes — the suite asserts the contracts described at the end.

Resident model: `Qwen-Image-2.1-Turbo-INT8`, `image_defaults: {steps: 8, cfg_scale: 1.0, 1024x1024}`,
no `soft_pixel_cap` published for it; `Flux-2-Klein-9B-TheNoise` publishes `2097152`, and
`builtin.Qwen-Image-2.1-Turbo-INT8` publishes `4194304`. The table's stand-in is therefore `2097152`.

## Durations, and the timeout that could not hold them

| endpoint | size | steps | mask | wall |
|---|---|---|---|---|
| `/images/edits` | 1024x1024 | 4 | – | 20.2 s |
| `/images/edits` | 1440x1088 | 4 | – | 29.7 s |
| `/images/edits` | 1024x1024 | 8 | yes | 33.5 s |
| `/images/edits` | 1920x1088 (2.09 MP) | 8 | yes | **71.1 s** |
| `/images/edits` (through the app) | 1440x1088 | 8 | yes | 60–70 s per ask |
| `/images/edits` (through the app) | 1760x1168 | 8 | yes | 94.6 s |

`seconds ≈ constant × pixels × steps` holds to within ~7 % across these (predicted 66.8 s for the
1920x1088 row against 71.1 s measured), which is why `LatencyModel::request_timeout_s` scales the
model's best-sampled run that way instead of falling back to the floor. A model that is not resident
loads inside the request, which measured 72–198 s earlier, so the floor has to cover a load plus a
generation: it is 600 s.

Before this, the exact-key ETA left every new mark on the 60 s floor and a full-canvas inpaint came back
as `timeout - /images/edits: timeout` on a healthy server.

## The seed's real bound, probed at 256² and one step (a 400 runs no model)

| seed | `/images/generations` (JSON) | `/images/edits` (multipart form) |
|---|---|---|
| 0 | 200 | 200 |
| 2147483647 | 200 | 200 |
| 2147483648 | 200 | – |
| 3000000000 | 200 | **400 `Invalid value for 'seed': must be an integer`** |
| 4294967295 | 200 | **400 same** |

The upload path parses `seed` as a signed 32-bit integer, so `ai::kMaxSeed = 0x7FFFFFFF` — the stricter
of two endpoints that publish nothing about it. `seed: 0` is accepted everywhere, so keeping 0 out of the
app's own draws is our "no seed yet" convention, not an engine rule.

## Sizes, on the box, through the app

* Full-HD document, mark `[200,200,1000,700]` → grown crop `[0,0,1440,1080]`, asked `1440x1088`,
  `scale 1.0000`, `resampled false`; the engine answered `1440x1088` (`answered_as_requested` true).
  738860 pixels written under an 8 px feather; **0 pixels changed outside the coverage**, both asks.
* Two asks of the same mark, same words: seeds `144553783` and `1426894404`; 93.8 % of the marked area
  differs by more than 8/255 — different pictures, not the same one re-drawn.
* Over the ceiling: 2400x1600 mark, `scale 0.7390` → asked `1760x1168` = 2055680 px, inside the
  published 2097152. The engine answered `1760x1184`, one grid step taller than the ask, so
  `answered_as_requested` is false and the landing says both things separately:
  `answer: 2400x1600 under a 8px feather: 3840000 pixels, rescaled to 1760x1168 for the model's pixel
  ceiling, answered at a different size than requested`. Pixel-lock outside the coverage still exact.
* Gradient energy inside the mark (recorded, not asserted): source 39.36, answer 23.47 and 26.18. The
  model's own rendering, at a size that is 1:1 with the region.

## Contracts the suite holds (hermetic, after these findings)

* `test_region`: 8047 checks including a 1040-shape sweep — the ask is on the grid and never above the
  ceiling it was shrunk for; `resampled` and `scale` and the asked area agree with each other.
* `test_backend`: the ETA stays a measurement (no value for a shape never run here); the timeout scales
  by pixels × steps from the model's measured run, never below the floor, and the floor covers a cold
  ask (load + the largest edit the policy can ask for).
* `test_provider`: a seed above `kMaxSeed` is refused by name on both endpoints, at the boundary and one
  over.
* `test_app`: one seed per ask, a named seed dies with its press, a pin survives until released, the
  draw is masked to the field; the ceiling the compile used is on `ai.recipes`; the landing record and
  its log line report the rescale.
* `ai_job_probe` (recorded): all claims held, 3 requests; every ask's seed inside the form's bound.
