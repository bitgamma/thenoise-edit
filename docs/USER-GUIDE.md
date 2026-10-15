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
┌──────────────────────────────────────────────┬─────────────────────┐
│                                              │ Brush               │
│   Canvas                                     │  brush  [B]         │
│   (checkerboard where the layer is           │  eraser [E]         │
│    transparent, one image on top)            │  move   [V]         │
│                                              │  select [M]         │
│                                              │ ─ selection ─       │
│                                              │  70x50  [All][None] │
│                                              │ ─ on the marked ─   │
│                                              │  Remove [Ctrl+Bksp] │
│                                              │  Fill   [Ctrl+Sh+F] │
│                                              │   prompt            │
│                                              │   [____________]    │
│                                              │  Change [Ctrl+Sh+C] │
│                                              │   instruction       │
│                                              │   [____________]    │
│                                              │  Generate [Ctrl+G]  │
│                                              │   prompt            │
│                                              │   [____________]    │
│                                              │   aspect            │
│                                              │   [16:9         ▼] │
│                                              │   style             │
│                                              │   [Generate     ▼] │
│                                              │  (brush fields only │
│                                              │   for painting tools)│
│                                              ├─────────────────────┤
│                                              │ Layers | Log        │
│                                              │  Layer 2    ●       │
│                                              │  Layer 1            │
│                                              │  [Add][Delete]      │
├──────────────────────────────────────────────┴─────────────────────┤
│ 450.0%  160 x 110  unsaved  select 24 px  sel 70x50  undo 0 / redo 0  4.6 ms  wire :35429 │
│ ai: Fill  Flux-2-Klein-9B-TheNoise  ~3.4 s  256x224  seed 0  j0001 succeeded  on 'Fill j0001', Ctrl+Z │
└──────────────────────────────────────────────────────────────────────┘
```

Panes are ordinary ImGui windows: drag a tab to dock them somewhere else, and the layout is
remembered in `imgui.ini` inside the runtime directory. The status bar's first
line reads out the zoom, the document size, its path (or `unsaved`), the tool and brush size,
`painting N dabs` during a live stroke, the undo/redo depth, the last frame time and the wire port;
its **second line is the AI's**: which model would answer the verb on screen, what it costs on this
box, the size a press would ask for, the seed that press would send, whether it would have to load a
model first, and the job that is running or waiting to land - and where its answer went. Enough to
reproduce a screenshot from the picture alone.

**There is no AI tab.** The verbs live beside the tools that draw the area they act on, and on the
keys - see [the verbs](#the-verbs).

**There is no New/Open dialog.** A document comes from a command-line argument or from the wire
(`doc.new`, `doc.open`). That is a missing piece of UI, not a hidden feature.

## Looking at the canvas

| Input | Does |
|---|---|
| Left-drag | paints on the active layer, or drags a marquee with the select tool |
| Right-drag | pans |
| Mouse wheel | zooms about the cursor, x1.10 per click |
| `F` | fit the document into the canvas pane |

Zoom is clamped (5% of fit up to 64x) and a fit leaves a 16 px margin. Panning past the image
is allowed for 48 px, so an edge is not a wall.

## Painting

`B` brush, `E` eraser, `V` move, `M` rectangular select, `Shift+M` elliptical select (see *A selection*). `[` and `]` step the size by x1.25 — the same keystroke means the same
thing at 4 px and at 400 px. `,` and `.` step **hardness** down and up, `Shift+,` and `Shift+.`
step **opacity**: the MVP gave four actions to two keys, so the modifier takes the second pair. A
step lands on the 5 % grid, so pressing `,` from a slider dragged to 0.73 gives 0.70 rather than
0.68, and both ends stop dead rather than going past 0 or 1. Hover a slider to see its keys;
`command.list` prints them all. The Brush panel holds the rest:

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

### Moving a layer

`V` picks up the **active layer** and drags it. Press somewhere, move, release: the layer slides
under the cursor and the gesture is one undo step. Offsets are whole document pixels — a move never
resamples, so nothing is blurred by dragging — and pixels pushed past the canvas edge are gone from
the layer, which `Ctrl+Z` brings back. The button-down position is the anchor, so where you press
does not matter, only how far you drag; a tremor smaller than half a pixel is not an edit at all, and
costs neither a step nor the unsaved mark. The layer under the cursor is the one that moves: with a
painting on the layer above, it stays where it is.

## A selection

`M` (or `Shift+M` for an ellipse), then drag. **The rectangle takes every document pixel it passes
over; the ellipse is the ellipse inscribed in the box you drag**, and its edge pixels are partly
selected in proportion to how much of each one the curve covers. The marching ants run along the
selected pixels, so what is outlined is exactly what is writable. The ants are drawn in screen pixels,
so they stay crisp at any zoom, and every side of the outline — including the edge of a hole and a
second, separate selection region — marches with the same phase and speed. Dragging past the edge still
selects up to the edge; a click selects one pixel; releasing without a marquee puts the selection down.

A soft edge is not a second kind of selection: a partly covered pixel is the same 8-bit coverage a
layer mask holds, so painting through an ellipse's edge lays on colour in the same proportion and
`add`/`subtract` combine curves and rectangles exactly as they combine boxes.

The selection belongs to the document and **limits every write to the pixels it covers**: painting,
erasing and moving a layer all leave the pixels outside it exactly as they were. A stroke that lands
only outside the selection changes no byte and costs no undo step, so mis-clicking against a selection
does not spend your history. Undo restores pixels and leaves the selection up — a selection is a
working state, not an edit, and is not in the undo chain.

*No selection* and *a selection that covers nothing* are different states: the first means the whole
canvas is writable, the second blocks every write, and `Ctrl+D` is what puts it down. `Ctrl+I` flips
the two, so inverting no selection selects the whole canvas and inverting twice returns what you had.

The Brush panel has the same three actions as buttons — **All**, **None**, **Invert** — and reports
the selection's size, how many pixels it touches and how much of the canvas it claims (`area`, the
coverage added up, which is what an ellipse measures). `select.rect {rect, mode}` and
`select.ellipse {rect, mode}` with `mode` one of `replace`, `add`, `subtract`, plus `select.all`,
`select.clear`, `select.invert` and `select.get`, are the wire
spelling, so a driver can put a selection down without a pointer and read back what the canvas window
agreed to.

A selection is saved into the `.noise` with the rest of the document, and a reopened document paints
as the one you closed: the plane is one byte per pixel (`selection.m8`), and a file that declares one
without carrying it, or carries one the wrong size, is refused rather than guessed at. Exporting a flat
PNG/JPEG/WebP ignores it — a selection is part of how you are editing, not part of a picture.
## A layer mask

**To layer mask** turns the selection into a mask on the active layer, and **From layer mask** puts
that mask back into the selection. Both are buttons in the Brush panel and commands
(`mask.from_selection`, `select.from_mask`), and both *copy*: the selection you converted is still
there afterwards, and neither costs an undo step.

A mask says **what shows**, not what exists. The layer's pixels are untouched - masking a layer and
then `Ctrl+Z`-ing everything back cannot fail, because there is nothing to fail to restore - so you
can keep painting under a masked-out area and the strokes are there when the mask moves. What changes
is the composite: the same picture the renderer shows, the flat export writes and `mergedimage.png`
holds inside the project.

A mask that covers nothing is a real state and hides the whole layer; removing the mask is what brings
the layer back. The panel says which layer carries one and how much of it shows
(`mask on 'Layer 1': 2295 px shown`), `layer.list` reports `mask` per row - `null` meaning none - and
a mask is saved in the `.noise` as its own plane, `layers/<n>/mask.m8`.

### Painting one

The Layers panel has a **mask** section for the active layer: **Add mask** puts a fully-shown mask on
it, and once there is one, **Remove**, **Show all**, **Hide all** and **Invert**. Below them, two
radio buttons say where strokes go: **paint: layer** writes colour as always, **paint: mask** writes
coverage. Aimed at the mask, the brush reveals it and the eraser hides it, with the same size, hardness
and opacity you set for painting - a soft brush lays a soft mask edge, and half opacity reveals half.

```text
mask.target {"plane": "mask"}   aim strokes at the mask, over the wire
mask.get                        where strokes go, and what the mask holds
```

There is no key for the aim yet: the radio buttons are the quick way, and `mask.target` is the way a
driver does it. Whatever set it, `app.state` reports `brush.paint_target` and a live gesture reports
the plane it has been writing since the button went down (`stroke.plane`), so a session can be read
rather than remembered.

One drag is one undo step, and `Ctrl+Z` puts the coverage back exactly - it does not touch the layer's
pixels, which is the same promise as before. A selection fences a mask stroke exactly as it fences a
colour one: what the brush did happens only where the selection reaches, and a half-selected pixel
moves half way.

Two counts are reported, because they answer different questions: **touched** is how many pixels the
mask reached, **shown** is how much of them it shows. An anti-aliased edge makes them differ,
and `mask.get` gives both (`covered` and `area`).

Aiming at a mask the layer does not have is refused rather than silently painting colour: a stroke
reports which layer it looked at and says to `mask.add` one. Filling and inverting a mask are undoable;
putting one on and taking it off are not steps, and taking one off drops the undo steps that named its
coverage, since there is nothing left to restore them into.

What is not here: enabling or disabling a mask without deleting it, applying it, viewing a mask on its
own, or a mask thumbnail to click. A mask still cannot be started from nothing but a selection or an
`Add mask`.

## Layers

The Layers panel lists them top-first, like every other editor. Each row is **an eye, the name and
an opacity slider**, with a **blend-mode dropdown** on the line under the name; clicking the name
selects the layer, and the selected layer is the one strokes land on, marked `active` in
`layer.list`. **The canvas never shows a single layer**: it
shows the *composite*, the stack flattened, which is also what a PNG export writes and what
`mergedimage.png` holds inside a project.

- The **eye** takes the layer out of the composite. Its pixels are untouched — hiding is not
  erasing — and it is still the layer a stroke lands on if it is the selected one, which is a
  good way to make invisible marks, so select what you mean to paint on.
- **Opacity** scales the whole layer where the stack is flattened. It is never baked into the
  layer, so a layer shown at 30 % is still painted on, edited and hidden at full strength, and
  dragging it back to 100 % gives you exactly the image you had.
- **The mode** says how the layer combines with what is *under* it: `normal` (the default) paints
  over, `multiply` and `color-burn` darken, `screen`, `lighten` and `color-dodge` lighten, `overlay`
  does both depending on the backdrop, and `darken`/`lighten` take the darker or lighter of the two
  per channel. A mode is a property of the layer, not of the brush: a stroke on a `multiply` layer
  still paints normally into that layer, and it is the layer as a whole that multiplies the stack.
  Two consequences are worth knowing before you reach for it: a layer with nothing under it looks
  the same in every mode, because there is nothing to blend with; and a mode is not an undo step, so
  `Ctrl+Z` after changing one takes back your last stroke and leaves the mode where you put it.
  Modes change colour and never coverage - a `multiply` layer hides what is under it exactly as
  often as a `normal` one does.
- `Add layer` inserts an empty layer directly above the active one and selects it. Empty, not
  white — an opaque new layer would hide everything under it.
- `Delete layer` removes the active one. The last layer standing is refused: it *is* the canvas.

Names come from `layer.add {name}`; there is no rename field in the panel yet. Modes are set the
same way visibility and opacity are: `layer.set {"index": 1, "mode": "multiply"}`, with the name
spelled as the specs spell it (`color-dodge`, hyphen) and refused, list attached, if it is not one
of the eight. The dropdown in the panel dispatches that same command, so a panel row and a script
cannot disagree about what a layer's mode is.

## Files

| Command | Key | Writes | Notes |
|---|---|---|---|
| `doc.save` | `Ctrl+S` | `.noise` only | to the document's path, or `{"path": …}`. Clears the unsaved mark |
| `doc.export` | — | one flat `.png` / `.jpg` / `.jpeg` / `.webp` | of the composite, extension deciding the format, with an optional `{"quality": 1..100}` for the lossy two. Takes neither the document's path nor its unsaved mark, because the project on disk is still the thing the document is |

`doc.save` to a non-`.noise` path is refused with a message that points at `doc.export`. A save
goes to `<path>.noise-tmp` and is renamed over the target, so a save that fails leaves the
previous file readable. Reading a flat image decides by its **bytes**, so a PNG renamed `.bin`
opens and a JPEG renamed `.png` opens as the JPEG it is; a file that is none of them is refused
naming the formats and the first bytes found. Writing decides by the extension, since a new file has
no bytes yet, and `quality` is refused on a PNG because PNG is lossless and the number would do
nothing.

A JPEG has no alpha channel, so exporting anything translucent to one composites it against white;
the answer says so (`discarded_alpha: true`) rather than losing the transparency quietly. WebP and
PNG keep alpha. All three are checked against Pillow rather than only against ourselves, in
`tools/image_interop_probe.py`: our encoder agreeing with our decoder would prove only that the two
agree.

Opening a flat image gives you a one-layer document; opening a `.noise` gives
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
call("select.rect", rect=[100, 100, 200, 150])   # every later write is clipped to this
print(call("select.get"))                        # bounds, covered count, canvas size
print(call("screenshot", name="demo")["path"])
PY
/tmp/…/noise-edit/captures/demo-c68a18bc770c.png
```

