# Working on the GUI in a devcontainer

Open the repository in VS Code and select **Dev Containers: Reopen in Container**.
Run your coding agent in the container, the image supplies the project tools,
not agent credentials or a particular agent version.
The container uses Ubuntu 24.04 and installs the exact Linux build dependencies from
`.github/workflows/ci.yml`, plus XTest, Pillow, NumPy and a file picker so the
GUI and its interaction probes work. CMake fetches its pinned Sokol, ImGui and JSON
sources during the initial configure; an internet connection is needed then.
The isolated `build-devcontainer/` avoids reusing a build cache created on the host.
A `compile_commands.json` symlink is created only if one does not already exist.

```sh
cmake --build build-devcontainer --parallel 4
ctest --test-dir build-devcontainer --output-on-failure
python3 tools/gui_snapshot.py --bin build-devcontainer/noise-edit --output out/gui.png
python3 tools/gui_snapshot.py --bin build-devcontainer/noise-edit --file path/to/image.png --output out/document.png
```

`gui_snapshot.py` launches the actual application, waits for it to draw, captures
the **whole window** through the debug wire and shuts down both the app and its
private Xvfb display. Inspect the resulting PNG when changing UI layout, alongside
the relevant `test_*_probe` runs. Use `--no-start-dialog` to inspect an empty
workspace instead of the welcome screen. The `out/` directory is gitignored.

The tests and screenshots use Mesa's software OpenGL (`llvmpipe`) and Xvfb: **no
GPU, host X11 socket or live image server is required**. The default `ctest` suite
uses recorded AI fixtures, not a real server. Live requests remain opt-in via
`NOISE_LIVE_TESTS` and `THENOISE_URL` / `THENOISE_API_KEY` as described in
[`docs/TESTING.md`](../docs/TESTING.md). The wire is loopback-only; don't expose it
through forwarded ports. Xvfb verifies software-GL layout and interaction, but it
cannot replace a final check on a real desktop/GPU.

For interactive GUI input tests, use `tools/wire_input.py` and the panel probes.
For the command protocol, use `command.list` through `tools/wire_call.py` on a
running instance, as required by `AGENTS.md`. After a timed-out probe, check for
orphaned processes with `python3 tools/sweep_procs.py --list`.

## WebAssembly and headless browser checks

This **same container** also includes Emscripten 6.0.10, Node.js 24 and
Playwright Chromium (including the headless shell). Emscripten ports are cached
under `~/.cache/emscripten`; Playwright lives under
`~/.local/share/thenoise-web-tools`, and Chromium under `~/.cache/ms-playwright`.
No packages are installed into the repository or `REF/node_modules`. The
container installs Playwright **before** native CMake configuration and checks
for its browser on every start. Existing downloads are reused. The Playwright
CLI and Node module are on the VS Code terminal environment (`PATH` / `NODE_PATH`).
The first container build needs internet for the SDK, npm package and browser.

```sh
emcc --version
node --version
playwright --version                 # available directly in the VS Code terminal
node -e "console.log(require('playwright').chromium.executablePath())"
node web/bridge.test.cjs
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release -DNOISE_BUILD_TESTS=OFF
cmake --build build-web --parallel 4
node web/smoke.cjs             # launches the server, checks both themes, writes screenshots
python3 -m http.server 8000 --bind 0.0.0.0 --directory build-web  # optional manual preview
```

If a browser cache was deleted, rerun `bash .devcontainer/playwright.sh`; the
command is also run automatically when the container starts.

Open the forwarded port 8000 to preview `noise-edit.html` interactively.
The smoke test writes `out/web-smoke/dark.png` and `light.png`, requires WebGL2,
and fails on runtime JS errors or HTTP failures. The headless browser uses
SwiftShader; test on real hardware too before shipping performance-sensitive UI.
It does **not** replace manual checks of editing workflows or the native CTest
suite. Keep `build-web/` separate from `build-devcontainer/`; never reuse their
CMake caches.

The browser deliberately has no TCP debug wire. Test it via the DOM, canvas,
Playwright screenshots and browser console instead. The container does not
implement browser AI networking, offline persistence or WebGL1 fallback.
