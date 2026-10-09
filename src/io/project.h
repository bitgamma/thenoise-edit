// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"
#include "io/zip.h"

#include <span>
#include <string>
#include <vector>

namespace noise::io {

// `.noise` is a ZIP: `mimetype` stored first so other applications sniff it, `manifest.json`
// as the app's own description of the document, one PNG per layer, and a flattened
// `mergedimage.png` for anything that cannot read the stack. v0 holds one layer and refuses a
// manifest that claims more, so a file is never opened with content quietly missing.
inline constexpr const char* kMimetype = "application/x-noise+zip";
inline constexpr const char* kProjectExtension = "noise";
inline constexpr int kFormatVersion = 1;

struct ProjectLayer {
    std::string name;
    Image pixels;
};

struct Project {
    int format = kFormatVersion;
    std::vector<ProjectLayer> layers;

    // Members this build does not define - provenance, thumbnails, a journal from a newer
    // build. Read as they are, written back unchanged: opening a file must not delete what
    // the app cannot yet show.
    std::vector<ZipEntry> carried;
};

bool load_project(const std::string& path, Project& out, std::string& error);

// v0 writes what `Document` holds: one layer by name, plus the members this build carried
// through from the file it opened. The manifest still describes a layer array, so these files
// stay readable when the app can hold more than one.
bool save_project(const std::string& path, const std::string& layer_name, const Image& pixels,
                  std::span<const ZipEntry> carried, std::string& error);

}  // namespace noise::io
