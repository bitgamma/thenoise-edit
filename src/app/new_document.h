// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace noise::app {

// What stands between the person and a document. `welcome` is what a launch with no file on the
// command line shows; `new_document` is the same body without the file half, from File > New.
enum class FrontDoor {
    closed,
    welcome,
    new_document,
};

// The sizes the New dialog offers. Sizes rather than ratios: at New there is no canvas for a ratio to
// describe, so the number typed *is* the document.
struct NewDocumentPreset {
    int width;
    int height;
    const char* label;
};

inline constexpr NewDocumentPreset kNewDocumentPresets[] = {
    {1024, 1024, "square"},
    {1024, 1365, "portrait"},
    {1365, 1024, "landscape"},
    {1920, 1080, "full HD"},
    {3840, 2160, "4K UHD"},
};
inline constexpr NewDocumentPreset kNewDocumentDefault{1024, 1024, "square"};

// The dialog's own ceiling. Past this an edge is a typo more often than an intention, and a document
// of that size costs 3.4 GB of RGBA before anything is drawn into it.
inline constexpr int kNewDocumentMaxEdge = 16384;

// The recipe family the dialog offers as *kind*: the head is what a prompt runs when nothing was
// chosen, its variants are the styles listed beside it. Adding a style to `assets/recipes/` adds a row
// to the dialog, which is why the family is named here and its members are not.
inline constexpr const char* kNewDocumentFamily = "new-image";

// The New dialog's fields, and what the `new_document` command reads and writes: state rather than
// widget values, so a click and a driver mean the same Create.
struct NewDocumentForm {
    int width = kNewDocumentDefault.width;
    int height = kNewDocumentDefault.height;
    std::string prompt;
    // Which recipe of `kNewDocumentFamily` the prompt will run.
    std::string kind = kNewDocumentFamily;
};

}  // namespace noise::app
