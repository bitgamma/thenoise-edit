// SPDX-License-Identifier: Apache-2.0
#include "ai/provider.h"

#include "core/base64.h"
#include "core/hash.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace noise::ai {

namespace {

// The one ceiling on a request dimension. It is a client-side guard for the "size rejected" failure
// row: the engine snaps an odd size rather than refusing it, so the sizes worth stopping here are the
// ones out of range, and a refusal that names the number is cheaper than a minute of GPU time.
inline constexpr int kMaxDimensionPx = 4096;
inline constexpr int kMaxSteps = 128;
inline constexpr const char* kResponseFormat = "b64_json";
const std::uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

struct EndpointShape {
    std::string path;
    std::vector<std::string> fields;
};

// Data, so adding a field is adding a row and the compiler still checks every endpoint is in the table.
EndpointShape shape_of(Endpoint endpoint) {
    switch (endpoint) {
        case Endpoint::generations:
            return EndpointShape{"/images/generations",
                                 {"model", "prompt", "size", "steps", "cfg_scale", "seed",
                                  "response_format"}};
        case Endpoint::edits:
            return EndpointShape{"/images/edits",
                                 {"model", "image", "image[]", "prompt", "size", "steps",
                                  "cfg_scale", "seed", "response_format"}};
        case Endpoint::upscale:
            return EndpointShape{"/images/upscale", {"model", "image"}};
        case Endpoint::chat:
            return EndpointShape{"/chat/completions", {"model", "messages", "stream"}};
    }
    // Unreachable while the switch covers every enumerator, which is what the build checks. Left empty
    // rather than repeating a row: an empty field list lets no field through, so an endpoint the table
    // does not know builds nothing instead of guessing.
    return EndpointShape{};
}

bool is_png(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(kPngMagic)) {
        return false;
    }
    return std::equal(std::begin(kPngMagic), std::end(kPngMagic), bytes.begin());
}

std::string image_set_id(const std::vector<RequestImage>& images) {
    if (images.empty()) {
        return sha256_hex(std::span<const std::uint8_t>());
    }
    if (images.size() == 1) {
        return images.front().sha256;
    }
    std::string identity = "noise-edit-images-v1\n";
    for (const RequestImage& image : images) {
        identity += std::to_string(image.png.size()) + ":" + image.sha256 + "\n";
    }
    return sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
}

bool blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += names[i];
    }
    return out;
}

std::string size_field(int width, int height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

std::string trimmed_double(double value) {
    std::string text = std::to_string(value);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

// Every field of a request passes through here, so the whitelist is enforced on the way in rather than
// audited afterwards: a field that is not in the endpoint's set never becomes a body at all.
class FieldSink {
public:
    explicit FieldSink(Endpoint endpoint) : endpoint_(endpoint) {}

    void add_text(const std::string& name, const std::string& value) {
        add(name, nlohmann::json(value));
    }
    void add_number(const std::string& name, double value) { add(name, nlohmann::json(value)); }
    void add_integer(const std::string& name, std::int64_t value) {
        add(name, nlohmann::json(value));
    }

    bool ok() const { return ok_; }
    std::vector<RequestField> fields() const { return fields_; }

private:
    void add(const std::string& name, nlohmann::json value) {
        ok_ = ok_ && field_allowed(endpoint_, name);
        if (!ok_) {
            return;
        }
        fields_.push_back(RequestField{name, std::move(value)});
    }

    Endpoint endpoint_;
    std::vector<RequestField> fields_;
    bool ok_ = true;
};

}  // namespace

std::string RequestField::text() const {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number()) {
        return trimmed_double(value.get<double>());
    }
    return value.dump();
}

bool field_allowed(Endpoint endpoint, const std::string& name) {
    const std::vector<std::string> fields = shape_of(endpoint).fields;
    return std::find(fields.begin(), fields.end(), name) != fields.end();
}

std::vector<std::string> allowed_fields(Endpoint endpoint) { return shape_of(endpoint).fields; }

std::string request_path(Endpoint endpoint) { return shape_of(endpoint).path; }

nlohmann::json BuiltRequest::json_body() const {
    if (multipart()) {
        return nullptr;
    }
    nlohmann::json body = nlohmann::json::object();
    for (const RequestField& field : fields) {
        body[field.name] = field.value;
    }
    return body;
}

