# noise-edit

A layered raster editor whose AI operations are ordinary editing commands with a compile step in the
middle: you mark an area and say what you want, the app compiles that into one request, and the answer
lands on its own layer with everything outside the marked area byte-identical.

## Run it

```sh
cmake -B build && cmake --build build && ./build/noise-edit [file.noise | image.png]
ctest --test-dir build                       # hermetic, opens no sockets
```

`$THENOISE_URL` and `$THENOISE_API_KEY` point it at a Lemonade server; `$NOISE_AI_FIXTURES` points it at
recorded responses instead. Without either it runs as an editor with no AI verbs available and says so.
`--width` / `--height` / `--runtime` / `--debug-wire` exist for drivers, not for people.

### Windows (Visual Studio 2022, x64)

Install Visual Studio 2022 with the **Desktop development with C++** workload, CMake, Git,
then use PowerShell from the source directory:

```powershell
git clone --depth 1 --branch 2026.10.16 https://github.com/microsoft/vcpkg.git "$env:USERPROFILE/vcpkg"
& "$env:USERPROFILE/vcpkg/bootstrap-vcpkg.bat" -disableMetrics
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:USERPROFILE/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-windows --config Release --parallel
ctest --test-dir build-windows -C Release --output-on-failure
cmake --install build-windows --config Release --prefix dist-windows
.\dist-windows\bin\noise-edit.exe
```

The first configure installs image codecs and cURL through vcpkg. The install tree contains the
exe, editing recipes and native file-dialog license; it can be zipped as a portable build.
The `x64-windows-static` triplet avoids extra codec/cURL DLLs; Windows must provide an OpenGL 3.3
capable graphics driver. Native file dialogs use Windows' own file picker. The Windows CI job
runs the headless native C++ tests and uploads the install tree as an artifact. X11 input and
GUI screenshot probes remain Linux-only; Windows interactive GUI behavior still needs manual
verification.

For container-based GUI development with Agents or VS Code, see
[the devcontainer guide](../.devcontainer/README.md).

## The four documents

| File | What it answers |
|---|---|
| **[PRODUCT.md](PRODUCT.md)** | what this is, who it is for, the scenario it has to serve, the rules that do not bend |
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | how it is built: modules, compositing, painting, history, `.noise`, the AI path, the debug wire |
| **[TESTING.md](TESTING.md)** | the tiers, what a live run costs, the probe harness, the house rule about processes |
| **[PLAN.md](PLAN.md)** | what is next and why, the traps the code cannot show, and a dated log of what was measured |

[`../AGENTS.md`](../AGENTS.md) is the working contract for whoever edits this tree — normative, and it
outranks the docs above. `docs/probes/` holds dated measurements; nothing there is a claim about how the
code works today.

Read in that order if you are new. `command.list` over the debug wire is the only reference for the command
surface and the keyboard, so no document duplicates it.
