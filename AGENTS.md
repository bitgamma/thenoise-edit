# AGENTS.md — ground rules

noise-edit: a layered raster editor whose AI operations are ordinary editing commands with
a compile step in the middle. C++20, sokol, Dear ImGui, Lemonade/`thenoise` backend.
Design docs: [docs/README.md](docs/README.md). Read `docs/AI-BACKEND.md` before touching `ai/`.

## 1. Architecture beats compatibility

Clean architecture outranks backwards compatibility and API stability. Internal APIs,
document formats, wire protocols and module boundaries may all be broken to get the design
right — in the pre-1.0 phase there is exactly one consumer of every interface and it is us.

- Never add a shim, alias, deprecated overload, `[[maybe_unused]]` compat path or versioned
  duplicate of a type to keep an old caller working. Change the caller.
- `.noise` files from earlier sessions are disposable. When the format changes, bump
  `manifest.json:format`, refuse to open older versions, and say which.
- When a refactor is right but large, say so and do it in one pass. Do not do the small wrong
  thing because it is smaller.

## 2. Tests must be worth their weight in gold

Tests are the backbone of confidence, and a weak test is worse than none: it spends runtime
and review attention to assert something that cannot break.

Write a test only if a plausible bug would turn it red. Reject tests that:

- restate a constant (`EXPECT(kMaxLayers == 64)`), mirror the implementation, or assert a
  constructor stored what it was handed;
- round-trip a value through the type that already guarantees it;
- only prove code compiles / does not crash / returns non-null;
- pass for structural reasons (a mock that returns what the test configured, then asserts it).

Prefer properties and invariants that constrain behaviour from the outside. The load-bearing
ones for this project:

- **pixel-lock**: after a composite, every byte outside the feathered mask is identical to
  the source. Whole-image diff, not a sampled point.
- **region compiler round-trip**: for arbitrary crops, sizes, scales and off-canvas masks,
  compile → inverse-map is exact, and output dims always satisfy `size_policy`.
- **blend equivalence**: CPU blend reference == GPU blend shader, over a golden pixel set
  including zero-alpha, full-overlap and extreme values.
- **field policy**: a built request contains exactly the whitelisted fields. No test doubles
  in the request path — assert on the serialized bytes.
- **golden images**: named fixtures in → exact bytes out, for codecs, mask ops, compositor.
- **recorded backend** (always on): `ai/` runs against committed HTTP fixtures — request field
  policy, response parsing, error mapping, cache keys. The default `ctest` run is hermetic
  and must never touch the network.
- **live backend** (on demand, tagged `live`): these are wanted, not exotic. The dev
  Lemonade server is reachable when the human exports `THENOISE_URL` and `THENOISE_API_KEY`
  — **ask for them whenever a change needs live verification**, naming the tests and the
  approximate image count it will spend. Without the env: `skip` with a loud reason, never a
  silent pass and never a spurious failure.
- Live tests assert **contract and invariants, never model output**: roles resolve, output
  dims satisfy `size_policy`, pixel-lock outside the mask holds exactly, status codes and
  error shapes, cancel works. Timings are recorded to `docs/probes/`, never asserted.
- The box is shared and a model swap costs minutes: live tests stay small (small sizes,
  default steps, a per-run image cap) and restore any model they switched.

Testability is a design input: if a behaviour is hard to test, the seam is in the wrong
place — fix the seam. Anything non-deterministic takes a seed and a clock as parameters.

## 3. Give yourself a way to see

The debug wire is not optional and is not deferred. If you cannot observe a behaviour you
will guess, and guessing is how an agent breaks a codebase.

- The wire (spec: `docs/ARCHITECTURE.md` §Debug wire) exists from M0 and grows with the app.
- **Every new feature ships the wire command that makes it observable and controllable.**
  No exception, treat its absence as an incomplete change.
- Dispatch through the same string-keyed command table the UI uses. If the UI can't do it via
  the command table, neither can you — that is a finding, not a reason to add a side door.
- `screenshot` and `image.get` exist so a vision model can look at the result. Verify
  visually: render, capture, inspect, and compare against what you expected.
- Prefer observation over instrumentation: read the state the wire already exposes rather
  than adding `printf`s and re-running.
- When you must instrument: `LOG` levels through the wire, never ad-hoc `fprintf(stderr, …)`.

## 4. One fact, one home

Duplicating values and defaults is the fastest way to make an LLM-maintained codebase lie.

- **No default arguments.** Every parameter is explicit at every call site. Defaults live in
  exactly one place — the relevant table (roles, recipes, size policy, prefs). A function
  that "has a sensible default" without being given one is a bug.
- No magic numbers in code: a named constant in the module that owns the concept, or a field
  in a data table. If a number appears twice, it needs a home.
