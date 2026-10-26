// SPDX-License-Identifier: Apache-2.0
#include "app/ai_references.h"

#include "app/command_args.h"
#include "app/state.h"
#include "core/hash.h"
#include "core/resample.h"
#include "io/flat.h"
#include "io/png.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace noise::app {
namespace {

inline constexpr int kReferenceThumbnailLongEdgePx = 44;

Image thumbnail_of(const Image& image) {
    const int long_edge = std::max(image.size().w, image.size().h);
    if (long_edge <= kReferenceThumbnailLongEdgePx) {
        return image;
    }
    const double scale = static_cast<double>(kReferenceThumbnailLongEdgePx) /
                         static_cast<double>(long_edge);
    const Extent size{std::max(1, static_cast<int>(std::lround(image.size().w * scale))),
                      std::max(1, static_cast<int>(std::lround(image.size().h * scale)))};
    return resample_lanczos3(image, size);
}

void cache_image(AiReference& reference, const Image& image) {
    reference.size = image.size();
    reference.png = io::encode_png(image);
    reference.thumbnail = thumbnail_of(image);
    reference.sha256 = sha256_hex(std::span<const std::uint8_t>(reference.png));
    reference.error.clear();
    ++reference.revision;
}

std::string reference_id(std::uint64_t number) {
    return "ref-" + std::to_string(number);
}

std::string image_name(const std::string& path) {
    std::string name = std::filesystem::path(path).filename().string();
    return name.empty() ? path : name;
}

const Layer* find_layer(const AppState& state, std::uint64_t id) {
    for (const Layer& layer : state.doc.layers()) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

Image layer_image(const Layer& layer) {
    const Extent size = layer.pixels.size();
    Rect bounds{size.w, size.h, 0, 0};
    int right = 0;
    int bottom = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t alpha = layer.pixels.at(x, y)[3];
            const std::uint8_t coverage = layer.mask.has_value()
                                              ? layer.mask->coverage(x, y)
                                              : kMaskIn;
            const int masked = (static_cast<int>(alpha) * static_cast<int>(coverage) + 127) / 255;
            const int shown = static_cast<int>(std::lround(masked * layer.opacity));
            if (shown == 0) {
                continue;
            }
            bounds.x = std::min(bounds.x, x);
            bounds.y = std::min(bounds.y, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (right <= bounds.x || bottom <= bounds.y) {
        return {};
    }
    bounds.w = right - bounds.x;
    bounds.h = bottom - bounds.y;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bounds.w) *
                                     static_cast<std::size_t>(bounds.h) * 4);
    for (int y = 0; y < bounds.h; ++y) {
        for (int x = 0; x < bounds.w; ++x) {
            const std::uint8_t* source = layer.pixels.at(bounds.x + x, bounds.y + y);
            std::uint8_t* target = pixels.data() +
                                   (static_cast<std::size_t>(y) *
                                        static_cast<std::size_t>(bounds.w) +
                                    static_cast<std::size_t>(x)) *
                                       4;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
            const std::uint8_t coverage = layer.mask.has_value()
                                              ? layer.mask->coverage(bounds.x + x, bounds.y + y)
                                              : kMaskIn;
            const int masked = (static_cast<int>(source[3]) * static_cast<int>(coverage) + 127) / 255;
            target[3] = static_cast<std::uint8_t>(std::lround(masked * layer.opacity));
            if (target[3] == 0) {
                target[0] = 0;
                target[1] = 0;
                target[2] = 0;
            }
        }
    }
    return Image::from_rgba(Extent{bounds.w, bounds.h}, std::move(pixels));
}

nlohmann::json references_json(const AppState& state) {
    nlohmann::json items = nlohmann::json::array();
    for (std::size_t i = 0; i < state.ai.references.size(); ++i) {
        const AiReference& reference = state.ai.references[i];
        items.push_back(nlohmann::json{{"id", reference.id},
                                      {"number", i + 2},
                                      {"source", reference.source},
                                      {"name", reference.name},
                                      {"layer_id", reference.source_layer_id == 0
                                                        ? nlohmann::json(nullptr)
                                                        : nlohmann::json(reference.source_layer_id)},
                                      {"linked", reference.source_layer_id != 0 &&
                                                     reference.source_document_epoch ==
                                                         state.document_epoch &&
                                                     find_layer(state, reference.source_layer_id) !=
                                                         nullptr},
                                      {"dimensions", nlohmann::json::array(
                                                         {reference.size.w, reference.size.h})},
                                      {"encoded_bytes", reference.png.size()},
                                      {"sha256", reference.sha256},
                                      {"revision", reference.revision},
                                      {"error", reference.error}});
    }
    return nlohmann::json{{"session_only", true},
                          {"primary", state.doc.empty()
                                          ? nlohmann::json(nullptr)
                                          : nlohmann::json{{"number", 1},
                                                           {"source", "document"},
                                                           {"name", state.doc.path().empty()
                                                                        ? "Current image"
                                                                        : image_name(state.doc.path())},
                                                           {"dimensions", nlohmann::json::array(
                                                                              {state.doc.width(),
                                                                               state.doc.height()})}}},
                          {"references", items},
                          {"count", state.ai.references.size()},
                          {"encodes", state.ai.reference_encodes},
                          {"error", state.ai.references_error}};
}

bool parse_paths(const nlohmann::json& args, std::vector<std::string>& paths,
                 std::string& error) {
    if (args.contains("path") && args.contains("paths")) {
        error = "'path' and 'paths' cannot be used together";
        return false;
    }
    if (args.contains("path")) {
        if (!args.at("path").is_string() || args.at("path").get<std::string>().empty()) {
            error = "'path' must be a non-empty string";
            return false;
        }
        paths.push_back(args.at("path").get<std::string>());
        return true;
    }
    if (!args.contains("paths") || !args.at("paths").is_array() || args.at("paths").empty()) {
        error = "expected non-empty string 'path' or array 'paths'";
        return false;
    }
    for (const nlohmann::json& path : args.at("paths")) {
        if (!path.is_string() || path.get<std::string>().empty()) {
            error = "every 'paths' entry must be a non-empty string";
            return false;
        }
        paths.push_back(path.get<std::string>());
    }
    return true;
}

void add_file_references(AppState& state, const std::vector<std::string>& paths) {
    std::vector<AiReference> added;
    std::uint64_t next_id = state.ai.next_reference_id;
    for (const std::string& path : paths) {
        std::string error;
        const Image image = io::load_flat_image(path, error);
        if (image.empty()) {
            throw std::runtime_error(path + ": " + error);
        }
        AiReference reference;
        reference.id = reference_id(next_id++);
        reference.source = "file";
        reference.name = image_name(path);
        cache_image(reference, image);
        added.push_back(std::move(reference));
    }
    state.ai.references.insert(state.ai.references.end(), std::make_move_iterator(added.begin()),
                               std::make_move_iterator(added.end()));
    state.ai.next_reference_id = next_id;
    state.ai.reference_encodes += paths.size();
}

AiReference layer_reference(AppState& state, const Layer& layer) {
    const Image image = layer_image(layer);
    if (image.empty()) {
        throw std::runtime_error("layer '" + layer.name + "' has no visible pixels after its mask");
    }
    AiReference reference;
    reference.id = reference_id(state.ai.next_reference_id);
    reference.source = "layer";
    reference.name = layer.name;
    reference.source_layer_id = layer.id;
    reference.source_document_epoch = state.document_epoch;
    reference.source_revision = layer.content_revision;
    reference.source_opacity = layer.opacity;
    cache_image(reference, image);
    ++state.ai.next_reference_id;
    ++state.ai.reference_encodes;
    return reference;
}

void set_error(AppState& state, const std::string& error) {
    state.ai.references_error = error;
}

}  // namespace