### Telling the editor where the server is

There is one configuration, and it is the environment:

```sh
export THENOISE_URL=https://<server>:13305/v1   # thenoise/lemonade on the LAN
export THENOISE_API_KEY=***                     # only if the server demands one
```

The status bar's AI line shows the session and starts it: a `Connect` button, `not connected` in red
until it works, and the resident model once it does. It connects **by itself at startup**, once, after the
window is on screen and whatever tab is showing. One attempt is the whole point: a server that is switched
off costs one connect timeout, not a retry every frame. A host that *refuses* answers instantly; a host
that swallows packets can hold that one frame for the connect timeout (5 s), which is the price of asking
instead of making you find it. After that the button is yours - `reconnect` re-resolves the roles, which
is what you want after the box downloads a model.

With no `$THENOISE_URL` the row says `no server: set $THENOISE_URL`, there is no button, and the log says
why once (`startup handshake: no server is configured`).

From a terminal, the same handshake (this is the status line's `Connect` button, not a side door - it is
the command table the UI calls):

```sh
python3 tools/wire_call.py server.connect
python3 tools/wire_call.py server.state            # what it resolved, and what is resident
```

### The verbs

Four operations, each an ordinary command with a key, listed under **"on the marked area"** in the Tool
panel because the panel is where the area gets drawn:

| Verb | Key | Needs marked | Asks you to type | What it does |
|---|---|---|---|---|
| **Remove** | `Ctrl+Backspace` | a selection (yours or the active layer's mask) | nothing | reconstructs the marked area; the area is the whole instruction |
| **Fill** | `Ctrl+Shift+F` | a selection | what should appear | reconstructs the marked area from those words |
| **Change** | `Ctrl+Shift+C` | a selection | what should change | changes the marked area in place, from those words |
| **Generate** | `Ctrl+G` | nothing (document scope) | the picture | makes a new image on a new layer, at the aspect and in the style chosen under it |

They are commands first and buttons second: `command.list` shows `ai.remove` bound to `Ctrl+Backspace`,
`ai.verbs` answers the same rows, and the panel asks the table for the labels and keys rather than
owning any. Select an area first (`M` rectangle, `Shift+M` ellipse, or paint a layer mask and pick it up
with `select.from_mask`) - Remove and Fill throw the marked pixels away, Change keeps them, and none of
them will act on a frame with nothing marked.

| What you see | What it is |
|---|---|
| a grey verb | the reason is its tooltip: "works on a selection and there is none", "no model fills the `edit` role: ...", "not connected" - a disabled verb is a sentence, never a mystery |
| the words box | one per verb that asks, under that verb, labelled with the field's own name (`prompt`, `instruction`) - so the sentence typed for Fill is visible where Fill will use it, not in a box belonging to whichever verb was last chosen. It starts empty because **an operation with no words is a coin toss**: press with the box blank and the run refuses, naming the recipe and the field. What is typed is committed as it is typed, and the key carries nothing of its own |
| `272x224`, model, `~3.4 s` | what a press would send, priced before it costs anything: the endpoint, the size the region compiler settles on (region + context, snapped to the model's multiple), which model would answer, and the box's own measured time for that size - or nothing at all when the box has not answered that size before, which is honest rather than a guess. All of it is geometry and table lookups: **marking an area encodes nothing**, and there is no preview step, because the only honest preview of an AI edit is the answer |
| `will load the model (resident: …)` | this press would swap the shared server; the popup asks, and says a swap costs tens of seconds. Nothing switches silently |
| `j0001 running`, then `on 'Remove j0001', Ctrl+Z` | the job, and where its answer went. **There is nothing to press**: a finished answer composites itself - feathered, colour-matched, pixel-locked - onto **its own new layer** directly above the one the region was flattened from, named for the verb and the job. Hiding it, blending it down, masking it or deleting it are ordinary layer operations, `Ctrl+Z` takes the layer and its pixels away together, and `job.apply` brings it back (it is idempotent: the answer is already there, it says so and adds nothing) |
| `aspect` | the frames this operation can be asked for, listed by the recipe and nowhere else: `canvas` first (the whole picture, which is every operation that is not a generation), then `1:1`, `3:4`, `4:3`, `16:9`. The choice sizes the request and the answer lands centred in that frame - **the pixels outside it are not touched by that run**. A recipe whose geometry already exists (a selection, a full-frame edit) has no dropdown, because there is nothing to choose: overriding pixels that exist would be a lie about the region |
| `style` | the recipes that stand in for this one at the same key and the same row, again listed by the recipe: `Generate`, `Photographic`, `New image (illustration)`. Choosing one changes which recipe the verb's words and key run, and the row says which is in force; **choosing runs nothing**. The three are interchangeable by load-time rule, so the words typed for one mean the same thing to all three |
| `seed 0` | the number in the request. It is on screen because it changes the answer, and it changes only through `ai.intent {seed: n}` - the panel shows the session's seed, so a number typed once is the number the next press sends |

**Two kinds of operation, and why only one of them smears.** *Reconstruct* (Remove, Fill) throws the
selected content away on purpose, so the uploaded picture has a blurred or flattened hole in exactly that
area while the surroundings go up sharp - they are the only instruction it gets. *Change in place* uploads
the region **exactly as it stands**: blur it and you have destroyed the thing you asked to change, which
is what an earlier build did and why its answers came back unrelated to the picture.
`tools/prep_proof.py` renders both uploads and asserts the difference whole-image.

### AI from a terminal

Everything the panel does is a command, so the whole path runs from a terminal too - which is how it is
tested. Point the app at a server, or at recordings of one, and the sequence is the same either way:

```sh
export THENOISE_URL=https://<server>:13305/v1 THENOISE_API_KEY=*** # the dev box
export NOISE_AI_FIXTURES=$PWD/tests/fixtures/http                # or replay instead of reaching the net
```

| Command | What it does |
|---|---|
| `server.connect` | `GET /health` then `GET /models`, then resolves every role against what this box actually has; answers the version, how many models, and the resolution of each role |
| `server.state` | the session as the app believes it: server, key present, which URL prefix answered, what is resident, whether the tables loaded, the latency estimates |
| `server.load {model, pin?}` | make one model resident. Explicit and priced: a swap is tens of seconds on a shared box, so it is never silent |
| `ai.roles` | each role with the model it resolved to, the reason, the fix command when the box offers one, and the candidates |
| `ai.recipes` | every recipe with its endpoint, role, operation, region, prep and composite policy, and the model its role resolves to |
| `ai.verbs` | the verb rows as the panel sees them: id, command, key, recipe, label, operation, the fields each asks for, its scope, whether it could run and why not, the model it would use and whether that would mean a swap, plus the `aspects` that recipe offers, the `variants` that stand in for it (id and label - what the row draws) and which recipe a press would actually run (`active`) |
| `ai.remove`, `ai.fill`, `ai.change`, `ai.generate` `{vars?, seed?, confirm?}` | one verb pressed. Each names its recipe to `ai.intent` with `run` set, so the key and a scripted intent are the same call sequence; the table is checked against the recipes at startup and a verb that no longer matches its recipe is refused |
| `ai.intent {recipe?, vars?, aspect?, seed?, job?, run?, confirm?, connect?}` | the action as a command: read it with no arguments, as often as you like: it answers the intent, the readiness, the session, the job and a **quote** (endpoint, size, model, measured ~s) computed without touching pixels - `upload_encodes` counts every region prep + encode the run has done, and reads never move it and it answers the intents on offer, the values in force, whether the action could run and why not, the compiled request, the job, and the session (`connected`, `configured`, `connect_attempted`). `run` is the Run button; `confirm` accepts a model swap; `connect` is the Connect button - the handshake, with the refusal in `error` if the server is down or unconfigured. `aspect` is an `"W:H"` string from that recipe's `aspects`, or `null` for the canvas; the read answers `aspects`, the `aspect` in force and the `family` of recipes the one key could run, each with the model its role resolves to and whether choosing it would mean loading a different one. Choosing a recipe drops an aspect the new one does not offer rather than applying a frame it never had |
| `ai.preview {recipe, vars?, aspect?, seed?, steps?, cfg_scale?}` | the exact request: endpoint, ordered field list, resolved size (for the aspect chosen, when the recipe offers aspects), the prepped PNG that would be uploaded and its blob hash. **Sends nothing** — this is what to look at before spending minutes of someone's GPU |
| `ai.run {recipe, vars?, aspect?, seed?, steps?, cfg_scale?}` | queue it. Answers the job with the region plan and the prepped file |
| `job.list`, `job.get {id}` | every job of **this run**: state, timings, model, blob digest, answer path, landing metadata. The queue is in memory and nothing is read back, so a fresh session lists nothing and a re-ask is a second request |
| `job.wait {id, timeout?}` | block until the job is terminal or the wait runs out |
| `job.cancel {id}` | stop it; an answer that already arrived is kept, and its path is in the refusal. There is no `job.retry` — pressing the verb again *is* the retry, and it re-sends |
| `job.apply {id, feather?, colour_match?}` | the idempotent landing door. Answers land by themselves when they finish, so this is for bringing one back after an undo, or for overriding the recipe's feather or colour match on the way: if the layer it made is still on the document it answers `already_landed` with the landing's own report and adds nothing. No `apply` argument any more - there is no preview mode, the layer is the preview - and no destination, ever |

`seed` is optional on `ai.preview`, `ai.run` and `ai.intent`: absent means the session's seed -
`server.state` says what that is, `ai.intent {seed: n}` sets it, and the status line reports it. Nothing draws
a random number inside a request path, so "run that again" gives the same picture.

Where an option is optional, leaving it out means **the recipe's policy**, not "off": `ai.compose` and
`job.apply` without `colour_match` land with the correction the recipe asked for, and the report's
`policy` block says what was actually used, so the claim is checkable rather than implied.

Two environment variables decide where it goes: `$THENOISE_URL` / `$THENOISE_API_KEY` for the real
server, and `$NOISE_AI_FIXTURES` pointing at a directory of recordings — set the second and the app
replays instead of reaching the network, which is how the default test suite proves the whole path.
`$NOISE_EDIT_ASSETS` says where `roles.json` and `recipes/` live (otherwise: `assets` beside the app, then
the runtime directory). A URL or a key never enters a document, a log line that is written to disk with
the repo, or this file.

`tools/ai_job_probe.py` runs exactly that sequence against a live app and asserts the parts that are ours:
the request obeyed the size policy and sent no `mask` field, the pixels that changed are the ones the
report counted, nothing outside the job's coverage moved, and undo gives the document back byte for byte.
With no `--live` it uses the committed recordings, so `ctest` checks the whole path without a socket.

That is the session that was run against this build, not a sketch of one: a `doc.new`, a red
stroke and a capture, and the PNG turned up where it said.

| Group | Commands |
|---|---|
| app | `app.state` `app.quit` `command.list` `command.invoke` `log.tail` `log.level` `wire.info` |
| document | `doc.new` `doc.open` `doc.save` `doc.export` `doc.get` `image.get` |
| layers | `layer.list` `layer.add` `layer.remove` `layer.select` `layer.set` |
| paint | `tool.brush` `tool.eraser` `tool.move` `tool.select` (one per tool row) `tool.set` `tool.stroke` `tool.pointer` `brush.larger` `brush.smaller` |
| selection | `select.rect` `select.ellipse` `select.all` `select.clear` `select.invert` `select.from_mask` `select.get` |
| masks | `mask.from_selection` `mask.add` `mask.remove` `mask.fill` `mask.invert` `mask.target` `mask.get` |
| view | `view.fit` `view.set` `view.pan` `view.zoom` |
| edit | `edit.undo` `edit.redo` |
| observe | `screenshot` `pixels.compare` |
| ai | `ai.compose` |

Things worth knowing when writing a driver:

- `command.list` returns every command with its title, shortcuts and **arguments**; that and
  `app.state` are what a test asserts against rather than a copy of this table.
- An argument a command does not take is **refused**, naming the argument and the names that would
  have worked. So `tool.brush {"size": 12}` is an error, not a silently ignored field: `tool.brush`
  selects the tool and takes nothing, brush fields belong to `tool.set`.
- `tool.stroke` takes a whole polyline, `tool.pointer` feeds the same path one
  `press`/`drag`/`release` event at a time, and both are the real stroke path — so a recorded
  gesture replays byte-identically. Any brush field you send overrides the session value, and
  the ones you omit come from what the app is on.
- `screenshot` captures the framebuffer, `image.get` returns document pixels; both write a PNG
  into `<runtime>/captures` (or return base64 with `{"base64": true}`) and report the path and
  sha256. `pixels.compare {a, b}` decodes two images of any flat format and diffs their pixels, reporting
  `differing_bytes` / `max_channel_delta`.
- `ai.compose {"image": path}` takes a PNG as an answer to a request the app has not sent yet: it lands where
  the **selection** is (no selection means the whole frame), feathered, colour-matched and blended — so
  `feather` is an integer or `"auto"`, `colour_match` and `lock_band` override what a recipe would have said.
  By default it **previews**: nothing on the document changes and it writes a PNG of what the new layer would
  hold. `{"apply": true}` adds that layer above the active one and writes the answer into it as **one undo
  step**, so `Ctrl+Z` removes the answer and the layer together and puts every other byte back. The reply
  reports the resolved policy, the coverage that was actually pasted, `pixels_changed`, the `layer` and its
  `layer_name`, and `answered_as_requested`, so "everything outside the coverage is untouched" is a thing a
  driver can check rather than admire. Note the counts: `pixels_changed` is what was *written*, and a
  flattened export can differ in fewer pixels than that when the answer reproduced something exactly.
- `Escape` and `Ctrl+Q` both quit, and nothing asks about unsaved changes.

The scripts in `tools/` are the same wire, already wired into `ctest`:
`wire_drive.py` (pan+zoom, capture, golden diff), `wire_input.py` (injected mouse and keyboard
gestures, `Ctrl+Backspace` on a dragged marquee included), `stroke_probe.py` (a 5000-dab stroke with the app still drivable),
`container_probe.py` (a project read back by Python's `zipfile`),
`ai_compose_probe.py` (an answer image landing on a real document, with the report checked against the
pixels the document actually shows),
`image_interop_probe.py` (our PNG/JPEG/WebP read by Pillow and Pillow's read by us), `tn_probe.py`
(the live AI server; see `AI-BACKEND.md`).

## What does not exist yet

Stated plainly, because "the button is missing" and "the feature is broken" are different
problems and only one of them is a bug:

- **No answers yet for `Heal`, `Upscale`, `Reimagine`, `Colorize the flattened frame` or the two image
  styles** (`tools/prep_proof.py` names them: they need a `super_resolution` model and a stronger
  inpainting model than the box has). They are recipes, so they are drivable and observable on the wire -
  `ai.run {recipe: "heal"}` etc. - and a box with the right models would answer them. Roles, jobs, the
  cache and the model swap are all reachable from the same table - see "AI from a terminal" - and
  `docs/AI-BACKEND.md` says what else is designed and not built (draft→refine stages, the variants tray,
  A/B wipe, the journal panel, warm-up).
- **One seed field went with the old panel**: the seed shows on the status bar and changes through
  `ai.intent {seed: n}`; there is no box for it in the window yet.
- **No jobs tray.** One job at a time is shown in the status bar; queueing variants (`seed+i`) and comparing
  them side by side is designed and not built.
- **Nothing local replaces a server.** If the `edit` role cannot resolve, the edit recipes refuse with
  the reason. The in-process fallbacks in the design (Telea/Chan `heal`) are not built.
- **Selections are rectangular and elliptical only.** No lasso, wand, feather or grow/shrink.
  Masks can be painted (`mask.target`), but there is nothing to enable, disable, apply or view one
  with, and no thumbnail to click.
- **No reorder, rename, duplicate or merge** in the Layers panel.
- **Four tools**: brush, eraser, move and select. No fill, eyedropper, lasso or wand, no pen pressure.
- **No `.ora`/PSD at all**: OpenRaster is out of the design by decision, so a foreign layered file
  is refused naming its mimetype rather than guessed at.
- **No autosave or crash recovery**, and the undo history lives and dies with the session.
