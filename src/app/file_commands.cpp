// SPDX-License-Identifier: Apache-2.0
#include "app/file_commands.h"

#include "app/command_args.h"
#include "app/command_groups.h"
#include "io/config.h"
#include "io/flat.h"
#include "io/project.h"

#include <nlohmann/json.hpp>

#include <thread>
#include <utility>

namespace noise::app {

namespace {

const char* front_door_name(FrontDoor door) {
    switch (door) {
        case FrontDoor::closed:
            return "closed";
        case FrontDoor::welcome:
            return "welcome";
        case FrontDoor::new_document:
            return "new";
    }
    return "closed";
}

json presets_json() {
    json out = json::array();
    for (const NewDocumentPreset& preset : kNewDocumentPresets) {
        out.push_back({{"width", preset.width},
                       {"height", preset.height},
                       {"label", preset.label}});
    }
    return out;
}

json new_document_json(const NewDocumentForm& form) {
    return {{"width", form.width},
            {"height", form.height},
            {"prompt", form.prompt},
            {"max_edge", kNewDocumentMaxEdge},
            {"presets", presets_json()}};
}

json filters_json(const std::vector<platform::FileFilter>& filters) {
    json out = json::array();
    for (const platform::FileFilter& filter : filters) {
        out.push_back({{"label", filter.label}, {"extensions", filter.extensions}});
    }
    return out;
}

}  // namespace

std::optional<FileSet> file_set_from_name(const std::string& name) {
    if (name == "open") {
        return FileSet::open;
    }
    if (name == "project") {
        return FileSet::project;
    }
    if (name == "flat") {
        return FileSet::flat;
    }
    if (name == "image") {
        return FileSet::image;
    }
    return std::nullopt;
}

std::string file_set_name(FileSet set) {
    switch (set) {
        case FileSet::open:
            return "open";
        case FileSet::project:
            return "project";
        case FileSet::flat:
            return "flat";
        case FileSet::image:
            return "image";
    }
    return "open";
}

std::string file_set_label(FileSet set) {
    switch (set) {
        case FileSet::open:
            return "a file";
        case FileSet::project:
            return "a document";
        case FileSet::flat:
            return "an image";
        case FileSet::image:
            return "a picture to insert";
    }
    return "a file";
}

platform::PickKind pick_kind(FileSet set) {
    // A switch rather than "is it the open set": "insert a picture" reads a file too, and a ternary on
    // one name handed the Insert button a save dialog.
    switch (set) {
        case FileSet::open:
        case FileSet::image:
            return platform::PickKind::open;
        case FileSet::project:
        case FileSet::flat:
            return platform::PickKind::save;
    }
    return platform::PickKind::open;
}

std::string file_set_names() {
    std::string out;
    for (const FileSet set :
         {FileSet::open, FileSet::project, FileSet::flat, FileSet::image}) {
        if (!out.empty()) {
            out += ", ";
        }
        out += file_set_name(set);
    }
    return out;
}

std::vector<platform::FileFilter> file_filters(FileSet set) {
    const std::vector<std::string> images = io::flat_export_extension_list();
    switch (set) {
        case FileSet::image:
            // The reader sniffs bytes, so what it can put into a layer is exactly what the codecs
            // write — no separate list to keep honest, and no `.noise` in an insert dialog.
            return {{"Images", images}, {"All files", {"*"}}};
        case FileSet::open:
            return {{"Images", images},
                    {"noise document", {io::kProjectExtension}},
                    {"All files", {"*"}}};
        case FileSet::project:
            return {{"noise document", {io::kProjectExtension}}};
        case FileSet::flat:
            break;
    }
    // One filter per format: a dialog that offers "Images" for an export cannot derive the extension
    // from what the person typed, so the format they picked is the format that gets written.
    std::vector<platform::FileFilter> filters;
    for (const std::string& extension : images) {
        filters.push_back({extension, {extension}});
    }
    filters.push_back({"All files", {"*"}});
    return filters;
}

json front_door_json(const AppState& state) {
    return {{"front_door", front_door_name(state.front_door)},
            {"new_document", new_document_json(state.new_document)}};
}

void register_file_commands(AppState& state, CommandTable& table) {
    // The picker blocks for as long as the person takes, and a frame loop that waits for it stops
    // answering the wire. So the answer arrives on a worker and `file.pick.state` is what anyone reads.
    table.add("file.pick", "Ask the desktop for a path; read the answer from file.pick.state",
              {"Ctrl+O"},
              {"for", "title", "path", "multiple"}, [&state](const json& args) -> CommandResult {
                  std::string set_name = "open";
                  if (!arg_name(args, "for", set_name)) {
                      return fail("file.pick",
                                  "expected string 'for' (" + file_set_names() + ")");
                  }
                  const std::optional<FileSet> set = file_set_from_name(set_name);
                  if (!set.has_value()) {
                      return fail("file.pick", "'for' names no file set: '" + set_name + "' (" +
                                                  file_set_names() + ")");
                  }
                  platform::PickRequest request{};
                  request.kind = pick_kind(*set);
                  request.filters = file_filters(*set);
                  request.title = "Choose " + file_set_label(*set);
                  if (!arg_text(args, "title", request.title)) {
                      return fail("file.pick", "expected string 'title'");
                  }
                  if (!arg_text(args, "path", request.initial_path)) {
                      return fail("file.pick", "expected string 'path'");
                  }
                  if (!arg_bool(args, "multiple", request.multiple)) {
                      return fail("file.pick", "expected boolean 'multiple'");
                  }
                  if (request.multiple && request.kind != platform::PickKind::open) {
                      return fail("file.pick",
                                  "'multiple' asks for several paths and saving writes one");
                  }
                  {
                      std::lock_guard lock(state.pick_mutex);
                      if (state.pick_running) {
                          return fail("file.pick", "a picker is already open ('" +
                                                      state.pick_title +
                                                      "'); answer it, then ask again");
                      }
                      state.pick_running = true;
                      state.pick_title = request.title;
                      state.pick_answer = std::nullopt;
                  }
                  if (state.pick_worker.joinable()) {
                      state.pick_worker.join();
                  }
                  state.pick_worker = std::thread([&state, request] {
                      const platform::PickResult answered = platform::pick_path(request);
                      std::lock_guard lock(state.pick_mutex);
                      state.pick_answer = answered;
                      state.pick_running = false;
                  });
                  return command_ok({{"started", true},
                                     {"for", file_set_name(*set)},
                                     {"title", request.title},
                                     {"filters", filters_json(request.filters)}});
              });

    table.add("file.recent", "The documents this app opened or saved last, newest first", {}, {},
              [](const json&) -> CommandResult {
        return command_ok({{"files", io::load_recent_files()}});
    });

    table.add("new_document", "Read or set the New dialog's fields", {},
              {"width", "height", "prompt"}, [&state](const json& args) -> CommandResult {
        int width = state.new_document.width;
        int height = state.new_document.height;
        std::string prompt = state.new_document.prompt;
        if (!arg_int(args, "width", width) || !arg_int(args, "height", height)) {
            return fail("new_document", "expected integers 'width' and 'height'");
        }
        if (!arg_text(args, "prompt", prompt)) {
            return fail("new_document", "expected string 'prompt'");
        }
        if (width <= 0 || height <= 0) {
            return fail("new_document", "width and height must be positive (got " +
                                            std::to_string(width) + "x" + std::to_string(height) + ")");
        }
        if (width > kNewDocumentMaxEdge || height > kNewDocumentMaxEdge) {
            return fail("new_document", "an edge over " + std::to_string(kNewDocumentMaxEdge) +
                                            " px is refused before the document is allocated");
        }
        state.new_document = NewDocumentForm{width, height, prompt};
        return command_ok(new_document_json(state.new_document));
    });

    // The door is one command with one argument rather than three commands with the same effect: what
    // is on screen is a state, and the launch path and a menu click should not be able to disagree
    // about which of the three they meant.
    table.add("front_door", "Show or hide the New/Open dialog ('welcome', 'new', 'close')", {},
              {"show"}, [&state](const json& args) -> CommandResult {
        std::string show;
        if (arg_name(args, "show", show)) {
            if (show == "welcome") {
                state.front_door = FrontDoor::welcome;
            } else if (show == "new") {
                state.front_door = FrontDoor::new_document;
            } else if (show == "close") {
                state.front_door = FrontDoor::closed;
            } else {
                return fail("front_door",
                            "'show' is 'welcome', 'new' or 'close', not '" + show + "'");
            }
        }
        return command_ok(front_door_json(state));
    });

    table.add("file.new", "Open the New dialog (it creates nothing until Create)", {"Ctrl+N"}, {},
              [&state](const json&) -> CommandResult {
        state.front_door = FrontDoor::new_document;
        return command_ok(front_door_json(state));
    });

    table.add("file.pick.state", "What the last file pick answered", {}, {},
              [&state](const json&) -> CommandResult {
                  std::lock_guard lock(state.pick_mutex);
                  json out{{"running", state.pick_running}, {"title", state.pick_title}};
                  if (state.pick_answer.has_value()) {
                      out["cancelled"] = state.pick_answer->cancelled;
                      out["paths"] = state.pick_answer->paths;
                      out["error"] = state.pick_answer->error;
                  }
                  return command_ok(out);
              });

    // One door for "the person chose a path", so a dialog, a driver and a file drop use the same word
    // and get the same thing done. The routing lives here rather than in the widget because the frame
    // that opens a picker and the frame that receives an answer are minutes apart — and because a
    // command a person can click has to be one a probe can call.
    table.add("file.answer", "Do what a picked path means for that file set", {}, {"for", "path"},
              [&state, &table](const json& args) -> CommandResult {
                  std::string set_name = "open";
                  if (!arg_name(args, "for", set_name)) {
                      return fail("file.answer",
                                  "expected string 'for' (" + file_set_names() + ")");
                  }
                  const std::optional<FileSet> set = file_set_from_name(set_name);
                  if (!set.has_value()) {
                      return fail("file.answer", "'for' names no file set: '" + set_name + "' (" +
                                                    file_set_names() + ")");
                  }
                  std::string path;
                  if (!arg_text(args, "path", path) || path.empty()) {
                      return fail("file.answer", "expected a non-empty string 'path'");
                  }
                  const json picked{{"path", path}};
                  switch (*set) {
                      case FileSet::open:
                          return table.invoke("doc.open", picked);
                      case FileSet::project:
                          return table.invoke("doc.save", picked);
                      case FileSet::flat:
                          return table.invoke("doc.export", picked);
                      case FileSet::image:
                          break;
                  }
                  if (state.doc.empty()) {
                      return fail("file.answer",
                                  "putting a picture into a document needs a document first");
                  }
                  // The middle of the canvas. A picture chosen from a dialog has no pointer to land
                  // under, and arriving in a corner would read as a bug in the import.
                  return table.invoke("layer.import",
                                      json{{"path", path},
                                           {"at", json::array({state.doc.width() / 2,
                                                               state.doc.height() / 2})}});
              });
}

}  // namespace noise::app
