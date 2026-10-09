// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"
#include "core/layer.h"
#include "io/zip.h"

#include <span>
#include <string>
#include <vector>

namespace noise::io {

// `.noise` is a ZIP: `mimetype` stored first so other applications sniff it, `manifest.json`
// as the app's own description of the document, one PNG per layer bottom-first, and a flattened
// `mergedimage.png` for anything that cannot read the stack. Every layer covers the whole canvas,
// which is what the manifest's `size` says, so a file whose layers disagree is refused rather
// than opened with a layer silently cropped or padded.
inline constexpr const char* kMimetype = "application/x-noise+zip";
inline constexpr const char* kProjectExtension = "noise";

// Format 2 added `visible`, `opacity` and `mode` to every layer row and made them required, so
// a file cannot half-state how a layer looks. The version is bumped for optional fields too, and
// a file of any other version is refused by number: there is one writer, it is us, and silently
// guessing a default for a field the format stopped writing is how a document opens wrong.
inline constexpr int kFormatVersion = 2;

struct Project {
    int format = kFormatVersion;
    // Bottom-first. `id` is left alone: identity belongs to the open document, and a file
    // cannot name a layer that document has.
    std::vector<Layer> layers;

    // Members this build does not define - provenance, thumbnails, a journal from a newer
    // build. Read as they are, written back unchanged: opening a file must not delete what
    // the app cannot yet show.
    std::vector<ZipEntry> carried;
};

bool load_project(const std::string& path, Project& out, std::string& error);

// `layers` bottom-first and `merged` the flattened stack, both from the document being saved;
// `carried` is whatever the file being replaced held that this build does not define.
bool save_project(const std::string& path, std::span<const Layer> layers, const Image& merged,
                  std::span<const ZipEntry> carried, std::string& error);

}  // namespace noise::io
