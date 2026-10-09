# `.noise` v0: what a save costs, and who can read it

Measured 2026-10-09 on the dev box: Debug build, Xvfb + llvmpipe, driven by
`tools/container_probe.py` (ctest `test_container_interop`). These are records; the only bound in
the test is a 20 s save, which is a regression guard rather than a target.

| document | on disk | `doc.save` | `doc.open` |
|---|---|---|---|
| 40×24, solid fill | 1.5 KiB | 2.2 ms | 2.2 ms |
| 2048², one soft 90 px stroke across the diagonal | 293 KiB | 570 ms (917 ms worst run) | 220 ms (323 ms worst) |

## What the numbers ruled in and out

* **A save encodes the layer PNG twice** — `layers/0/layer.png`, then `mergedimage.png`, which is
  what a reader that does not know our format looks at. That is the bulk of the 570 ms, and it is
  accepted because saving is explicit (`Ctrl+S`) and never happens on the frame path. When M2
  makes it *N+1* encodes it stops being acceptable: the fix then is one composite encode plus a
  per-layer PNG only for layers that changed, which needs the dirty set the compositor is going to
  have anyway. Nothing is built for that now.
* **Nothing of the paint path is on the save path**: 570 ms is PNG encoding plus one `write` and
  one `rename`. So `doc.save` can stay a command the UI may call at any time without a progress
  affordance below roughly 4096², and the status bar's unsaved mark is all the feedback it needs.
* **293 KiB for a 2048² layer holding one stroke** means container overhead is the PNG's own:
  members are stored, not deflated, and PNG payload is already deflate. Deflating members would buy
  nothing and would break the sniffing rule that member 0 (`mimetype`) be stored at a fixed offset.

## The finding that matters

The first writer put the wrong size on the central directory in the end-of-directory record: it
measured the directory *after* starting to append the end record, so the size came out 12 bytes too
large. `unzip -l` listed that archive happily; Python's `zipfile` refused it with
`BadZipFile: Bad magic number for central directory`. Our own reader accepted it too, because it
used the directory's offset without checking that `offset + size` lands where the end record sits.

Two checks hold the format to something outside itself:

* `tests/test_zip.cpp` parses the end record by hand and refuses a directory whose extent does not
  reach the end record, and refuses trailing bytes after it.
* `tools/container_probe.py` reads a project the app saved with `zipfile` — not our code — asserts
  `mimetype` is member 0 and stored uncompressed, decodes `layers/0/layer.png` with Pillow and
  checks its pixels, and round-trips one painted edit through save and reopen.

The rule to keep: **a writer is tested by a reader that is not ours.** Our reader and our writer
share a misunderstanding, and a shared misunderstanding never turns a test red.