nlohmann::json BuiltRequest::preview_json() const {
    nlohmann::json fields = nlohmann::json::array();
    for (const RequestField& field : this->fields) {
        // A base64 PNG is not something to print: the preview says how much went and where it is.
        const nlohmann::json shown =
            (field.name == "image" || field.name == "image[]") && field.value.is_string()
                ? nlohmann::json("<b64 " + std::to_string(field.value.get<std::string>().size()) +
                                 " chars>")
                : field.value;
        fields.push_back(nlohmann::json::array({field.name, shown}));
    }
    nlohmann::json image_rows = nlohmann::json::array();
    std::size_t total_image_bytes = 0;
    for (std::size_t i = 0; i < images.size(); ++i) {
        const RequestImage& image = images[i];
        const std::string field = endpoint == Endpoint::edits && images.size() > 1
                                      ? "image[]"
                                      : "image";
        image_rows.push_back(nlohmann::json{{"index", i + 1},
                                           {"field", field},
                                           {"source", image.source},
                                           {"name", image.name},
                                           {"dimensions", nlohmann::json::array(
                                                              {image.width, image.height})},
                                           {"encoded_bytes", image.png.size()},
                                           {"content_type", "image/png"},
                                           {"filename", images.size() == 1
                                                             ? "image.png"
                                                             : "image-" + std::to_string(i + 1) +
                                                                   ".png"},
                                           {"sha256", image.sha256}});
        total_image_bytes += image.png.size();
    }
    return nlohmann::json{{"endpoint", endpoint_name(endpoint)},
                          {"path", path},
                          {"model", params.model_id},
                          {"server", params.server_name},
                          {"size", size_field(params.width, params.height)},
                          {"steps", params.steps},
                          {"cfg_scale", params.cfg_scale},
                          {"seed", params.seed},
                          {"prompt", params.prompt},
                          {"fields", fields},
                          {"wire_image_fields", [&] {
                               nlohmann::json names = nlohmann::json::array();
                               for (const nlohmann::json& image : image_rows) {
                                   names.push_back(image["field"]);
                               }
                               return names;
                           }()},
                          {"images", image_rows},
                          {"image_count", images.size()},
                          {"image_bytes", total_image_bytes},
                          {"blob", params.blob_id},
                          {"allowed_fields", allowed_fields(endpoint)}};
}

