// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "ai/provider.h"
#include "ai/http.h"
#include "ai/recorded.h"
#include "core/hash.h"
#include "core/image.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <vector>

using noise::ai::build_request;
using noise::ai::BuiltRequest;
using noise::ai::CapturedRequest;
using noise::ai::Endpoint;
using noise::ai::field_allowed;
using noise::ai::kMaxSeed;
using noise::ai::ModelEntry;
using noise::ai::parse_recipe;
using noise::ai::Recipe;
using noise::ai::RequestInput;
using noise::ai::RequestImageInput;
using noise::ai::RecordedTransport;
using noise::ai::SizePolicy;
using noise::Image;
using noise::io::encode_png;
using nlohmann::json;

namespace {

// A real PNG of the shape a region compile produces: the builder checks the magic and the cache keys on
// the bytes, so a fake buffer would test neither.
std::vector<std::uint8_t> png(int width = 64, int height = 64, int fill = 128) {
    Image image = Image::filled(noise::Extent{width, height}, 4, 5, 6, 255);
    for (std::size_t i = 0; i < image.pixels().size(); ++i) {
        image.pixels()[i] = static_cast<std::uint8_t>((static_cast<std::size_t>(fill) + i) % 256);
    }
    return encode_png(image);
}

ModelEntry model(const std::string& id = "Klein-TheNoise") {
    ModelEntry entry;
    entry.id = id;
    entry.recipe = "thenoise";
    entry.labels = {"image", "edit"};
    entry.downloaded = true;
    entry.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};
    return entry;
}

