// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/ai_intent.h"
#include "app/ai_session.h"
#include "app/ants.h"
#include "app/ai_verbs.h"
#include "app/coverage.h"
#include "app/new_document.h"
#include "app/pointer.h"

#include "core/brush.h"
#include "core/document.h"
#include "io/zip.h"
#include "core/geom.h"
#include "core/image.h"
#include "io/config.h"
#include "platform/file_dialog.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
inline constexpr const char* kWindowTitle = "TheNoise Editor";

// A rectangle in framebuffer pixels. Not `Rect`: that one counts document pixels, and these two are
// compared against different things all day.
struct ScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

// One layer's card in the panel: the bordered box that owns its controls, and every control's box, so a
// driver presses what the app drew instead of remembering a layout. The rects are reported whether or not
// the control can be pressed, and no two of them may overlap - the item ImGui submitted first for a point
// is the one that gets it, so an overlapping control is a dead one.
struct LayerCard {
    int index = 0;
    std::string name;
    bool active = false;
    bool visible = true;
    ScreenRect box{};
    ScreenRect visibility{};
    ScreenRect title{};
    // The layer's own pictures, where the card drew them. Empty when there was nothing to draw, which
    // is what makes "the thumbnail is the layer" a claim a driver can check rather than assume.
    ScreenRect picture{};
    ScreenRect mask_picture{};
    ScreenRect move_up{};
    ScreenRect move_down{};
    ScreenRect merge{};
    ScreenRect opacity{};
    ScreenRect mode{};
    bool can_move_up = false;
    bool can_move_down = false;
    bool can_merge = false;
};

// The row of controls under the cards: the ones that answer to the stack rather than to a layer. Their
// boxes are reported so a driver presses the button instead of the command it stands in for - the claim
// being that the panel works, not that the command table does.
struct LayerPanelControls {
    ScreenRect add{};
    ScreenRect remove{};
    ScreenRect flatten{};
    // The mask section's two doorways, published because clicking them is the only way to test the
    // frame they live in: `Remove` changes the state the same frame's own text is drawn from.
    ScreenRect mask_add{};
    ScreenRect mask_remove{};
    bool can_flatten = false;
    bool has_mask = false;
};

// The gesture the pointer is making, if any. The walk carries the dab spacing across events
// and the scratch holds the stroke's coverage, which is what lets a gesture painted frame by
// frame land on the same bytes as the same path painted in one go.
struct PointerStroke {
    bool active = false;
    Vec2 last{};
    std::size_t dabs = 0;
    DabWalk walk{0.0, false};
    StrokeScratch scratch;
    // Which of the layer's two planes this press writes, taken when the button went down. Held here
    // rather than read per event because the held-aside bytes are one plane's: a target switched
    // mid-drag would otherwise leave a gesture writing coverage into a snapshot of colour.
    LayerPlane plane = LayerPlane::pixels;
    // And, for a coverage press, which lifetime it was aimed at, for the same reason.
    app::CoverageTarget coverage = app::CoverageTarget::marked;
};

// The gesture the pointer is making with the move tool. The snapshot is the active layer as it was
// when the button went down, and every event redraws from it rather than from what the last event
// left behind: a gesture fed in point by point then lands on the same bytes as the same offset
// applied once, which is what makes it testable the way a stroke is. `placed` is where the content
// sat after the previous event, which is the half of the next dirty rectangle that has to be
// vacated as well as the half that has to be filled.
struct PointerMove {
    bool active = false;
    Vec2 origin{};
    int dx = 0;
    int dy = 0;
    std::size_t steps = 0;
    Image snapshot;
    Rect content{};
};

// The gesture the pointer is making with the select tool. The rectangle is the marquee in document
// pixels and it is live: the document holds it during the drag, so a state report taken mid-gesture
// says what is selected rather than what was selected before the button went down.
struct PointerSelection {
    bool active = false;
    Vec2 origin{};
    Rect rect{};
    // Held from the moment the button goes down rather than read again on every drag: switching
    // tools mid-marquee must not change the shape of the one already being drawn.
    MaskShape shape = MaskShape::rect;
    // The same argument for the lifetime: the press opened one door - the layer's provisional bytes or
    // the document's marked coverage - and releasing has to shut that one, however the switch was
    // aimed by then.
    app::CoverageTarget coverage = app::CoverageTarget::marked;
};

