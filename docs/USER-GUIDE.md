# User guide

What the app does today, how to drive it, and what is deliberately not there yet. Written
against the current tree; `command.list` is the truth about commands and keys if this page and
the binary disagree.

## Build and run

```sh
cmake -B build -G Ninja && cmake --build build      # -> build/noise-edit
./build/noise-edit                                  # empty window, no document
./build/noise-edit some.png                         # opens it as one layer
./build/noise-edit project.noise                    # opens the stack
```

| Option | Effect |
|---|---|
| `--width=N` / `--height=N` | window size, default 1280x800 |
| `--debug-wire=N` | debug wire port; `0` (the default) picks a free one |
| `--no-debug-wire` | do not open the wire at all |
| `--help` | usage |

A headless box needs a display: `xvfb-run -a ./build/noise-edit`. Everything the app writes
goes to one directory — `$NOISE_RUNTIME_DIR`, else `$XDG_RUNTIME_DIR/noise-edit`, else
`~/.cache/noise-edit` — and it holds `wire-port`, `imgui.ini` and `captures/`. If
`XDG_RUNTIME_DIR` is not writable, set `NOISE_RUNTIME_DIR` somewhere that is.

## The window

One dockspace, four panes and a status bar:

```
┌──────────────────────────────────────────────┬────────────────┐
│                                              │ Brush          │
│   Canvas                                     │  brush  [B]    │
│   (checkerboard where the layer is           │  eraser [E]    │
│    transparent, one image on top)            │  size  ───●─── │
│                                              │  hardness …    │
│                                              ├────────────────┤
│                                              │ Layers         │
│                                              │  Layer 2    ●  │
│                                              │  Layer 1       │
│                                              │  [Add][Delete] │
│                                              ├────────────────┤
│                                              │ Log            │
├──────────────────────────────────────────────┴────────────────┤
│ 72.5%  1600 x 1000  ~/sketch.noise  brush 24 px  undo 3 / redo 0  5.7 ms  wire :41823 │
└───────────────────────────────────────────────────────────────┘
```

Panes are ordinary ImGui windows: drag a tab to dock them somewhere else, and the layout is
remembered in `imgui.ini` inside the runtime directory. The status bar reads out the zoom, the
document size, its path (or `unsaved`), the tool and brush size, `painting N dabs` during a
live stroke, the undo/redo depth, the last frame time and the wire port — enough to reproduce a
screenshot from the picture alone.

**There is no New/Open dialog.** A document comes from a command-line argument or from the wire
(`doc.new`, `doc.open`). That is a missing piece of UI, not a hidden feature.

## Looking at the canvas

| Input | Does |
|---|---|
| Left-drag | paints on the active layer |
| Right-drag | pans |
| Mouse wheel | zooms about the cursor, x1.10 per click |
| `F` | fit the document into the canvas pane |

Zoom is clamped (5% of fit up to 64x) and a fit leaves a 16 px margin. Panning past the image
is allowed for 48 px, so an edge is not a wall.

## Painting

`B` brush, `E` eraser. `[` and `]` step the size by x1.25 — the same keystroke means the same
thing at 4 px and at 400 px. The Brush panel holds the rest:

| Field | Means |
|---|---|
| `size` | dab diameter in document pixels, 1 … 4096 |
| `hardness` | fraction of the radius painted at full strength; the rim fades |
| `opacity` | the whole stroke's opacity — a second pass over the same pixels does not darken it |
| `flow` | each dab's opacity, so a slow stroke *does* build up |
| `spacing` | gap between dab centres as a fraction of the diameter |
| `color` | RGBA, alpha included |

One drag is one undo step, and while it is held down what you see is the stroke's real
result, not a preview: the gesture accumulates coverage, and every frame repaints from the
pixels it found. `Ctrl+Z` / `Ctrl+Y` undo and redo.

**Undo covers pixels only.** Hiding or fading a layer, adding one, and deleting one are not
undoable — and deleting a layer discards the steps made on it, because a change to a layer that
is gone cannot be put back.

## Layers

The Layers panel lists them top-first, like every other editor. Each row is **an eye, the name,
an opacity slider**; clicking the name selects the layer, and the selected layer is the one
strokes land on, marked `active` in `layer.list`. **The canvas never shows a single layer**: it
shows the *composite*, the stack flattened, which is also what a PNG export writes and what
`mergedimage.png` holds inside a project.

- The **eye** takes the layer out of the composite. Its pixels are untouched — hiding is not
  erasing — and it is still the layer a stroke lands on if it is the selected one, which is a
  good way to make invisible marks, so select what you mean to paint on.
- **Opacity** scales the whole layer where the stack is flattened. It is never baked into the
  layer, so a layer shown at 30 % is still painted on, edited and hidden at full strength, and
  dragging it back to 100 % gives you exactly the image you had.
- `Add layer` inserts an empty layer directly above the active one and selects it. Empty, not
  white — an opaque new layer would hide everything under it.
- `Delete layer` removes the active one. The last layer standing is refused: it *is* the canvas.

Names come from `layer.add {name}`; there is no rename field in the panel yet, and blend modes do
not exist yet either — every layer composites `normal`.

## Files