- Single sources of truth, and nothing else may restate them:
  | Concept | Home |
  |---|---|
  | commands + keyboard shortcuts | the command table |
  | model capabilities & assignment | `roles.toml` + `/v1/models` (resolved in `ai/registry`) |
  | AI request fields allowed | `ai/provider` field policy (the only place a field can be added) |
  | prompt templates & region/prep/composite policy | `recipes/*.toml` |
  | document model & defaults | `core/document` |
  | blend mode maths | `core/blend` reference + the shader pair, checked by a test |
  | UI prefs / session state | `io/config`, `manifest.json` |
- Derived values are computed, never cached in two places. If a cache is needed, one owner
  and an explicit invalidation path.
- Enums are exhaustive `switch`es with no `default`, so adding a case breaks the build and
  nothing hides an unhandled state.
- No parallel representations of one idea (e.g. a second enum, a bitmask mirror, a stringly
  typed shadow of a type that already exists).

## 5. Comments: only what the code cannot say

Comments and docstrings are reduced to the minimum and are not a log of the work.

- Never write comments that restate the code, name the caller, or explain task context
  (`// used by the remove flow`, `// added for M4`, `// TODO: revisit`). History belongs to
  git; intent belongs to commit messages and docs.
- A comment must carry information that is not derivable from the code: an invariant the
  reader cannot check locally, a hardware/backend constraint, a measured number with a
  reason, or a deliberate trade-off.
- Prefer a name or a type over a comment. Prefer a test over a promise.
- Document invariants at the type/contract boundary, not at each use site.
- No comment blocks around obvious code, no separators, no file headers beyond the SPDX line.

## 6. This codebase is maintained by LLMs

Human-friendliness is not a requirement; LLM-friendliness is: grep-able, locally
comprehensible, and honest about its state.

- One concept per file, files under ~500 lines, functions under ~60.
- Boring C++20: no macros beyond build/system ones, no template metaprogramming, no CRTP
  ladders, no expression templates, no SFINAE puzzles, no overloading-by-implication.
  Simple code a reader can hold entirely in context beats clever code.
- Data-driven over control-flow: tables and declarative files (`recipes/*.toml`,
  `roles.toml`) instead of branching trees. Adding a behaviour should mean adding a row.
- Names are domain terms from the docs (`Document`, `Layer`, `Mask`, `RegionPlan`,
  `Recipe`, `ServerProfile`, `Role`). Never invent a synonym for a documented term.
- Machine-readable everything observable: state dumps, job records, request previews, logs.
  Errors name the thing: file path, blob hash, command id, recipe id, job id, model id,
  field name. Never `invalid argument` without saying which.
- Deterministic by construction: explicit seeds, explicit clocks, no wall-clock reads or
  `rand()` inside logic that a test can reach.
- Fail loudly and early (assert/log/return error) rather than silently coercing.
- Keep `docs/` true. When a doc claim stops matching the code, fixing the doc is part of the
  change that broke it.

## 7. The human is a software engineer — spend their judgement

- If you catch yourself deliberating between two designs for more than a few minutes, stop
  and ask: state both options, give a recommendation and the reason, ask one question.
- Always ask before: a new dependency; a change to `.noise`, the debug wire protocol, the
  field policy, or the P0 scope in `docs/MVP.md`; swapping models on the shared server for
  anything other than a test that restores it; anything network-touching beyond the
  configured server.
- Request, don't perm: needing the live server is a resource request ("export
  `THENOISE_URL`/`THENOISE_API_KEY`, I'll spend ~N images"), not a design approval question.
- Never ask about: formatting, naming within an established convention, adding tests, dead
  code removal, or anything the docs already decide.
- Prefer a working, reversible, observable step over a large speculative one. Ship the slice,
  observe it on the wire, then continue.
- Report what you actually observed (commands run, tests red/green, screenshots), never what
  you merely expect.

## House rules

- Apache-2.0. One `// SPDX-License-Identifier: Apache-2.0` line at the top of every source
  file; nothing else in the header.
- Build: `cmake -B build && cmake --build build && ctest --test-dir build`. Warnings are
  errors (`-Wall -Wextra -Werror`); a clean build is part of "done".
- Platform code lives only in `src/platform/` behind one header; `src/core/` imports nothing
  but the standard library and stays headless.
- Portability is by design even where we only build for Linux today: no POSIX-only calls
  outside the shim, UTF-8 internally, `std::filesystem` at the boundary.
- No network access at build time, and none in the default test run. Live-tier tests are
  gated on `THENOISE_URL`/`THENOISE_API_KEY`. Vendored/FetchContent deps only, pinned.
- Commits: imperative subject, one concern each; body says why. Never commit an unsigned
  build-breaking tree, and never commit generated files.
- Definition of done: builds warning-free · tests added that would fail if the feature broke ·
  debug-wire command for any new observable state · docs still true · no new duplicated
  constants or default arguments.
