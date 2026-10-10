// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"
#include "core/layer.h"
#include "core/mask.h"
#include "io/zip.h"

#include <optional>
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

// Format 4 added a layer's mask: the same coverage plane as the selection, in a member the layer's
// row names and omits entirely when the layer has no mask - "no mask" and "a mask that hides
// everything" are different states, exactly as they are for the selection. Format 3 added the
// selection plane and its manifest row. Format 2 added `visible`, `opacity` and `mode` to every
// layer row and made them required, so
// a file cannot half-state how a layer looks. Format 3 added the selection: a coverage plane, and
// a manifest `selection` row that is *absent* when the document has no selection - which is a
// different state from a plane that covers nothing, and cannot be inferred from a missing field of
// an older file. The version is bumped for optional fields too, and a file of any other version is
// refused by number: there is one writer, it is us, and silently guessing a default for a field the
// format stopped writing is how a document opens wrong.
inline constexpr int kFormatVersion = 4;

// The selection's coverage plane: one byte per canvas pixel, raw, deflate-compressed by the
// container. Its size is the manifest `size`, restated nowhere.
inline constexpr const char* kSelectionName = "selection.m8";

// A layer mask is the same plane, named by that layer's row rather than by the manifest, so a
// reader that does not know masks still finds the layer's own PNG and simply shows more than the
// file meant.
inline constexpr const char* kMaskExtension = ".m8";

struct Project {
    int format = kFormatVersion;
    // Bottom-first. `id` is left alone: identity belongs to the open document, and a file
    // cannot name a layer that document has.
    std::vector<Layer> layers;
    // The document's selection, at the canvas size. Absent means no selection, not an empty one.
    std::optional<Mask> selection;

    // Members this build does not define - provenance, thumbnails, a journal from a newer
    // build. Read as they are, written back unchanged: opening a file must not delete what
    // the app cannot yet show.
    std::vector<ZipEntry> carried;
};

bool load_project(const std::string& path, Project& out, std::string& error);

// `layers` bottom-first and `merged` the flattened stack, both from the document being saved;
// `selection` is the document's, or nullopt for none; `carried` is whatever the file being replaced
// held that this build does not define.
bool save_project(const std::string& path, std::span<const Layer> layers, const Image& merged,
                  const std::optional<Mask>& selection, std::span<const ZipEntry> carried,
                  std::string& error);

}  // namespace noise::io
