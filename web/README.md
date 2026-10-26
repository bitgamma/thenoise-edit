# WebAssembly editor build

The browser target runs the same Dear ImGui/Sokol editor, with WebGL2 instead of desktop
OpenGL. It is a separate build: it does not replace the Linux executable.

## Build

Activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(`emcc`, `emcmake`, and `em++` must be on `PATH`), then run from the repository root:

```sh
emcmake cmake -S . -B build-web -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNOISE_BUILD_TESTS=OFF
cmake --build build-web --parallel 4
python3 -m http.server 8000 --directory build-web
```

Open `http://localhost:8000/noise-edit.html` in a WebGL2-capable browser.
Do not use `file://`: Emscripten needs to fetch its `.wasm` and `.data` files.
The build emits `noise-edit.html`, `noise-edit.js`, `noise-edit.wasm`, and
`noise-edit.data`; deploy all four together. The `.data` contains the recipes,
role table and recipes. The editor uses the default ImGui font. The first configure fetches the pinned Sokol,
ImGui and JSON source trees, and the first Emscripten build may fetch codec ports.

A native build remains unchanged:

```sh
cmake -S . -B build-local -G Ninja
cmake --build build-local --parallel 4
ctest --test-dir build-local --output-on-failure
```

## Browser capabilities

- Create a document with **New**. Open PNG, JPEG, WebP and `.noise` files with the **browser Open button** or drag
  a file onto the canvas. When a document is open, a dropped image is inserted as
  a layer. The browser toolbar is intentional: browsers may reject a native file
  picker triggered from a delayed ImGui render callback instead of a real click.
- The **Save project** button downloads a `.noise` file. **Export image** downloads
  PNG by default (type `.jpg`, `.jpeg`, or `.webp` in the save prompt to change
  format). The native file commands are reused for encoding and document changes.
- Theme preference (Light/Dark) is saved to browser localStorage. Image and
  project data is held in ephemeral Emscripten MEMFS and must be **downloaded** to
  survive a reload or a closed tab; browser storage does not silently back up work.
- File picker / upload / download are browser APIs. Native X11 pickers, POSIX
  sockets, and the TCP debug wire are not present in the web target.
- **AI generation is not available in this first browser build.** Native
  libcurl plus the blocking background job thread cannot run in a normal
  non-pthread browser context. Proper AI support requires an asynchronous
  browser Fetch transport/queue and a server configured for CORS (and suitable
  authentication). The native editor's AI features are unchanged.
- WebGL2 is required. Threads, SharedArrayBuffer, COOP/COEP and a proxy server
  are **not** required for local editing.

## Tests

```sh
node web/bridge.test.cjs     # hermetic browser-file bridge tests; no WASM required
```

After building, check the browser console for errors and exercise: New, Open,
layer import, brush, undo/redo, Save project, Export image, reload/re-open,
and theme persistence in both themes. The native CTest suite remains the
primary CPU/IO regression suite. Web builds deliberately do not register
native executable/Xvfb tests.