| Command | Key | Writes | Notes |
|---|---|---|---|
| `doc.save` | `Ctrl+S` | `.noise` only | to the document's path, or `{"path": …}`. Clears the unsaved mark |
| `doc.export` | — | one flat `.png` | of the composite. Takes neither the document's path nor its unsaved mark, because the project on disk is still the thing the document is |

`doc.save` to a non-`.noise` path is refused with a message that points at `doc.export`. A save
goes to `<path>.noise-tmp` and is renamed over the target, so a save that fails leaves the
previous file readable. Opening a PNG gives you a one-layer document; opening a `.noise` gives
you the stack, plus any members this build does not understand, which are written back
unchanged so a newer file cannot lose what an older app cannot show.

`.noise` is a ZIP you can poke at:

```
mimetype              "application/x-noise+zip", stored, first member
manifest.json         format, size, layers[{name, src, visible, opacity, mode}]
layers/0/layer.png    one PNG per layer, bottom-first
mergedimage.png       the flattened composite
```

A file whose `format` is not what this build writes is refused, naming both the version it met
and the version it speaks. Files from earlier sessions are disposable.

## Driving it from a terminal

The debug wire is a loopback JSON-lines server, and it has no powers the UI does not: every
frame is a command id from the same table the buttons and keys use, executed on the render
thread. Bind is `127.0.0.1`, port ephemeral unless `--debug-wire=N`, published in
`<runtime>/wire-port` and in the log. Up to 4 clients, 8 MiB per frame.

```sh
$ RT="${NOISE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-$HOME/.cache}/noise-edit}"
$ cat "$RT/wire-port"
41823
$ python3 - <<'PY'
import json, socket
s = socket.create_connection(("127.0.0.1", 41823)); f = s.makefile("rwb")
def call(cmd, **args):
    f.write((json.dumps({"id": 1, "cmd": cmd, "args": args}) + "\n").encode()); f.flush()
    reply = json.loads(f.readline())
    assert reply.get("ok"), reply
    return reply["result"]
call("doc.new", width=512, height=512)
print(call("app.state")["document"]["width"])
call("tool.stroke", points=[[64, 64], [400, 380]], size=32.0, hardness=0.8,
     opacity=1.0, flow=1.0, spacing=0.25, color=[220, 40, 40, 255])
print(call("screenshot", name="demo")["path"])
PY
/tmp/…/noise-edit/captures/demo-c68a18bc770c.png
```

That is the session that was run against this build, not a sketch of one: a `doc.new`, a red
stroke and a capture, and the PNG turned up where it said.

| Group | Commands |
|---|---|
| app | `app.state` `app.quit` `command.list` `command.invoke` `log.tail` `log.level` `wire.info` |
| document | `doc.new` `doc.open` `doc.save` `doc.export` `doc.get` `image.get` |
| layers | `layer.list` `layer.add` `layer.remove` `layer.select` |
| paint | `tool.brush` `tool.eraser` `tool.set` `tool.stroke` `tool.pointer` `brush.larger` `brush.smaller` |
| view | `view.fit` `view.set` `view.pan` `view.zoom` |
| edit | `edit.undo` `edit.redo` |
| observe | `screenshot` `pixels.compare` |

Things worth knowing when writing a driver:

- `command.list` returns every command with its title and shortcuts; that and `app.state` are
  what a test asserts against rather than a copy of this table.
- `tool.stroke` takes a whole polyline, `tool.pointer` feeds the same path one
  `press`/`drag`/`release` event at a time, and both are the real stroke path — so a recorded
  gesture replays byte-identically. Any brush field you send overrides the session value, and
  the ones you omit come from what the app is on.
- `screenshot` captures the framebuffer, `image.get` returns document pixels; both write a PNG
  into `<runtime>/captures` (or return base64 with `{"base64": true}`) and report the path and
  sha256. `pixels.compare {a, b}` diffs two PNGs byte for byte and reports
  `differing_bytes` / `max_channel_delta`.
- `Escape` and `Ctrl+Q` both quit, and nothing asks about unsaved changes.

The scripts in `tools/` are the same wire, already wired into `ctest`:
`wire_drive.py` (pan+zoom, capture, golden diff), `wire_input.py` (injected mouse and keyboard
gestures), `stroke_probe.py` (a 5000-dab stroke with the app still drivable),
`container_probe.py` (a project read back by Python's `zipfile`), `tn_probe.py` (the live AI
server; see `AI-BACKEND.md`).

## What does not exist yet

Stated plainly, because "the button is missing" and "the feature is broken" are different
problems and only one of them is a bug:

- **No AI at all.** No server profile, no roles, no generate / remove / replace / upscale.
  That is milestones M3 and M4.
- **No selections and no masks.** Nothing to select with, nothing to mask a layer with.
- **No blend modes.** Every layer composites `normal` over what is under it; the mode set
  (`multiply`, `screen`, …) is the next increment on the layer model.
- **No reorder, rename, duplicate or merge** in the Layers panel; no move tool.
- **Only brush and eraser.** No fill, eyedropper, lasso or wand, no pen pressure.
- **PNG in, PNG out** — no JPEG/WebP import or export, no ORA/PSD.
- **No autosave or crash recovery**, and the undo history lives and dies with the session.
