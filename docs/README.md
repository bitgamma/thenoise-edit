# Docs

**[`AGENTS.md`](../AGENTS.md) is normative**: ground rules for how this codebase is built and
maintained (architecture over compatibility, tests that can actually fail, the debug wire, one
fact per home, minimal comments, LLM-friendly code, when to ask the human). Where a doc here
and AGENTS.md disagree, AGENTS.md wins.

| Doc | What's in it |
|---|---|
| [MVP.md](MVP.md) | scope, P0 feature set, explicit non-goals, feature-availability matrix, milestones, UI layout, risks, decision log, open questions |
| [ARCHITECTURE.md](ARCHITECTURE.md) | stack + module layout, threading, compositing, painting path, undo/blob store, file format (`.noise` vs OpenRaster/PSD), locked design decisions |
| [AI-BACKEND.md](AI-BACKEND.md) | the Lemonade/`thenoise` contract: supported models & roles, field tiers (and why `mask` is never sent), region compiler, result compositor, draft/refine flow, recipes, error handling, the probe harness |

Reading order for a contributor: **MVP.md → AI-BACKEND.md → ARCHITECTURE.md**.

The one idea that ties it together: the backend is a dumb `image + words → image`
function, so *all spatial precision lives in the client* — crop, prep, prompt, resample,
composite under a feathered mask. The Noise document format exists mainly so that
generation provenance (prompt, model, seed, timings) is a first-class file-format feature
and so that undoing a multi-minute generation costs a pointer swap.