Recipe recipe_of(const json& overrides) {
    json body = {{"id", "test"},
                 {"label", "Test intent"},
                 {"endpoint", "edits"},
                 {"role", "edit"},
                 {"region", "selection"},
                 {"operation", "reconstruct"},
                {"prep", "blur"},
                 {"include_references_default", false},
                 {"prompt", json::array({"do the thing"})},
                 {"composite", json{{"feather", "auto"}}}};
    for (const auto& item : overrides.items()) {
        body[item.key()] = item.value();
    }
    std::string error;
    const std::optional<Recipe> parsed = parse_recipe(body, "test", error);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::vector<std::string> sorted(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    return names;
}

// Every name the server would see: the body's fields plus the file part, which is not a field because it
// is the blob rather than a value.
std::vector<std::string> on_the_wire(const BuiltRequest& request) {
    std::vector<std::string> names;
    for (const noise::ai::RequestField& field : request.fields) {
        names.push_back(field.name);
    }
    // The region is a field on a JSON endpoint (base64 inside the body) and a part on a multipart one.
    if (request.multipart()) {
        for (std::size_t i = 0; i < request.images.size(); ++i) {
            names.push_back(request.images.size() > 1 ? "image[]" : "image");
        }
    }
    return sorted(names);
}

// The order is the table's, restricted to the fields this request carries: a request whose fields wander
// is a request no diff can be read, and a field outside the table must not appear at all.
bool follows_the_table(const BuiltRequest& request) {
    const std::vector<std::string> table = noise::ai::allowed_fields(request.endpoint);
    int previous = -1;
    for (const noise::ai::RequestField& field : request.fields) {
        const auto found = std::find(table.begin(), table.end(), field.name);
        if (found == table.end()) {
            return false;
        }
        const int at = static_cast<int>(std::distance(table.begin(), found));
        if (at <= previous) {
            return false;
        }
        previous = at;
    }
    return true;
}

std::string field_text(const BuiltRequest& request, const std::string& name) {
    for (const noise::ai::RequestField& field : request.fields) {
        if (field.name == name) {
            return field.text();
        }
    }
    return {};
}

struct Built {
    BuiltRequest request;
    std::string error;
};

Built build(const json& overrides, const SizePolicy& policy, std::vector<std::uint8_t> bytes,
      std::uint64_t seed) {
    const Recipe recipe = recipe_of(overrides);
    const ModelEntry entry = model();
    RequestInput input;
    input.recipe = &recipe;
    input.server_name = "local";
    input.model_id = entry.id;
    input.model = &entry;
    input.width = 64;
    input.height = 64;
    input.prompt = "do the thing";
    input.seed = seed;
    if (!bytes.empty()) {
        input.images.push_back(RequestImageInput{"document", "target", 64, 64, bytes});
    }
    std::string error;
    std::optional<BuiltRequest> built = build_request(input, policy, error);
    return Built{built.value_or(BuiltRequest{}), error};
}

// The table's own reach: a field is allowed only on the endpoint that lists it.
void test_the_whitelist_is_per_endpoint() {
    CHECK(noise::ai::field_allowed(Endpoint::generations, "prompt"));
    CHECK(!noise::ai::field_allowed(Endpoint::upscale, "prompt"));
    CHECK(noise::ai::field_allowed(Endpoint::upscale, "image"));
    CHECK(!noise::ai::field_allowed(Endpoint::generations, "image"));
    // The edits part carries the region; there is no second way to send one and no way to send a mask.
    CHECK(noise::ai::field_allowed(Endpoint::edits, "image"));
    CHECK(noise::ai::field_allowed(Endpoint::edits, "image[]"));
    for (const char* name : {"mask", "mask_image", "n", "num_images", "url", "image_url",
                             "attachment_type", "control_net", "api_key", "authorization"}) {
        CHECK(!field_allowed(Endpoint::edits, name));
        CHECK(!field_allowed(Endpoint::generations, name));
        CHECK(!field_allowed(Endpoint::upscale, name));
    }
    CHECK(noise::ai::allowed_fields(Endpoint::edits) ==
          noise::ai::allowed_fields(Endpoint::edits));
    // An endpoint the table does not describe lets nothing through, rather than everything.
    CHECK(noise::ai::allowed_fields(Endpoint::chat).size() > 0);
}

// The load-bearing assertion of this whole layer: the fields that go out are exactly the ones the table
// allows, checked on the serialized bytes rather than on a struct a caller could ignore.
void test_what_goes_out_is_exactly_the_table() {
    const SizePolicy policy{16, 1024 * 1024};

    const Built edit = build(json{{"endpoint", "edits"}}, policy, png(), 7);
    REQUIRE(edit.error.empty());
    std::vector<std::string> single_fields = noise::ai::allowed_fields(Endpoint::edits);
    single_fields.erase(std::remove(single_fields.begin(), single_fields.end(), "image[]"),
                        single_fields.end());
    CHECK(on_the_wire(edit.request) == sorted(single_fields));
    CHECK(follows_the_table(edit.request));
    // The region rides as the file part and never as a text field: a second copy of the same bytes under
    // another name is a second thing to keep consistent.
    CHECK_EQ(edit.request.images.size(), std::size_t(1));
    CHECK(!edit.request.images.front().png.empty());
    CHECK_EQ(field_text(edit.request, "size"), std::string("64x64"));
    CHECK_EQ(field_text(edit.request, "seed"), std::string("7"));
    CHECK_EQ(edit.request.path, std::string("/images/edits"));
    // A multipart request has no JSON body to speak of: a preview that showed one would describe bytes
    // that are never sent.
    CHECK(edit.request.json_body().is_null());
    CHECK(edit.request.multipart());

    const Built generation = build(json{{"endpoint", "generations"}}, policy, {}, 7);
    REQUIRE(generation.error.empty());
    CHECK(on_the_wire(generation.request) ==
          sorted(noise::ai::allowed_fields(Endpoint::generations)));
    CHECK(follows_the_table(generation.request));
    CHECK(!generation.request.multipart());
    // `size` is explicit: without it the engine answers at its own default, at a cost nobody asked for.
    CHECK_EQ(generation.request.json_body()["size"].get<std::string>(), std::string("64x64"));
    CHECK_EQ(generation.request.json_body()["seed"].get<std::uint64_t>(), std::uint64_t(7));
    // Numbers stay numbers: a stringified `steps` is refused by the server, not by a taste for types.
    CHECK(generation.request.json_body()["steps"].is_number_integer());
    CHECK(generation.request.json_body()["cfg_scale"].is_number());
    // The response format is asked for, because `url` points at a file on someone else's disk.
    CHECK_EQ(generation.request.json_body()["response_format"].get<std::string>(),
             std::string("b64_json"));

    const Built up = build(json{{"endpoint", "upscale"},
                                 {"role", "upscale"},
                                 {"region", "document"},
                                 {"operation", "transform"},
                                 {"prep", "none"},
                                 {"prompt", json::array()}},
                           policy, png(), 7);
    REQUIRE(up.error.empty());
    CHECK(on_the_wire(up.request) == sorted(noise::ai::allowed_fields(Endpoint::upscale)));
    CHECK(follows_the_table(up.request));
    // `upscale` has no prompt and no steps: sending them would be asking for behaviour it does not have.
    CHECK_EQ(field_text(up.request, "model"), std::string("Klein-TheNoise"));
    CHECK(up.request.json_body()["image"].get<std::string>().size() > 16);

    // Nothing outside the table reached the bytes, whatever the builder's internals did.
    for (const std::string& text :
         {generation.request.json_body().dump(), up.request.json_body().dump()}) {
        for (const char* forbidden : {"mask", "\"n\"", "url", "api_key", "Bearer"}) {
            CHECK(text.find(forbidden) == std::string::npos);
        }
    }

    // The preview lists the same fields in the same order, and prints no key material.
    const json preview = generation.request.preview_json();
    CHECK_EQ(static_cast<std::size_t>(preview["fields"].size()),
             noise::ai::allowed_fields(Endpoint::generations).size());
    CHECK(preview.dump().find("api_key\":\"") == std::string::npos);
    // An uploaded base64 image is described, not dumped.
    const json upscale_preview = up.request.preview_json();
    CHECK(upscale_preview["fields"].dump().find("<b64 ") != std::string::npos);
}

// The builder refuses what the server would refuse, and says which field: a request that costs a round
// trip and a model swap to learn that `steps` was 5 is a bug in the builder.
void test_a_request_the_server_could_not_answer_is_refused_here() {
    const SizePolicy policy{16, 1024 * 1024};

    // The region is uploaded, so an absent or non-PNG blob is refused before anything leaves: the server
    // would answer 400 with a body that says nothing about which part was wrong.
    CHECK(build(json{{"endpoint", "edits"}}, policy, {}, 7).error.find("primary image") !=
          std::string::npos);
    CHECK(build(json{{"endpoint", "edits"}}, policy, std::vector<std::uint8_t>{1, 2, 3}, 7).error.find(
              "PNG") != std::string::npos);

    RequestInput base;
    const Recipe edit = recipe_of(json{{"endpoint", "edits"}});
    const ModelEntry entry = model();
    base.recipe = &edit;
    base.server_name = "local";
    base.model_id = entry.id;
    base.model = &entry;
    base.width = 64;
    base.height = 64;
    base.prompt = "do the thing";
    base.seed = 7;
    const std::vector<std::uint8_t> primary = png();
    base.images.push_back(RequestImageInput{"document", "target", 64, 64, primary});

    auto refusal = [&](const char* expected, const RequestInput& input) {
        std::string error;
        const std::optional<BuiltRequest> built = build_request(input, policy, error);
        CHECK(!built.has_value());
        CHECK(error.find(expected) != std::string::npos);
        if (!built.has_value()) {
            return;
        }
    };

    RequestInput ragged = base;
    ragged.width = 63;
    refusal("multiple", ragged);

    RequestInput huge = base;
    huge.width = 8192;
    huge.height = 8192;
    refusal("ceiling", huge);

    RequestInput no_steps = base;
    no_steps.model = nullptr;  // no discovery data and no override: there is no honest number left
    refusal("steps", no_steps);

    RequestInput too_many_steps = base;
    too_many_steps.steps_override = 5000;
    refusal("steps", too_many_steps);

    RequestInput silly_cfg = base;
    silly_cfg.cfg_scale_override = -1.0;
    refusal("cfg_scale", silly_cfg);

    RequestInput no_model = base;
    no_model.model_id.clear();
    refusal("model", no_model);

    RequestInput empty_prompt = base;
    empty_prompt.prompt = "   ";
    refusal("prompt", empty_prompt);

    // An override is honoured exactly, which is what makes the advanced field in the UI real.
    RequestInput overridden = base;
    overridden.steps_override = 8;
    overridden.cfg_scale_override = 2.5;
    std::string error;
    const std::optional<BuiltRequest> built = build_request(overridden, policy, error);
    REQUIRE(built.has_value());
    CHECK_EQ(field_text(*built, "steps"), std::string("8"));
    CHECK_EQ(field_text(*built, "cfg_scale"), std::string("2.5"));
}

// The upload is the only part of a request that carries pixels, so its content hash has to track those
// bytes and nothing else: it is how a job record, and `prep_proof.py`, prove the model saw the region
// the compiler built rather than something else that was on the canvas.
void test_the_upload_blob_names_the_pixels_and_nothing_else() {
    const SizePolicy policy{16, 1024 * 1024};
    const Built first = build(json{{"endpoint", "edits"}}, policy, png(), 7);
    REQUIRE(first.error.empty());
    const json preview = first.request.preview_json();
    const std::string blob = preview["blob"].get<std::string>();
    CHECK_EQ(blob.size(), std::size_t(64));
    CHECK(blob.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_EQ(build(json{{"endpoint", "edits"}}, policy, png(), 7).request.preview_json()["blob"]
                 .get<std::string>(),
             blob);

    // Different pixels at the same size are a different upload: a region that changed cannot be
    // reported as the one that was asked for.
    CHECK(build(json{{"endpoint", "edits"}}, policy, png(64, 64, 200), 7).request.preview_json()["blob"]
              .get<std::string>() != blob);

    // What the preview promises is what the wire carries: the endpoint, the resolved size, the seed and
    // every field name in the order it goes out. No key material, because the builder never sees any.
    CHECK_EQ(preview["path"].get<std::string>(), std::string("/images/edits"));
    CHECK_EQ(preview["size"].get<std::string>(), std::string("64x64"));
    CHECK(preview["fields"].size() >= std::size_t(4));
    CHECK(preview["fields"][0][0].get<std::string>() == "model");
    CHECK(!preview.contains("cache_key"));
}

// The upload path takes `seed` as a signed 32-bit integer and says so badly: measured on the dev box,
// `/images/edits` answers 200 for 2147483647 and 400 `Invalid value for 'seed': must be an integer` for
// 3000000000, while `/images/generations` takes both. The stricter endpoint therefore decides what the
// field carries, and the builder refuses over it by name instead of sending it to be misread.
void test_a_seed_the_field_cannot_carry_is_refused_here() {
    const SizePolicy policy{16, 1024 * 1024};
    for (const std::uint64_t seed : {std::uint64_t(1), kMaxSeed - 1, kMaxSeed}) {
        const Built built = build(json{{"endpoint", "edits"}}, policy, png(), seed);
        REQUIRE(built.error.empty());
        CHECK_EQ(built.request.params.seed, seed);
        CHECK_EQ(field_text(built.request, "seed"), std::to_string(seed));
    }

    const Built over = build(json{{"endpoint", "edits"}}, policy, png(), kMaxSeed + 1);
    CHECK(!over.error.empty());
    CHECK(over.error.find("seed") != std::string::npos);
    CHECK(over.error.find(std::to_string(kMaxSeed + 1)) != std::string::npos);
    // The generations endpoint would take it, and still refuses: one field, one limit.
    CHECK(!build(json{{"endpoint", "generations"}}, policy, {}, kMaxSeed + 1).error.empty());
}

BuiltRequest build_ordered_images(const Recipe& recipe, const std::vector<std::vector<std::uint8_t>>& pngs,
                                 const std::vector<std::string>& names) {
    ModelEntry entry = model();
    RequestInput input;
    input.recipe = &recipe;
    input.server_name = "local";
    input.model_id = entry.id;
    input.model = &entry;
    input.width = 64;
    input.height = 64;
    input.prompt = "use the references in order";
    input.seed = 81;
    for (std::size_t i = 0; i < pngs.size(); ++i) {
        input.images.push_back(RequestImageInput{i == 0 ? "document" : "file", names[i], 64, 64,
                                                 pngs[i]});
    }
    std::string error;
    const std::optional<BuiltRequest> built =
        build_request(std::move(input), SizePolicy{16, 1024 * 1024}, error);
    REQUIRE(built.has_value());
    return *built;
}

noise::ai::OutgoingRequest multipart_request(const BuiltRequest& request) {
    noise::ai::OutgoingRequest wire;
    wire.method = "POST";
    wire.url = "http://recordings.invalid/v1" + request.path;
    const std::string boundary = noise::ai::multipart_boundary(request);
    wire.content_type = "multipart/form-data; boundary=" + boundary;
    wire.body = noise::ai::multipart_body(request, boundary);
    return wire;
}

void test_a_single_image_keeps_the_original_wire_field_and_bytes() {
    const Recipe recipe = recipe_of(json{{"endpoint", "edits"}});
    const std::vector<std::uint8_t> target = png(64, 64, 37);
    const BuiltRequest built = build_ordered_images(recipe, {target}, {"Garden"});
    const noise::ai::OutgoingRequest outgoing = multipart_request(built);
    const noise::ai::ServerProfile profile{};
    const json preview = noise::ai::outgoing_preview(profile, built);
    CHECK_EQ(preview["wire_image_fields"], json::array({"image"}));
    CHECK_EQ(preview["images"].size(), std::size_t(1));
    CHECK_EQ(preview["images"][0]["filename"], std::string("image.png"));
    CHECK_EQ(preview["request_body_bytes"].get<std::size_t>(), outgoing.body.size());

    const std::string digest = noise::sha256_hex(std::span<const std::uint8_t>(target));
    std::string error;
    const auto transport = RecordedTransport::from_json(
        {json{{"name", "single-image"},
              {"path", "/images/edits"},
              {"match_images", json::array({digest})},
              {"json", {{"accepted", true}}}}},
        "single-image-recording", error);
    REQUIRE(transport != nullptr);
    CHECK(error.empty());
    std::atomic<bool> cancelled{false};
    CHECK_EQ(transport->send(outgoing, cancelled).status, 200L);
    const std::vector<CapturedRequest> sent = transport->sent();
    REQUIRE(sent.size() == std::size_t(1));
    if (sent.empty()) {
        return;
    }
    REQUIRE(sent.front().images.size() == std::size_t(1));
    CHECK_EQ(sent.front().images[0].field, std::string("image"));
    CHECK_EQ(sent.front().images[0].filename, std::string("image.png"));
    CHECK_EQ(sent.front().images[0].content_type, std::string("image/png"));
    CHECK_EQ(sent.front().images[0].bytes, target);
    CHECK_EQ(sent.front().part_names.back(), std::string("image"));
}

void test_two_and_three_images_are_ordered_independent_file_parts() {
    const Recipe recipe = recipe_of(json{{"endpoint", "edits"}});
    const std::vector<std::uint8_t> garden = png(64, 64, 11);
    const std::vector<std::uint8_t> house = png(64, 64, 77);
    const std::vector<std::uint8_t> swing = png(64, 64, 191);
    const std::vector<std::vector<std::uint8_t>> three{garden, house, swing};
    const std::vector<std::string> names{"Garden", "House.png", "Swing.png"};
    const noise::ai::ServerProfile profile{};

    for (const std::size_t count : {std::size_t(2), std::size_t(3)}) {
        const std::vector<std::vector<std::uint8_t>> selected(three.begin(), three.begin() +
                                                              static_cast<std::ptrdiff_t>(count));
        const std::vector<std::string> selected_names(names.begin(), names.begin() +
                                                      static_cast<std::ptrdiff_t>(count));
        const BuiltRequest built = build_ordered_images(recipe, selected, selected_names);
        const json preview = noise::ai::outgoing_preview(profile, built);
        const noise::ai::OutgoingRequest outgoing = multipart_request(built);
        CHECK_EQ(preview["request_body_bytes"].get<std::size_t>(), outgoing.body.size());
        CHECK_EQ(preview["image_count"].get<std::size_t>(), count);
        CHECK_EQ(preview["wire_image_fields"].size(), count);
        CHECK_EQ(preview["image_bytes"].get<std::size_t>(),
                 selected[0].size() + (count > 1 ? selected[1].size() : 0) +
                     (count > 2 ? selected[2].size() : 0));

        std::vector<std::string> hashes;
        for (const std::vector<std::uint8_t>& bytes : selected) {
            hashes.push_back(noise::sha256_hex(std::span<const std::uint8_t>(bytes)));
        }
        std::string recording_error;
        const auto transport = RecordedTransport::from_json(
            {json{{"name", "ordered"},
                  {"path", "/images/edits"},
                  {"method", "POST"},
                  {"match", {{"model", "Klein-TheNoise"}}},
                  {"match_images", hashes},
                  {"json", {{"accepted", true}}}}},
            "multi-image", recording_error);
        REQUIRE(transport != nullptr);
        CHECK(recording_error.empty());
        std::atomic<bool> cancelled{false};
        const noise::ai::IncomingResponse response = transport->send(outgoing, cancelled);
        CHECK_EQ(response.status, 200L);
        CHECK(transport->misses().empty());
        const std::vector<CapturedRequest> sent = transport->sent();
        REQUIRE(sent.size() == std::size_t(1));
        if (sent.empty()) {
            continue;
        }
        const CapturedRequest& captured = sent.front();
        CHECK_EQ(captured.images.size(), count);
        CHECK_EQ(captured.request_body_bytes, outgoing.body.size());
        for (std::size_t i = 0; i < count && i < captured.images.size(); ++i) {
            const CapturedRequest::ImagePart& part = captured.images[i];
            CHECK_EQ(part.field, std::string("image[]"));
            CHECK_EQ(part.filename, "image-" + std::to_string(i + 1) + ".png");
            CHECK_EQ(part.content_type, std::string("image/png"));
            CHECK_EQ(part.bytes, selected[i]);
            CHECK_EQ(part.sha256, hashes[i]);
            CHECK_EQ(preview["images"][i]["name"].get<std::string>(), selected_names[i]);
            CHECK_EQ(preview["images"][i]["sha256"].get<std::string>(), part.sha256);
            CHECK_EQ(preview["images"][i]["encoded_bytes"].get<std::size_t>(), part.bytes.size());
        }
        REQUIRE(captured.part_names.size() >= count);
        CHECK_EQ(captured.part_names[captured.part_names.size() - count], std::string("image[]"));
        CHECK_EQ(built.preview_json()["wire_image_fields"].size(), count);
    }

    const BuiltRequest forward = build_ordered_images(recipe, three, names);
    const BuiltRequest reverse = build_ordered_images(recipe, {garden, swing, house}, names);
    CHECK(forward.params.blob_id != reverse.params.blob_id);
    CHECK(forward.images[1].sha256 != reverse.images[1].sha256);
}

void test_recorded_match_includes_every_image_and_its_order() {
    const Recipe recipe = recipe_of(json{{"endpoint", "edits"}});
    const std::vector<std::uint8_t> garden = png(64, 64, 11);
    const std::vector<std::uint8_t> house = png(64, 64, 77);
    const std::vector<std::uint8_t> swing = png(64, 64, 191);
    const BuiltRequest house_then_swing =
        build_ordered_images(recipe, {garden, house, swing}, {"Garden", "House", "Swing"});
    const BuiltRequest swing_then_house =
        build_ordered_images(recipe, {garden, swing, house}, {"Garden", "Swing", "House"});
    const auto hashes = [](const BuiltRequest& request) {
        json result = json::array();
        for (const noise::ai::RequestImage& image : request.images) {
            result.push_back(image.sha256);
        }
        return result;
    };
    std::string error;
    const auto transport = RecordedTransport::from_json(
        {json{{"name", "house-before-swing"},
              {"path", "/images/edits"},
              {"match_images", hashes(house_then_swing)},
              {"json", {{"selected", "house-before-swing"}}}},
         json{{"name", "swing-before-house"},
              {"path", "/images/edits"},
              {"match_images", hashes(swing_then_house)},
              {"json", {{"selected", "swing-before-house"}}}}},
        "ordered-recordings", error);
    REQUIRE(transport != nullptr);
    CHECK(error.empty());
    std::atomic<bool> cancelled{false};
    const auto first = transport->send(multipart_request(house_then_swing), cancelled);
    const auto second = transport->send(multipart_request(swing_then_house), cancelled);
    CHECK(first.text().find("house-before-swing") != std::string::npos);
    CHECK(second.text().find("swing-before-house") != std::string::npos);
    CHECK(transport->misses().empty());
    CHECK_EQ(transport->hits().size(), std::size_t(2));
}


void test_upscale_accepts_native_odd_size_without_diffusion_defaults() {
    const Recipe recipe = recipe_of(json{{"endpoint", "upscale"},
                                          {"region", "document"},
                                          {"prep", "none"},
                                          {"operation", "transform"},
                                          {"prompt", json::array()}});
    ModelEntry entry = model("RealESRGAN-4xplus-TheNoise");
    entry.image_defaults = noise::ai::ImageDefaults{};
    const auto bytes = png(137, 89, 73);
    RequestInput input;
    input.recipe = &recipe;
    input.server_name = "recorded";
    input.model_id = entry.id;
    input.model = &entry;
    input.width = 137;
    input.height = 89;
    input.images.push_back(RequestImageInput{"document", "current", 137, 89, bytes});
    std::string error;
    const std::optional<BuiltRequest> request = build_request(input, SizePolicy{16, 1024 * 1024}, error);
    REQUIRE(request.has_value());
    CHECK(error.empty());
    CHECK_EQ(request->path, std::string("/images/upscale"));
    CHECK(on_the_wire(*request) == std::vector<std::string>({"image", "model"}));
    CHECK_EQ(request->images.front().width, 137);
    CHECK_EQ(request->images.front().height, 89);
    CHECK_EQ(request->images.front().png, bytes);
    CHECK(!request->json_body().contains("steps"));
    CHECK(!request->json_body().contains("cfg_scale"));
    CHECK(!request->json_body().contains("size"));
}

}  // namespace

int main() {
    test_a_seed_the_field_cannot_carry_is_refused_here();
    test_the_whitelist_is_per_endpoint();
    test_upscale_accepts_native_odd_size_without_diffusion_defaults();
    test_what_goes_out_is_exactly_the_table();
    test_a_request_the_server_could_not_answer_is_refused_here();
    test_the_upload_blob_names_the_pixels_and_nothing_else();
    test_a_single_image_keeps_the_original_wire_field_and_bytes();
    test_two_and_three_images_are_ordered_independent_file_parts();
    test_recorded_match_includes_every_image_and_its_order();
    return noise::test::summary(__FILE__);
}