void refresh_ai_references(AppState& state) {
    for (AiReference& reference : state.ai.references) {
        if (reference.source_layer_id == 0) {
            continue;
        }
        if (reference.source_document_epoch != state.document_epoch) {
            reference.source_layer_id = 0;
            continue;
        }
        const Layer* layer = find_layer(state, reference.source_layer_id);
        if (layer == nullptr) {
            reference.error.clear();
            continue;
        }
        if (reference.source_revision == layer->content_revision &&
            reference.source_opacity == layer->opacity) {
            reference.name = layer->name;
            continue;
        }
        const Image image = layer_image(*layer);
        if (image.empty()) {
            reference.error = "layer '" + layer->name + "' has no visible pixels after its mask";
            reference.name = layer->name;
            reference.source_revision = layer->content_revision;
            reference.source_opacity = layer->opacity;
            continue;
        }
        reference.name = layer->name;
        reference.source_revision = layer->content_revision;
        reference.source_opacity = layer->opacity;
        cache_image(reference, image);
        ++state.ai.reference_encodes;
    }
}

nlohmann::json ai_references_json(AppState& state) {
    refresh_ai_references(state);
    return references_json(state);
}

void register_ai_reference_commands(AppState& state, CommandTable& table) {
    table.add("ai.references", "Read the ordered AI reference images", {}, {},
              [&state](const nlohmann::json&) -> CommandResult {
                  return command_ok(ai_references_json(state));
              });

    table.add("ai.references.add", "Add independent image files or one document layer to AI references",
              {}, {"path", "paths", "layer"}, [&state](const nlohmann::json& args) -> CommandResult {
                  state.ai.references_error.clear();
                  try {
                      if (args.contains("layer")) {
                          if (args.contains("path") || args.contains("paths")) {
                              throw std::runtime_error("'layer' cannot be combined with 'path' or 'paths'");
                          }
                          int index = 0;
                          if (!want_int(args, "layer", index)) {
                              throw std::runtime_error("'layer' must be a document layer index");
                          }
                          if (index < 0 || static_cast<std::size_t>(index) >= state.doc.layers().size()) {
                              throw std::runtime_error("'layer' index " + std::to_string(index) +
                                                      " is outside the document's " +
                                                      std::to_string(state.doc.layers().size()) +
                                                      " layers");
                          }
                          const Layer& layer = state.doc.layer(static_cast<std::size_t>(index));
                          state.ai.references.push_back(layer_reference(state, layer));
                      } else {
                          std::vector<std::string> paths;
                          std::string error;
                          if (!parse_paths(args, paths, error)) {
                              throw std::runtime_error(error);
                          }
                          add_file_references(state, paths);
                      }
                  } catch (const std::runtime_error& error) {
                      set_error(state, error.what());
                      return fail("ai.references.add", error.what());
                  }
                  return command_ok(ai_references_json(state));
              });

    table.add("ai.references.remove", "Remove one additional AI reference", {}, {"id"},
              [&state](const nlohmann::json& args) -> CommandResult {
                  if (!args.contains("id") || !args.at("id").is_string()) {
                      return fail("ai.references.remove", "expected string 'id' from ai.references");
                  }
                  const std::string id = args.at("id").get<std::string>();
                  const auto found = std::find_if(state.ai.references.begin(), state.ai.references.end(),
                                                  [&id](const AiReference& reference) {
                                                      return reference.id == id;
                                                  });
                  if (found == state.ai.references.end()) {
                      return fail("ai.references.remove", "no additional reference with id '" + id + "'");
                  }
                  state.ai.references.erase(found);
                  state.ai.references_error.clear();
                  return command_ok(ai_references_json(state));
              });

    table.add("ai.references.move", "Reorder an additional AI reference", {}, {"id", "to"},
              [&state](const nlohmann::json& args) -> CommandResult {
                  std::string id;
                  int to = -1;
                  if (!arg_name(args, "id", id) || !want_int(args, "to", to)) {
                      return fail("ai.references.move", "expected string 'id' and destination 'to'");
                  }
                  const auto found = std::find_if(state.ai.references.begin(), state.ai.references.end(),
                                                  [&id](const AiReference& reference) {
                                                      return reference.id == id;
                                                  });
                  if (found == state.ai.references.end()) {
                      return fail("ai.references.move", "no additional reference with id '" + id + "'");
                  }
                  const int from = static_cast<int>(std::distance(state.ai.references.begin(), found));
                  const int count = static_cast<int>(state.ai.references.size());
                  if (to < 0 || to >= count) {
                      return fail("ai.references.move", "'to' " + std::to_string(to) +
                                                         " is outside the " + std::to_string(count) +
                                                         " additional references");
                  }
                  if (from != to) {
                      AiReference moved = std::move(*found);
                      state.ai.references.erase(found);
                      state.ai.references.insert(state.ai.references.begin() + to, std::move(moved));
                  }
                  return command_ok(ai_references_json(state));
              });
}

}  // namespace noise::app
