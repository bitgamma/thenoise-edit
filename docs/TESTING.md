# TESTING — how any of this is proven, and what it costs

The bar for a test in every tier is `AGENTS.md` §2: a plausible bug must turn it red. The architecture facts
being asserted are in [ARCHITECTURE.md](ARCHITECTURE.md); what a run is *for* is
[PRODUCT.md](PRODUCT.md)'s scenario.

## Tiers

| Tier | Runs | Network | Content |
|---|---|---|---|
| **unit / property** | always, `ctest` | no | region compiler round-trips, aspect names, mask ops, blend reference, history and undo invariants, format round-trips |
| **golden image** | always, `ctest` | no | named fixtures → exact bytes: codecs, mask ops, composite, the viewport capture |
| **recorded backend** | always, `ctest` | no | `tests/fixtures/http` as the server: `test_roles` (resolution order, the shipped `roles.json`), `test_recipes` (policy parsing, the shipped set agreeing with the roles), `test_provider` (single-image compatibility and exact ordered multipart bytes for two and three images), `test_backend` (discovery, decoding, every failure class, cancellation, replay, and that a re-ask is one send more) |
| **app over the wire** | always, `ctest` | no | `tools/*_probe.py`: the running editor driven through its own command table — paint, layers, masks, view, panel, and `ai_job_probe.py` for the whole AI path against recordings; `test_app` covers adding/removing/reordering file and layer references, undo relinking, per-operation inclusion defaults, ordered Change/Fill requests, cache invalidation, queued snapshots, request landing, pixel lock and undo/redo |
| **live backend** | on demand, tagged `live` | **yes** | the real contract against the dev box |

The default `ctest` run opens no sockets, even in a shell that exports the credentials: live tests are
registered **only** with `-DNOISE_LIVE_TESTS=ON`.

```sh
cmake -B build && cmake --build build && ctest --test-dir build      # hermetic, ~5 min
cmake -B build -DNOISE_LIVE_TESTS=ON && ctest --test-dir build -L live
```

## Live tests

Live tests are expected, not exotic. The dev box is reachable when the human exports `THENOISE_URL` and
`THENOISE_API_KEY`; **ask for them when a change needs live verification**, naming the tests and the
approximate image count. Behaviour:

* absent env ⇒ `skip` with a loud reason, never `fail`, never silently green;
* present env ⇒ they run, and their failures are as real as anyone else's;
* a live run loads the model the role resolves to when the box is running another — the swap refusal is a
  guard for a hand on a key, and a driver that refuses every verb is a green run of nothing — and puts
  back whatever was resident when it started, including after a failure. Say how many images a run spent;
  `--max-images` is what bounds it, and a spent budget skips the rest rather than passing them;
* they assert **contract and invariants, never model output**: roles resolve, output dims satisfy
  `size_policy`, **pixel-lock outside the coverage holds** (that part is ours, so it is exact whatever the
  model produced), error shapes and status codes, cancel works;
* they are budgeted — small sizes, model default steps, a per-run `--max-images` cap — and they put back
  any model they swapped, because a swap costs minutes on a shared box;
* timings are recorded to `docs/probes/` and never asserted. The runtime uses a measured EMA and an
  adaptive `timeout_request` rather than published latencies for the same reason.

A budget that runs out mid-probe reports a skip, never a pass over checks that were never made.

## Probe harness

`tools/tn_probe.py` exercises a real server directly and its output becomes `docs/probes/*.md`. It resolves
the `edit` role the way the registry does (capability predicate over what the box has downloaded, smallest
first; `--model` overrides), then asserts only what the client branches on — discovery fields, that an
explicit `size` comes back exact, that a snap lands on the policy's multiple, the shape of a 404 and a 400 —
and prints timings instead of asserting them. Requests vary the seed, since an identical request is a
server-cache hit rather than a run.