struct AppState {
    Document doc;
    io::Theme ui_theme = io::Theme::dark;
    // The brush the next stroke uses, and the one the last stroke ran with: the status bar
    // and the wire read this, so a screenshot can be reproduced from the state alone.
    Brush brush = kBrushDefaults;
    Rgba8 color = kDefaultColor;
    std::string tool = kDefaultTool;
    // Which coverage a coverage-writing tool is aimed at. No reset policy of its own: the layer may
    // have no mask, and then a stroke aimed at one is refused naming the layer, rather than quietly
    // painting the marked area instead.
    app::CoverageTarget coverage_target = app::CoverageTarget::marked;
    // How marked coverage is drawn: boundary, wash, or both.
    app::CoverageView coverage_view = app::CoverageView::both;
    // What the pointer overlay drew last frame, and the probe's force switch for the brush ring.
    app::PointerView pointer{};
    std::optional<bool> footprint_forced{};
    // The verb whose words the bar's popup is holding, if one is open. A verb that asks for nothing is
    // never in here: its button runs, and a popup for it would be an empty box.
    std::optional<app::VerbId> ask_verb;
    // An ask to leave that unsaved work is holding. It is state rather than a dialog the window
    // remembers, so the wire sees the question and answers it the same way the button does.
    bool quit_pending = false;
    PointerStroke stroke;
    PointerMove move;
    PointerSelection select;
    ViewTransform view{{0, 0}, 1.0, {0.0, 0.0}};
    // Members of the .noise file that was opened which this build does not define, written
    // back unchanged on save so opening a newer file cannot delete what we cannot show.
    std::vector<io::ZipEntry> carried;
    // Where the app writes: captures, the wire endpoint file, the imgui layout. Set once
    // by the host so nothing in the command layer reaches for an ambient directory.
    std::string runtime_dir;
    // The AI path's tables, server and queue. `open_ai_session` fills it once the host has set
    // `runtime_dir`; every AI command reads it and nothing else constructs one.
    app::AiSession ai;
    // What the "Edit selection" panel is asking for. State rather than widget values: the panel and
    // `ai.intent` read one thing, so a screenshot, a wire reply and a click cannot disagree.
    app::AiIntent ai_intent;
    // The canvas content area, published by the host every frame it draws.
    Extent viewport{1, 1};
    // False until the host has published `viewport` once: before that the only extent on
    // hand is the framebuffer, and a fit measured against it latches the wrong scale.
    bool viewport_published = false;
    Extent framebuffer{1, 1};
    // Top-left of the canvas content area inside the framebuffer. View coordinates are
    // relative to it, so a capture needs it to place an image rect on screen.
    Vec2 canvas_origin{};
    // Where the verb bar was drawn this frame, in framebuffer pixels, width zero when there was no
    // mark to anchor it to. Published for the wire: a driver clicks the bar by reading this back
    // rather than by measuring a screenshot.
    ScreenRect verb_bar{};
    // Where the layers panel drew this frame: the panel's own box and one card per layer, top card
    // first, each with the box its visibility control occupies. A driver clicks what the app reports
    // rather than a coordinate it remembered from a screenshot.
    ScreenRect layers_window{};
    // The panel's picture cache, read out of it each frame it draws: how many thumbnails it has had to
    // resample since the run started.
    std::uint64_t layer_thumb_rebuilds = 0;
    std::vector<LayerCard> layer_cards{};
    LayerPanelControls layer_panel{};
    // And the same for the phrase box the bar opens, so a driver can read the box's own rect rather
    // than deriving it from the bar's.
    ScreenRect verb_ask{};
    std::uint16_t wire_port = 0;
    // Bumped once per presented frame: a wire client waits on it so captures and
    // assertions land on a frame the app actually drew.
    std::uint64_t frame = 0;
    // Published by the host each frame, in the units the queue stamps jobs with. A widget that says
    // "6 s" reads this rather than calling a clock, so a test can pin the sentence.
    double now = 0.0;
    // How many times an upload has been prepped and PNG-encoded this run. Reading the AI action must not
    // move it; `ai.preview` and a run must. It is a number rather than a log line because the claim
    // "choosing not to run costs nothing" has to be assertable.
    std::uint64_t upload_encodes = 0;
    // Bumped when a *different* document becomes the open one (`doc.new`, `doc.open`). A job carries the
    // epoch it was queued under and its answer lands only in that document: "the answer lands by
    // itself" cannot mean "into whatever file is open when the model finally answers", and it must not
    // mean "twice".
    std::uint64_t document_epoch = 0;
    // Wall time of the last presented frame, as the compositor reported it.
    double frame_ms = 0.0;
    // A fit asked for before there was a canvas to measure against waits here; the host
    // applies it on the first frame whose viewport can hold the document.
    bool fit_pending = false;
    // Vertices the last frame's UI demanded, against the ceiling it was staged into.
    // Past the ceiling ImGui drops draw commands, so this is what to read when the screen
    // is missing pieces rather than broken.
    app::AntOverlay ants;
    std::uint64_t ui_vertices = 0;
    std::uint64_t ui_vertex_limit = 0;

    // The dialog between a launch and a document: New, and at launch also Open with the recent files.
    // State rather than an ImGui popup flag, so `app.state` says whether the app is waiting for a
    // person and a driver can put the door there or take it away.
    app::FrontDoor front_door = app::FrontDoor::closed;
    app::NewDocumentForm new_document;

    // A native file picker blocks until the person answers, so it runs on a worker and the answer lands
    // here for the panel, the wire and the frame loop to read. One at a time: a second dialog would be a
    // modal nobody can see, and an answer has to belong to the request still on screen.
    std::mutex pick_mutex;
    bool pick_running = false;
    std::string pick_title;
    std::optional<platform::PickResult> pick_answer;
    // Which file set an open pick belongs to, whether its answer still needs routing, and what to say
    // about it where the buttons are. The pick's own record rather than the widget's, so a pick asked
    // for over the wire is not silently dropped.
    std::string pick_set;
    bool pick_needs_route = false;
    std::string pick_note;
    // Joined when the state goes away, so nothing can free it under a worker still writing the answer.
    std::thread pick_worker;

    ~AppState() {
        if (pick_worker.joinable()) {
            pick_worker.join();
        }
    }

    // True while the pointer is inside a gesture of any tool. The host ends the gesture on the
    // button release through this, so a new tool has one place to be counted in and cannot leave
    // the button held by being forgotten in a condition elsewhere.
    bool gesturing() const { return stroke.active || move.active || select.active; }

    // Whether the active layer holds the plane a stroke is aimed at. The panel hides the affordance,
    // the commands refuse with this question, and the tool never starts a gesture it cannot hold - so
    // "there is no mask here" is answered in one place rather than guessed at three.
    bool coverage_writable() const {
        if (doc.empty()) {
            return false;
        }
        return coverage_target == app::CoverageTarget::marked ||
               active_layer().mask.has_value();
    }

    // The layer a write, a mask or a refusal is about.
    const Layer& active_layer() const {
        return doc.layer(static_cast<std::size_t>(doc.active()));
    }
};

}