std::optional<BuiltRequest> build_request(RequestInput input, const SizePolicy& size_policy,
                                         std::string& error) {
    if (input.recipe == nullptr) {
        error = "build_request: no recipe";
        return std::nullopt;
    }
    const Recipe& recipe = *input.recipe;
    if (recipe.endpoint == Endpoint::chat) {
        error = "recipe '" + recipe.id + "': the chat endpoint is the `enhance` role's, and nothing "
                                        "sends it yet";
        return std::nullopt;
    }
    if (input.model_id.empty()) {
        error = "recipe '" + recipe.id + "': role '" + recipe.role +
                "' resolved to no model, so there is nothing to name in the 'model' field";
        return std::nullopt;
    }
    if (input.width <= 0 || input.height <= 0) {
        error = "recipe '" + recipe.id + "': size must be positive, got " +
                size_field(input.width, input.height);
        return std::nullopt;
    }
    if (input.width > kMaxDimensionPx || input.height > kMaxDimensionPx) {
        error = "recipe '" + recipe.id + "': size " + size_field(input.width, input.height) +
                " exceeds the " + std::to_string(kMaxDimensionPx) +
                "px ceiling; compile a smaller region";
        return std::nullopt;
    }
    if (size_policy.multiple > 1 &&
        (input.width % size_policy.multiple != 0 || input.height % size_policy.multiple != 0)) {
        // The region compiler rounds up to this multiple, so arriving here means a request was built
        // around it. The engine would snap the size and answer at dimensions nobody planned.
        error = "recipe '" + recipe.id + "': size " + size_field(input.width, input.height) +
                " is not a multiple of the policy's " + std::to_string(size_policy.multiple) +
                " - send this through plan_region";
        return std::nullopt;
    }

    const bool wants_prompt =
        recipe.endpoint == Endpoint::generations || recipe.endpoint == Endpoint::edits;
    if (wants_prompt && blank(input.prompt)) {
        error = "recipe '" + recipe.id + "': 'prompt' is empty, and the server refuses a request "
                                        "without one only for some endpoints - build nothing";
        return std::nullopt;
    }
    const bool wants_image = recipe.endpoint == Endpoint::edits || recipe.endpoint == Endpoint::upscale;
    if (recipe.endpoint == Endpoint::edits && input.images.empty()) {
        error = "recipe '" + recipe.id + "': the edits endpoint needs the primary image as Image 1";
        return std::nullopt;
    }
    if (recipe.endpoint == Endpoint::upscale && input.images.size() != 1) {
        error = "recipe '" + recipe.id + "': upscale accepts exactly one image";
        return std::nullopt;
    }
    if (!wants_image && !input.images.empty()) {
        error = "recipe '" + recipe.id + "': endpoint '" + endpoint_name(recipe.endpoint) +
                "' does not accept image references; choose an edit recipe or remove them";
        return std::nullopt;
    }
    const std::string image_field = input.images.size() > 1 ? "image[]" : "image";
    if (recipe.endpoint == Endpoint::edits && !field_allowed(recipe.endpoint, image_field)) {
        error = "recipe '" + recipe.id + "': image field '" + image_field +
                "' is not in the tier-1 field policy";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < input.images.size(); ++i) {
        const RequestImageInput& image = input.images[i];
        if (!is_png(image.png)) {
            error = "recipe '" + recipe.id + "': Image " + std::to_string(i + 1) + " ('" +
                    image.name + "') is " + std::to_string(image.png.size()) +
                    " bytes and does not start with a PNG header";
            return std::nullopt;
        }
        if (image.width <= 0 || image.height <= 0) {
            error = "recipe '" + recipe.id + "': Image " + std::to_string(i + 1) + " ('" +
                    image.name + "') has invalid dimensions " + size_field(image.width, image.height);
            return std::nullopt;
        }
        if (image.name.empty() || image.source.empty()) {
            error = "recipe '" + recipe.id + "': Image " + std::to_string(i + 1) +
                    " needs a source and a name for its preview";
            return std::nullopt;
        }
    }
    if (recipe.endpoint == Endpoint::upscale && !field_allowed(recipe.endpoint, "image")) {
        error = "recipe '" + recipe.id + "': field 'image' is not in the tier-1 field policy";
        return std::nullopt;
    }

    // `seed` goes out as a signed integer and the engines read it as 32 bits, so a larger value leaves
    // here as a number the answer comes back refusing to parse. Checked here because this is the one
    // place the field is formed, and the refusal names both numbers.
    if (input.seed > kMaxSeed) {
        error = "recipe '" + recipe.id + "': 'seed' is " + std::to_string(input.seed) +
                ", above the largest the field carries (" + std::to_string(kMaxSeed) + ")";
        return std::nullopt;
    }

    BuiltRequest request;
    request.endpoint = recipe.endpoint;
    request.path = shape_of(recipe.endpoint).path;
    request.params.server_name = input.server_name;
    request.params.model_id = input.model_id;
    request.params.prompt = input.prompt;
    request.params.width = input.width;
    request.params.height = input.height;
    request.params.seed = input.seed;
    for (const RequestImageInput& image : input.images) {
        RequestImage encoded;
        encoded.source = image.source;
        encoded.name = image.name;
        encoded.width = image.width;
        encoded.height = image.height;
        encoded.png.assign(image.png.begin(), image.png.end());
        encoded.sha256 = sha256_hex(image.png);
        request.images.push_back(std::move(encoded));
    }
    request.params.blob_id = image_set_id(request.images);

    // Steps and CFG are the model's numbers. An override is what the user asked for on the command
    // line; guessing a number here would be the app asserting a fact the server already publishes.
    const int steps = input.steps_override.has_value()
                          ? *input.steps_override
                          : (input.model != nullptr ? input.model->image_defaults.steps : 0);
    if (steps <= 0 || steps > kMaxSteps) {
        error = "recipe '" + recipe.id + "': no usable 'steps' for model '" + input.model_id +
                "' (got " + std::to_string(steps) + ", ceiling " + std::to_string(kMaxSteps) +
                "); the model's image_defaults do not carry one, so pass steps";
        return std::nullopt;
    }
    const double cfg = input.cfg_scale_override.has_value()
                           ? *input.cfg_scale_override
                           : (input.model != nullptr ? input.model->image_defaults.cfg_scale : 0.0);
    if (!(cfg >= 0.0) || std::isnan(cfg) || cfg > 100.0) {
        error = "recipe '" + recipe.id + "': no usable 'cfg_scale' for model '" + input.model_id +
                "' (got " + trimmed_double(cfg) + ")";
        return std::nullopt;
    }
    request.params.steps = steps;
    request.params.cfg_scale = cfg;

    FieldSink sink(recipe.endpoint);
    sink.add_text("model", input.model_id);
    if (recipe.endpoint == Endpoint::upscale) {
        sink.add_text("image", base64_encode(input.images.front().png));
    } else {
        sink.add_text("prompt", input.prompt);
        sink.add_text("size", size_field(input.width, input.height));
        sink.add_integer("steps", steps);
        sink.add_number("cfg_scale", cfg);
        sink.add_integer("seed", static_cast<std::int64_t>(input.seed));
        // Always b64: a `url` response hands back an address on the server's disk, which the app has
        // no business resolving and which no test can replay.
        sink.add_text("response_format", kResponseFormat);
    }
    if (!sink.ok()) {
        error = "recipe '" + recipe.id + "': a field is not in the tier-1 set for " +
                endpoint_name(recipe.endpoint) + " (allowed: " +
                join(allowed_fields(recipe.endpoint)) + ")";
        return std::nullopt;
    }
    request.fields = sink.fields();
    return request;
}

}  // namespace noise::ai
