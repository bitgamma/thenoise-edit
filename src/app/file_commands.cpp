// SPDX-License-Identifier: Apache-2.0
#include "app/file_commands.h"

#include "app/command_args.h"
#include "app/command_groups.h"
#include "io/flat.h"
#include "io/project.h"

#include <nlohmann/json.hpp>

#include <thread>
#include <utility>

namespace noise::app {

namespace {

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
    }
    return "a file";
}

platform::PickKind pick_kind(FileSet set) {
    return set == FileSet::open ? platform::PickKind::open : platform::PickKind::save;
}

std::vector<platform::FileFilter> file_filters(FileSet set) {
    const std::vector<std::string> images = io::flat_export_extension_list();
    switch (set) {
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

void register_file_commands(AppState& state, CommandTable& table) {
    // The picker blocks for as long as the person takes, and a frame loop that waits for it stops
    // answering the wire. So the answer arrives on a worker and `file.pick.state` is what anyone reads.
    table.add("file.pick", "Ask the desktop for a path; read the answer from file.pick.state", {},
              {"for", "title", "path", "multiple"}, [&state](const json& args) -> CommandResult {
                  std::string set_name = "open";
                  if (!arg_name(args, "for", set_name)) {
                      return fail("file.pick", "expected string 'for' (open, project, flat)");
                  }
                  const std::optional<FileSet> set = file_set_from_name(set_name);
                  if (!set.has_value()) {
                      return fail("file.pick", "'for' names no file set: '" + set_name +
                                                  "' (open, project, flat)");
                  }
                  platform::PickRequest request{};
                  request.kind = pick_kind(*set);
                  request.filters = file_filters(*set);
                  request.title = "Choose " + file_set_label(*set);
                  if (!arg_name(args, "title", request.title)) {
                      return fail("file.pick", "expected string 'title'");
                  }
                  if (!arg_name(args, "path", request.initial_path)) {
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
}

}  // namespace noise::app
