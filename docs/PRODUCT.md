# PRODUCT — what noise-edit is

One page. If a change does not serve this, it is not in scope. `docs/PLAN.md` holds the order of work;
`docs/ARCHITECTURE.md` holds how it is built; `docs/TESTING.md` holds how it is proven.

## In one sentence

An image editor that is easy to use because you tell it what you want, where you want it, in words - and
every answer arrives where nothing you made is overwritten, and the rest of the picture stays byte-for-byte
untouched.

## Who it is for

Someone with an idea and no patience for a mask workflow. They will type a sentence, circle a region and
press a button; they will not learn what a control image, a denoise strength or a model id is. They also
expect a real editor: files, layers, crop, undo.

## What it is not

- Not a node graph, not a compositing suite, not a prompt-engineering IDE.
- Not a chat. There is no conversation: a request is a command, it runs, it lands, it can be undone.
- Not model-facing. The user chooses *what* they want; roles pick the model, and the app only says which
  model will answer when that means a swap on a shared server.
- Not a browser of images. It opens files and saves files; it does not manage a library.

## The scenario this product has to serve

The beats below are the specification. Each one has to be doable without a keyboard shortcut being
memorised, and each is a step in `tools/flow_probe.py`.

1. **Open the program with an idea.** A start dialog: New or Open, with recent files. New offers the kinds
   of image it can make, a box for the prompt, a size - preset ratios and an explicit pixel size - and an
   *Enhance prompt* affordance (parked until the `enhance` role is switched on, but the UI has room for it
   from the first day). The dialog quotes what will be asked, the way the status line does: "3840×2160,
   will ask 3840×2160, about 30 s".
2. **Wait, visibly.** Progress, even coarse or indefinite: elapsed, what is happening, and Cancel.
3. **The image arrives** as the document's first layer. The document now exists at the chosen size.
4. **"Fix this."** The hand of a subject is broken. Circle it with any tool, press *Fix*. No prompt needed.
5. **"Put something here."** Select an area, press *Insert*, type "a streetlight with the red lamp on".
6. **"Put this thing here."** Drag an image in (or choose it from a menu): it lands as-is, on its own
   layer. It looks pasted. Press *Blend* with no prompt and it is repainted into the scene - still on its
   own layer.
7. **"Change this one thing."** Select the car, press *Edit*, type "make the car yellow".
8. **"Take that away."** Select the tree, press *Erase*.
9. **"This pose, that person."** Drag in a pose image, move and scale it over the subject, press *Blend*
   and type "apply the pose to the subject". (Openpose/control images come later; the door is the same.)
10. **"The framing is tight."** *Canvas size*: add 300 px on the left, choose *AI fill* - outpaint.
11. **"I am lost in layers."** *Flatten*, and the picture is one layer.
12. **"Wallpaper proportions."** Crop, no AI.
13. **"I want 4K."** *Image size*, new size, method: Lanczos / *AI upscale (fast)* / *AI upscale (best)*.
14. **Save** the document, then **Export** a flat WebP.

Working on an existing file is the same story with fewer steps: open, do, save.

## Concepts, in the words the UI will use

- **Document** - the picture and its layers, at one size. Saved as `.noise`.
- **Layer** - a rectangle of pixels with opacity, visibility and an optional mask. Answers always land on
  one. The Layers panel is **always visible**: it is where a person goes to understand what an edit did.
- **AI reference** - an independent image explicitly added from a local PNG/JPEG/WebP file or one document
  layer. The References section numbers these after Image 1, lets the person reorder and remove them, and
  keeps them for this app session only. Adding a file here does not insert a document layer; inserting a
  layer does not make it a reference. References give the model information and do not expand the marked
  area or change where an answer lands. Change and Fill include them by default; Blend, Heal and Remove
  leave them configured but excluded until the user turns them on for that operation.
- **Coverage** - one concept, two lifetimes. The *marked area* is transient tool state, painted, marquee'd
  or lassoed, drawn as marching ants; a layer's *mask* is the same paintable thing, stored in the document.
  One editor, one shape language, one maths: the target switch says which one the brush is working on.
- **Verb** - a named request with a question: *Fix*, *Insert*, *Edit*, *Erase*, *Blend*, *Generate*,
  *Upscale*. A verb is a row in `assets/recipes/`, not a branch in code. It has a scope - the marked area,
  the whole document, or a layer - and its answer lands on a new layer.
- **Quote** - what a press will do, shown before it is pressed: size, model, rough time. Computed without
  touching pixels.
- **Report** - what a press did: what moved, where, and the artefacts. Readable without opening a log.

## Rules that do not bend

1. **An answer never overwrites pixels that exist.** It arrives on a layer of its own, above the content it
   was compiled from. The one exception is a canvas that holds nothing: `New` made an empty layer, nobody
   has drawn on it, and the answer *is* the document — so it fills that layer rather than stacking a
   picture above an invisible one. Undo gives the blank canvas back.
2. **Pixel-lock.** Outside the coverage - and the feather the recipe asked for - the document is byte
   identical to what it was. Whole-image diff, not a sampled point.
3. **Document and job state follow different lifetimes.** The `.noise` document persists when saved;
   jobs, prompts, selections, AI references and undo history die with the run, and re-asking is the retry. App
   preferences such as the UI theme and recent-file list live in config and do not alter a document.
4. **Every request is priced before it is sent**, and its fields are chosen by one policy the UI cannot
   widen.
5. **The document owns its size.** Crop, canvas growth and resampling are undoable document commands, and
   the view follows - the window is the window.
6. **Everything observable is on the debug wire**, including whatever the UI can do; if the UI can do it,
   a driver can do it, and if a driver cannot, that is a bug in the command table, not a reason for a side
   door.

## Decisions taken 2026-10-15

- The verbs live **next to the marked area** - a floating bar at the selection - because that is where the
  eyes already are. The panel keeps the full controls for the same verbs.
- **Layers are always shown**, with thumbnails, and the mask thumbnail when there is one.
- **File dialogs are the desktop's own** (portal/zenity subprocess, one file in `src/platform/`), with the
  in-app browser as the fallback where no portal exists.
- **Save writes `.noise`, flat images are Export** (png / jpg / webp).
- **Fix, Insert and Erase are three buttons**, not one button with a mode: three things a person wants are
  three things they can see.
- **Seed is not a user-facing concept.** *Retry* re-asks the same request with a different seed and lands
  another layer, which is what "try again" means. Seeds stay on the debug wire.
- **Roles and recipes are ours to pin.** The sets are the shipped `assets/roles.json` and
  `assets/recipes/*.json`, read from disk at startup. A person choosing their own models and prompts comes
  much later, and it will mean *those files, found and edited* - so nothing may bake a role, a recipe, a
  prompt tail or a default size into code, and every table read has to survive being replaced at run time.
  That is the whole of what "foreseen" means here; no settings UI is built for it now.

## Open

- Where the size lives in the New dialog versus the *Image size* dialog - same fields, different promise.
- Naming that a person would confirm: *Blend*, *Fix*, *Insert*, *Erase* are ours until they say otherwise.
- How much of the advanced world (roles, models, seeds, the wire) is reachable from the menu before it
  stops being an easy editor.
