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
