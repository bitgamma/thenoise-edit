// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

namespace noise::platform {

// The unwritten seam, deliberately. macOS answers with `NSOpenPanel` from an Objective-C++ translation
// unit and Windows with `GetOpenFileNameW` from comdlg32; neither has been written because neither has a
// machine to test it on. Returning `cancelled` here would be a lie of the worst kind — the app would
// close its dialog as though the person had pressed Cancel — so this says what is missing instead.
PickResult pick_path(const PickRequest& request) {
    PickResult result;
    result.error = "no file picker is implemented for this target yet (asked: '" + request.title +
                   "')";
    return result;
}

}  // namespace noise::platform
