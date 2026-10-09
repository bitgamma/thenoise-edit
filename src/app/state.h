// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/brush.h"
#include "core/document.h"
#include "core/geom.h"
#include "core/image.h"

#include <cstdint>
#include <string>

namespace noise {

// The one home for view-navigation constants: nothing else may restate them.
struct ViewPolicy {
    double min_fit_fraction;
    double max_scale;
    double wheel_step;
    int fit_margin_px;
    int overpan_px;
};
inline constexpr ViewPolicy kViewPolicy{0.05, 64.0, 1.10, 16, 48};

inline constexpr const char* kAppVersion = "0.0.0-m0";
inline constexpr const char* kWindowTitle = "noise-edit";

// The gesture the pointer is making, if any. The walk carries the dab spacing across events
// and the scratch holds the stroke's coverage, which is what lets a gesture painted frame by
// frame land on the same bytes as the same path painted in one go.
struct PointerStroke {
    bool active = false;
    Vec2 last{};
    std::size_t dabs = 0;
    DabWalk walk{0.0, false};
    StrokeScratch scratch;
};

struct AppState {
    Document doc;
    // The brush the next stroke uses, and the one the last stroke ran with: the status bar
    // and the wire read this, so a screenshot can be reproduced from the state alone.
    Brush brush = kBrushDefaults;
    Rgba8 color = kDefaultColor;
    std::string tool = kDefaultTool;
    PointerStroke stroke;
    ViewTransform view{{0, 0}, 1.0, {0.0, 0.0}};
    // Where the app writes: captures, the wire endpoint file, the imgui layout. Set once
    // by the host so nothing in the command layer reaches for an ambient directory.
    std::string runtime_dir;
    // The canvas content area, published by the host every frame it draws.
    Extent viewport{1, 1};
    // False until the host has published `viewport` once: before that the only extent on
    // hand is the framebuffer, and a fit measured against it latches the wrong scale.
    bool viewport_published = false;
    Extent framebuffer{1, 1};
    // Top-left of the canvas content area inside the framebuffer. View coordinates are
    // relative to it, so a capture needs it to place an image rect on screen.
    Vec2 canvas_origin{};
    std::uint16_t wire_port = 0;
    // Bumped once per presented frame: a wire client waits on it so captures and
    // assertions land on a frame the app actually drew.
    std::uint64_t frame = 0;
    // Wall time of the last presented frame, as the compositor reported it.
    double frame_ms = 0.0;
    // A fit asked for before there was a canvas to measure against waits here; the host
    // applies it on the first frame whose viewport can hold the document.
    bool fit_pending = false;
    // Vertices the last frame's UI demanded, against the ceiling it was staged into.
    // Past the ceiling ImGui drops draw commands, so this is what to read when the screen
    // is missing pieces rather than broken.
    std::uint64_t ui_vertices = 0;
    std::uint64_t ui_vertex_limit = 0;
};

}
