# Native file dialogs

The native application uses [Native File Dialog Extended v1.4.1](https://github.com/btzy/nativefiledialog-extended),
licensed Zlib. CMake FetchContent pins the v1.4.1 release tag. The OS creates the picker as a separate,
movable window parented to the editor. The browser/WASM build keeps the JavaScript file bridge.
Linux native builds use the GTK 3 backend and need `libgtk-3-dev`; the current Sokol host supplies an
X11 parent window. The rest of the native application is still Linux-first, so Windows and macOS
builds remain future platform work.

- **Open**: `.noise` projects or PNG/JPEG/WebP images.
- **Save As**: `.noise` project, with overwrite confirmation.
- **Export**: one of the codec-supported image extensions, with overwrite confirmation.
- **Insert**: select an image to add to the document as a layer.
- **Add Reference**: multi-select image files without adding layers. Selected paths are sorted by
  UTF-8 path before `file.answer`, giving the References panel a deterministic starting order that
  can be changed there.
- **Navigation**: the first picker opens in the user's home directory. After a successful selection,
  later requests without an explicit path start in that file's directory. A provided path selects its
  parent directory; save requests also prefill its filename.
- **Save behavior**: the system dialog confirms overwrites. The selected filter supplies an extension
  when the filename has none on platforms that support that behavior.

File selection requests can originate from the UI or debug wire. The host consumes them on its main
thread; the native dialog runs synchronously there and returns a `PickResult`. While the OS modal is
open, the host frame loop and debug-wire polling pause. After it closes, the existing `file.answer`
command performs file operations and `file.pick.state` reports the result.

Native smoke verification after build:
1. `test_file_dialog_ui` opens the OS dialog, checks that it is a separate window, moves it outside
   the editor bounds when the display permits, cancels it, then selects a PNG and checks the inserted layer.
2. Open a PNG and a `.noise` file.
3. Save As a new `.noise`, then repeat with an existing filename and Cancel overwrite.
4. Export as PNG/JPEG/WebP, including a filename without an extension.
5. Insert an image, then select several images via Add Reference and reorder them.
6. Confirm keyboard shortcuts after a dialog closes.

The native picker follows the operating system's appearance, not the editor's Light/Dark theme.