`tools/ai_job_probe.py` is the app-level one: it drives a running `noise-edit` over the wire through
`server.connect` → `ai.preview` → `ai.run` → `job.wait` → `job.apply`, checks that the request obeyed the
size policy and sent no `mask`, that the pixels which changed are exactly the ones the report counted, that
none lie outside the coverage the job claimed, and that undo returns the document byte for byte. Without
`--live` it points the app at `tests/fixtures/http` and runs in the default suite, which is why the whole
path is checked hermetically and then again cheaply against the box. `--only generate` aims a live run at
one cell so the budget goes where the change is.

`tools/wire_drive.py` is the shared harness: app startup, display claim, child reaping, golden capture.
Everything in `tools/` that makes a claim is registered in `tests/CMakeLists.txt`, and every probe that
starts the app asks `wire_drive.display_skip()` first, so a box with neither `$DISPLAY` nor `Xvfb` reads
`SKIP:` — a probe that goes red there is hiding the app's own failure behind an unavailable resource, and
until recently `prep_proof` and `coverage_view_probe` did exactly that.

Two tools measure and gate nothing. `mask_speed_probe.py` prints what a mask event and a marquee event
cost *against* a pixel event (a ratio, because an absolute millisecond on a shared box is a coin toss),
and `tn_probe.py` prints a server's latencies. Both are run by hand, and the seam table names them for
the number they produce, not as a check the suite makes.

## House rule for every probe

Starting the app means taking responsibility for it. With no `DISPLAY`, `wire_drive.start_app` raises its
own Xvfb on a display number it **claimed** first: creating `/tmp/.X11-lock/X<n>` with `O_EXCL` and the
owner's pid inside, and treating a lock whose pid is gone as stale. `xvfb-run -a` is not usable here — it
picks a number by *listing* `/tmp/.X*-lock` and then creating the one it saw free, so probes starting
together take the same number, and the loser spends its run against an X server the winner shut down
(`XIO: fatal IO error on X server`, a reset wire, and a probe reporting an app defect that never happened;
measured at 3 failures in 8 concurrent runs before the claim, 0 in 16 after).

A probe that injects events asks `wire_drive.ensure_display(width, height)` first: it claims and raises the
server at the size the app's window will want, points `$DISPLAY` at it, and the app started afterwards
shares that server — the injector and the app must be looking at the same X server or the events land on
nobody.

`wire_drive.start_app` registers each child and reaps its **process group** on exit and on SIGINT/SIGTERM,
Xvfb included, and releases the locks it claimed. Reaping is not enough, because a probe killed by
`SIGKILL` — an escalated test timeout, a closed terminal — runs no cleanup at all, and an editor holding a
framebuffer redraws at full CPU until someone notices. **`python3 tools/sweep_procs.py` is the back stop**:
it kills every `noise-edit` and `Xvfb` the user owns and exits 1 when it found any. Run it after a probe
session, after a timed-out `ctest`, and after any ad-hoc script that started the app; `--list` looks without
touching. An ad-hoc script is not exempt either: it cleans up in `finally` with `wd.shutdown(proc, wire)`.

## Driving the app from a terminal

`app.state` answers a live `AppState` and the wire port file is `<runtime>/wire-port`. `tools/wire_call.py`
sends one command to a running instance; `command.list` on the wire is the **only** reference for what
commands exist, what they take and what key is bound — no document restates it, because a restatement is a
second fact and will be the wrong one. `screenshot` and `image.get` exist so a vision model can look at the
result; prefer reading state the wire already exposes over adding prints.

## Still owed to the box

* One live run measuring the paired drift correction (`colour_match_drift`) on a real answer — the numbers
  in `probes/2026-10-11-ai-job-live.md` belong to the estimator that no longer exists.
* The recipe/prompt pass (~10 images) that picks prep, context ratio and steps per verb, which only makes
  sense once the verbs have their final names.
* The prep ablation (`none|average|blur|pixelate|white`), edit drift over a large region, and cancel
  mid-request. Multi-image transport and the recorded edit path are covered offline; live acceptance
  of multiple references and model use of each one still need an on-demand run.

## Records

`docs/probes/` holds dated measurements — what ran, what it cost, what it showed. They are never restated
elsewhere, and they carry no hostname, port or key: the box is `$THENOISE_URL`.
