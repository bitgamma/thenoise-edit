// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "ai/cache.h"
#include "ai/http.h"
#include "ai/queue.h"
#include "ai/recorded.h"
#include "ai/server.h"

#include "core/hash.h"
#include "core/image.h"
#include "io/file.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>

#include <iterator>
#include <string>
#include <vector>

using noise::ai::AiError;
using noise::ai::BlobCache;
using noise::ai::build_request;
using noise::ai::BuiltRequest;
using noise::ai::Client;
using noise::ai::Endpoint;
using noise::ai::FailureClass;
using noise::ai::failure_name;
using noise::ai::JobQueue;
using noise::ai::JobState;
using noise::ai::Landing;
using noise::ai::LatencyModel;
using noise::ai::load_recipes;
using noise::ai::ModelEntry;
using noise::ai::parse_health;
using noise::ai::RecordedTransport;
using noise::ai::RecipeSet;
using noise::ai::RequestInput;
using noise::ai::ServerProfile;
using noise::ai::SizePolicy;
using noise::Extent;
using noise::Image;
using noise::io::decode_png;
using noise::io::encode_png;
using noise::io::read_file;
using noise::io::write_file_atomic;
using nlohmann::json;

namespace {

const std::string kModel = "Flux-2-Klein-9B-TheNoise";
const std::string kUpscaler = "RealESRGAN-4xplus-TheNoise";

std::vector<std::uint8_t> bytes_of(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string string_of(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

ServerProfile profile() {
    ServerProfile server;
    server.name = "recorded";
    server.base_url = "http://recorded.invalid:13305/v1";
    server.api_key = "test-key-not-a-secret";
    return server;
}

// The fixtures are the server: they are the shapes it was observed sending, and the default test run may
// not open a socket. A missing recording is a refusal, never an invented answer.
std::unique_ptr<RecordedTransport> fixtures() {
    std::string error;
    std::unique_ptr<RecordedTransport> loaded =
        RecordedTransport::load(NOISE_TEST_FIXTURES_DIR, error);
    REQUIRE(loaded != nullptr);
    return loaded;
}

std::vector<std::uint8_t> region_png() {
    Image image = Image::filled(Extent{64, 64}, 10, 20, 30, 255);
    for (std::size_t i = 0; i < image.pixels().size(); ++i) {
        image.pixels()[i] = static_cast<std::uint8_t>(i % 256);
    }
    return encode_png(image);
}

// A canvas-sized coverage PNG: the queue stores these bytes beside the job so a late answer composites
// under the mask that was in force when it was asked for.
std::vector<std::uint8_t> coverage_png() {
    return encode_png(Image::filled(Extent{256, 256}, 0, 0, 0, 255));
}

BuiltRequest edits_request_for(const RecipeSet& recipes, const std::string& model, int width = 64,
                               int height = 64) {
    const noise::ai::Recipe* recipe = recipes.find("inpaint-remove");
    REQUIRE(recipe != nullptr);
    ModelEntry entry;
    entry.id = model;
    entry.recipe = "thenoise";
    entry.labels = {"image", "edit"};
    entry.downloaded = true;
    entry.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};

    RequestInput input;
    input.recipe = recipe;
    input.server_name = "recorded";
    input.model_id = model;
    input.model = &entry;
    input.width = width;
    input.height = height;
    input.prompt = "remove the object, match the wall";
    input.seed = 1234;
    input.png = region_png();
    std::string error;
    const std::optional<BuiltRequest> built =
        build_request(input, SizePolicy{16, 1024 * 1024}, error);
    REQUIRE(built.has_value());
    return *built;
}

BuiltRequest edits_request(const RecipeSet& recipes, int width = 64, int height = 64) {
    const noise::ai::Recipe* recipe = recipes.find("inpaint-remove");
    REQUIRE(recipe != nullptr);
    ModelEntry entry;
    entry.id = kModel;
    entry.recipe = "thenoise";
    entry.labels = {"image", "edit"};
    entry.downloaded = true;
    entry.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};

    RequestInput input;
    input.recipe = recipe;
    input.server_name = "recorded";
    input.model_id = kModel;
    input.model = &entry;
    input.width = width;
    input.height = height;
    input.prompt = "remove the object, match the wall";
    input.seed = 1234;
    input.png = region_png();
    std::string error;
    const std::optional<BuiltRequest> built =
        build_request(input, SizePolicy{16, 1024 * 1024}, error);
    REQUIRE(built.has_value());
    return *built;
}

RecipeSet recipes_or_fail() {
    std::string error;
    const std::optional<RecipeSet> set =
        load_recipes(std::string(NOISE_TEST_ASSETS_DIR) + "/recipes", error);
    REQUIRE(set.has_value());
    return *set;
}

std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("noise-backend-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void test_discovery_reads_the_recorded_server() {
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });

    std::string load_error;
    AiError error;
    const std::optional<noise::ai::ServerHealth> health = client.health(error);
    REQUIRE(health.has_value());
    CHECK(health->ok);
    CHECK(!health->version.empty());
    // Nothing resident: the state that makes `no model loaded` a possible answer.
    CHECK(health->model_loaded.empty());
    CHECK_EQ(health->max_models_image, 1);

    const std::optional<std::vector<ModelEntry>> models = client.models(error);
    REQUIRE(models.has_value());
    CHECK_EQ(models->size(), std::size_t(7));
    bool edit_capable = false;
    bool sd_cpp_kept_out = false;
    for (const ModelEntry& entry : *models) {
        if (entry.id == kModel) {
            CHECK(entry.downloaded);
            CHECK(entry.has_label("edit"));
            CHECK_NEAR(entry.image_defaults.steps, 4.0, 1e-12);
            edit_capable = true;
        }
        if (entry.id == std::string("SD-Turbo")) {
            CHECK_EQ(entry.recipe, std::string("sd-cpp"));
            sd_cpp_kept_out = true;
        }
    }
    CHECK(edit_capable);
    CHECK(sd_cpp_kept_out);

    // The request that went out carries the key as a bearer header and nothing else: no admin key, no
    // query parameter.
    const std::vector<noise::ai::CapturedRequest> sent = transport->sent();
    REQUIRE(sent.size() >= std::size_t(2));
    CHECK_EQ(sent[0].method, std::string("GET"));
    CHECK(sent[0].url.find("api_key=") == std::string::npos);
}

// What leaves the app, on the wire: the same field list the preview promised, the region as a file part,
// and the answer decoded from the bytes the server actually returns.
void test_an_answer_comes_back_from_the_recorded_edit() {
    const RecipeSet recipes = recipes_or_fail();
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });

    const BuiltRequest request = edits_request(recipes);
    AiError error;
    const std::optional<Client::Answer> answer = client.run(request, 60, cancelled, error);
    if (!answer.has_value()) {
        std::fprintf(stderr, "  run: %s | %s\n", error.message.c_str(), error.detail.c_str());
    }
    REQUIRE(answer.has_value());
    CHECK_EQ(answer->image.size().w, 64);
    CHECK_EQ(answer->image.size().h, 64);
    // The fixture's pixels, not a re-encode of something near them: the decode path is the one the
    // compositor will be handed.
    CHECK_EQ(answer->image.at(0, 0)[0], std::uint8_t(12));
    CHECK_EQ(answer->image.at(0, 0)[1], std::uint8_t(200));
    CHECK_EQ(answer->image.at(0, 0)[3], std::uint8_t(255));

    const std::vector<noise::ai::CapturedRequest> sent = transport->sent();
    REQUIRE(!sent.empty());
    const noise::ai::CapturedRequest& wire = sent.back();
    CHECK_EQ(wire.method, std::string("POST"));
    CHECK(wire.path.find("/images/edits") != std::string::npos);
    CHECK_EQ(wire.field("model"), kModel);
    CHECK_EQ(wire.field("size"), std::string("64x64"));
    CHECK_EQ(wire.field("seed"), std::string("1234"));
    CHECK_EQ(wire.field("response_format"), std::string("b64_json"));
    CHECK(wire.image_bytes > std::size_t(0));
    // The bytes that left are the bytes that were built, by hash: a transport that re-encoded or
    // truncated the region would change the cache key's meaning.
    CHECK_EQ(wire.image_sha, noise::sha256_hex(std::span<const std::uint8_t>(request.png)));
    // No mask field, ever: the engine has none, and precision is region plus prep.
    CHECK(string_of(bytes_of(wire.body_text)).find("mask") == std::string::npos);
    CHECK(transport->misses().empty());
}

// Every failure the box is known to give has its own class and its own instruction, because "request
// failed" cannot be acted on.
void test_each_failure_the_server_gives_is_a_different_answer() {
    struct Case {
        const char* model;
        FailureClass kind;
        const char* hint_needle;
    };
    const Case cases[] = {
        {"NoSuchModel-TheNoise", FailureClass::model_not_found, "model"},
        {"unauthorized", FailureClass::auth, "key"},
        {"not-resident", FailureClass::no_model_loaded, "load"},
        {"bad-size", FailureClass::size_rejected, "size"},
        {"needs-prompt", FailureClass::refusal, "prompt"},
        {"proxy", FailureClass::bad_image, "proxy"},
        {"empty-reply", FailureClass::bad_image, "nothing to composite"},
        {"timed-out", FailureClass::timeout, "reachable"},
        {"cancelled-mid-transfer", FailureClass::cancelled, "cancelled"},
    };
    const RecipeSet recipes = recipes_or_fail();
    for (const Case& testCase : cases) {
        std::atomic<bool> cancelled{false};
        const std::unique_ptr<RecordedTransport> transport = fixtures();
        Client client(*transport, profile(), [] { return 0.0; });

        ModelEntry entry;
        entry.id = testCase.model;
        entry.recipe = "thenoise";
        entry.labels = {"image", "edit"};
        entry.downloaded = true;
        entry.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};
        const noise::ai::Recipe* recipe = recipes.find("inpaint-remove");
        REQUIRE(recipe != nullptr);
        RequestInput input;
        input.recipe = recipe;
        input.server_name = "recorded";
        input.model_id = testCase.model;
        input.model = &entry;
        input.width = 64;
        input.height = 64;
        input.prompt = "do the thing";
        input.seed = 3;
        input.png = region_png();
        std::string build_error;
        const std::optional<BuiltRequest> built =
            build_request(input, SizePolicy{16, 1024 * 1024}, build_error);
        REQUIRE(built.has_value());

        AiError error;
        const std::optional<Client::Answer> answer = client.run(*built, 60, cancelled, error);
        CHECK(!answer.has_value());
        if (answer.has_value()) {
            continue;
        }
        if (error.kind != testCase.kind) {
            std::fprintf(stderr, "  %s -> %s (%s)\n", testCase.model, failure_name(error.kind),
                         error.message.c_str());
        }
        CHECK(error.kind == testCase.kind);
        CHECK(!error.hint.empty());
        CHECK(!error.message.empty());
        // The detail survives so the job record can show what the server really said.
        if (std::string(testCase.model) == "NoSuchModel-TheNoise") {
            CHECK(error.detail.find("model_not_found") != std::string::npos);
        }
    }
}

void test_cancellation_and_unmatched_requests_are_honest() {
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });

    ModelEntry entry;
    entry.id = "cancelled-mid-transfer";
    entry.recipe = "thenoise";
    entry.labels = {"image", "edit"};
    entry.downloaded = true;
    entry.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};
    Client cancel_client(*transport, profile(), [] { return 0.0; });

    const RecipeSet recipes = recipes_or_fail();
    const noise::ai::Recipe* recipe = recipes.find("inpaint-remove");
    REQUIRE(recipe != nullptr);
    RequestInput input;
    input.recipe = recipe;
    input.server_name = "recorded";
    input.model_id = "cancelled-mid-transfer";
    input.model = &entry;
    input.width = 64;
    input.height = 64;
    input.prompt = "do the thing";
    input.png = region_png();
    std::string build_error;
    const std::optional<BuiltRequest> built =
        build_request(input, SizePolicy{16, 1024 * 1024}, build_error);
    REQUIRE(built.has_value());

    AiError error;
    CHECK(!cancel_client.run(*built, 60, cancelled, error).has_value());
    CHECK(error.kind == FailureClass::cancelled);

    // Nothing recorded for this request: a refusal that names what was asked, never an answer from a
    // recording that merely looked similar.
    std::string empty_error;
    const std::unique_ptr<RecordedTransport> empty =
        RecordedTransport::from_json(std::vector<json>{}, "empty", empty_error);
    REQUIRE(empty_error.empty());
    Client strict(*empty, profile(), [] { return 0.0; });
    AiError miss_error;
    CHECK(!strict.run(edits_request(recipes), 60, cancelled, miss_error).has_value());
    CHECK(miss_error.kind == FailureClass::server_down);
    const std::vector<std::string> misses = empty->misses();
    CHECK_EQ(misses.size(), std::size_t(1));
    CHECK(misses[0].find("/images/edits") != std::string::npos);
}

void test_the_blob_cache_holds_answers_by_content() {
    const std::filesystem::path dir = scratch("cache");
    BlobCache cache(dir.string());
    const std::string key = noise::sha256_hex(std::string("an answer"));
    std::string error;

    std::vector<std::uint8_t> read_back;
    CHECK(!cache.lookup(key, read_back, error));
    CHECK_EQ(cache.stats().misses, std::size_t(1));

    const std::vector<std::uint8_t> png = region_png();
    REQUIRE(cache.store(key, png, error));
    REQUIRE(cache.lookup(key, read_back, error));
    CHECK(read_back == png);
    CHECK_EQ(cache.stats().hits, std::size_t(1));
    CHECK_EQ(cache.stats().entries, std::size_t(1));
    CHECK(cache.stats().bytes == png.size());

    // A key arrives from a hash, but the API takes a string: anything that could walk out of the cache
    // directory is refused rather than joined.
    // Strings, not `const char*`: the 64-'g' case has no literal, and a `.c_str()` from a temporary in
    // an initializer list is a pointer into a destroyed buffer by the time the loop body reads it.
    const std::vector<std::string> bad = {
        "", "../../etc/passwd", "short",
        "UPPERCASE000000000000000000000000000000000000000000000000000000", std::string(64, 'g')};
    for (const std::string& bad_key : bad) {
        std::string ignored;
        CHECK(cache.blob_path(bad_key).empty());
        CHECK(!cache.lookup(bad_key, read_back, ignored));
        CHECK(!cache.store(bad_key, png, ignored));
    }
    // Refusals are counted and nothing is written: a cache that answered a traversal key by writing next
    // to it would be a bug the counters are the only way to see.
    CHECK(cache.stats().refused_keys >= std::size_t(5));
    CHECK_EQ(cache.stats().entries, std::size_t(1));
    std::size_t files = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    CHECK_EQ(files, std::size_t(1));
    CHECK(cache.json().contains("entries"));
    std::filesystem::remove_all(dir);
}

// A queue with one worker is a queue, and a job that takes a minute has to be stoppable. The slow
// recording is what makes these states exist without a wall clock being asserted: the transfer holds,
// the second job waits behind it, and the cancellation is what the queue does about it.
void test_a_queued_and_a_running_job_are_both_stoppable() {
    const std::filesystem::path dir = scratch("cancel");
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });
    BlobCache cache((dir / "cache").string());
    // This is the one test where elapsed time is part of what is under test - a job that is cancelled
    // mid-transfer, another that waits behind it - so the injected clock is a real monotonic one. The
    // assertions are still about states and bytes, never about how long anything took.
    JobQueue queue(
        JobQueue::Deps{&client, &cache, (dir / "jobs").string()},
        [] {
            return static_cast<double>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count()) /
                   1000.0;
        });
    const RecipeSet recipes = recipes_or_fail();

    const BuiltRequest slow = edits_request_for(recipes, "slow-motion");
    const BuiltRequest fast = edits_request(recipes);
    Landing landing;
    landing.crop = noise::Rect{0, 0, 64, 64};
    landing.requested = Extent{64, 64};
    landing.canvas = Extent{256, 256};

    const std::string running_id =
        queue.enqueue(slow, "inpaint-remove", "edit", landing, coverage_png());
    // Wait for the worker to have taken it: "queued" and "running" are different things to cancel, and
    // the difference is the point. The slow recording holds 700ms, so this is a few milliseconds.
    bool started = false;
    for (int tries = 0; tries < 400 && !started; ++tries) {
        queue.pump();
        const std::optional<noise::ai::JobRecord> record = queue.find(running_id);
        started = record.has_value() && record->state == JobState::running;
        if (!started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(started);

    const std::string waiting_id =
        queue.enqueue(fast, "inpaint-remove", "edit", landing, coverage_png());
    CHECK(queue.find(waiting_id)->state == JobState::queued);

    std::string error;
    CHECK(queue.cancel(waiting_id, error));
    CHECK(queue.find(waiting_id)->state == JobState::cancelled);
    CHECK_EQ(queue.find(waiting_id)->error, std::string("cancelled before it started"));
    // A cancelled queue entry never reaches the server, which is the only reason cancelling a mistake is
    // free rather than a waste of the box.
    const std::optional<noise::ai::JobRecord> never = queue.find(waiting_id);
    REQUIRE(never.has_value());
    CHECK(never->result_png.empty());

    // The running one is stopped in flight: the transport sees the flag before its delay is up, so the
    // class is `cancelled` and not `timeout`.
    CHECK(queue.cancel(running_id, error));
    REQUIRE(queue.wait_for(running_id, 20.0));
    const std::optional<noise::ai::JobRecord> stopped = queue.find(running_id);
    REQUIRE(stopped.has_value());
    CHECK(stopped->state == JobState::cancelled);
    CHECK_EQ(stopped->failure, std::string("cancelled"));

    // An answer that exists is not taken back: cancelling a landed job says where the answer is instead
    // of pretending to unmake it.
    const std::string done_id =
        queue.enqueue(fast, "inpaint-remove", "edit", landing, coverage_png());
    REQUIRE(queue.wait_for(done_id, 20.0));
    CHECK(queue.find(done_id)->state == JobState::succeeded);
    CHECK(!queue.cancel(done_id, error));
    CHECK(error.find("already succeeded") != std::string::npos);
    CHECK(!queue.find(done_id)->result_png.empty());

    // A cancelled job is the one thing a retry is for.
    const std::optional<std::string> retried = queue.retry(waiting_id, error);
    REQUIRE(retried.has_value());
    REQUIRE(queue.wait_for(*retried, 20.0));
    CHECK(queue.find(*retried)->state == JobState::succeeded);

    // What the box actually saw: one slow transfer and two fast ones. The entry cancelled while queued
    // cost nothing, which is the whole reason a queue and an immediate request are different things.
    // What the box saw: one slow transfer, and one fast answer. The entry cancelled while queued cost
    // nothing, and the retry of it needed no new request because the answer to those bytes already
    // existed - which is the queue and the cache doing their separate jobs.
    int slow_sends = 0;
    int fast_sends = 0;
    for (const auto& [name, count] : transport->hits()) {
        slow_sends += name.find("700ms") != std::string::npos ? count : 0;
        fast_sends += name.find("an answer for the edit role") != std::string::npos ? count : 0;
    }
    CHECK_EQ(slow_sends, 1);
    CHECK_EQ(fast_sends, 1);
    CHECK(queue.find(*retried)->from_cache);
    std::filesystem::remove_all(dir);
}

// The queue's whole promise in one run: a job goes in, an answer comes out and lands as a PNG on disk,
// the second identical request never reaches the server, and the record says all of it.
void test_a_job_runs_once_and_the_second_time_is_a_cache_hit() {
    const std::filesystem::path dir = scratch("queue");
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });
    BlobCache cache((dir / "cache").string());

    // A clock that advances a step per read: the worker's reads and this thread's are concurrent, so a
    // shared `now` would be a data race, and a frozen one would make every elapsed exactly zero and let
    // a job that never started look instantaneous.
    // A clock that advances a step per read, shared through a pointer so the copies `std::function` makes
    // all count into the same counter: the worker reads it concurrently with this thread, so a plain
    // `double now` captured by reference would be a data race, and a frozen one would make every elapsed
    // exactly zero - a job that never started would look instantaneous.
    const auto reads = std::make_shared<std::atomic<int>>(0);
    const auto clock = [reads] { return 100.0 + 5.0 * static_cast<double>(reads->fetch_add(1)); };
    JobQueue queue(JobQueue::Deps{&client, &cache, (dir / "jobs").string()}, clock);
    const RecipeSet recipes = recipes_or_fail();
    const BuiltRequest request = edits_request(recipes);

    Landing landing;
    landing.crop = noise::Rect{8, 8, 64, 64};
    landing.requested = Extent{64, 64};
    landing.canvas = Extent{256, 256};
    landing.extent = "layer";
    landing.target_layer = 0;
    landing.document_checksum_before = "before";

    const std::string id =
        queue.enqueue(request, "inpaint-remove", "edit", landing, coverage_png());
    REQUIRE(queue.wait_for(id, 20.0));
    const std::optional<noise::ai::JobRecord> record = queue.find(id);
    REQUIRE(record.has_value());
    CHECK(record->state == JobState::succeeded);
    CHECK_EQ(record->recipe_id, std::string("inpaint-remove"));
    CHECK_EQ(record->model_id, kModel);
    CHECK_EQ(record->attempts, 1);
    CHECK(!record->from_cache);
    // Timings come from the injected clock, so a test sees the numbers it fed and no wall clock at all.
    CHECK(record->finished_at > record->started_at);
    CHECK(record->started_at >= record->submitted_at);

    std::vector<std::uint8_t> png;
    std::string error;
    REQUIRE(queue.result_png(id, png, error));
    // The answer on disk is an image, and the blob key is the hash of exactly those bytes: the cache and
    // the record have to agree or a re-run re-asks for something it already has.
    const Image answer = decode_png(png, error);
    REQUIRE(!error.empty() == false);
    CHECK_EQ(answer.size().w, 64);
    CHECK_EQ(answer.size().h, 64);
    CHECK_EQ(record->blob, noise::sha256_hex(std::span<const std::uint8_t>(png)));

    // Landing metadata survived the job: where the answer belongs and under which mask.
    CHECK_EQ(record->landing.extent, std::string("layer"));
    CHECK(record->landing.crop == (noise::Rect{8, 8, 64, 64}));
    CHECK(!record->landing.coverage_png_path.empty());
    CHECK(std::filesystem::exists(record->landing.coverage_png_path));
    CHECK(std::filesystem::exists(record->result_png));
    CHECK(std::filesystem::exists(record->request_png));

    const std::size_t hits_before = cache.stats().hits;
    const std::size_t sent_before = transport->sent().size();
    const std::string again =
        queue.enqueue(request, "inpaint-remove", "edit", landing, std::vector<std::uint8_t>{});
    REQUIRE(queue.wait_for(again, 20.0));
    const std::optional<noise::ai::JobRecord> second = queue.find(again);
    REQUIRE(second.has_value());
    CHECK(second->state == JobState::succeeded);
    CHECK(second->from_cache);
    CHECK_EQ(second->blob, record->blob);
    CHECK_EQ(cache.stats().hits, hits_before + std::size_t(1));
    CHECK_EQ(transport->sent().size(), sent_before);

    // A retry of a succeeded job is a new job, not a mutation: the first answer stays what it was.
    std::string retry_error;
    const std::optional<std::string> retried = queue.retry(id, retry_error);
    CHECK(!retried.has_value());
    CHECK(!retry_error.empty());

    const json dump = queue.json();
    CHECK(dump.contains("jobs"));
    CHECK_EQ(static_cast<std::size_t>(dump["jobs"].size()), std::size_t(2));

    // Durability: the records and their answers survive the queue, so a job that landed yesterday can
    // still be applied today.
    JobQueue reopened(JobQueue::Deps{&client, &cache, (dir / "jobs").string()}, clock);
    const std::vector<noise::ai::JobRecord> restored = reopened.all();
    CHECK_EQ(restored.size(), std::size_t(2));
    const std::optional<noise::ai::JobRecord> first = reopened.find(id);
    REQUIRE(first.has_value());
    CHECK(first->state == JobState::succeeded);
    std::vector<std::uint8_t> restored_png;
    REQUIRE(reopened.result_png(id, restored_png, error));
    CHECK(restored_png == png);
    std::filesystem::remove_all(dir);
}

// A job that was in flight when the app stopped cannot be resumed - the request died with the process -
// so the record has to say so instead of hanging in `running` forever.
void test_a_job_left_running_is_reported_interrupted() {
    const std::filesystem::path dir = scratch("interrupted");
    const std::filesystem::path jobs = dir / "jobs";
    std::filesystem::create_directories(jobs);
    const json record = json{{"id", "j1"},
                             {"recipe", "inpaint-remove"},
                             {"role", "edit"},
                             {"server", "recorded"},
                             {"model", kModel},
                             {"state", "running"},
                             {"cache_key", noise::sha256_hex(std::string("x"))},
                             {"attempts", 1},
                             {"submitted_at", 10.0},
                             {"started_at", 11.0},
                             {"landing",
                              {{"crop", {8, 8, 64, 64}},
                               {"requested", {64, 64}},
                               {"canvas", {256, 256}},
                               {"extent", "layer"}}}};
    std::string error;
    REQUIRE(write_file_atomic((jobs / "jobs.json").string(),
                              bytes_of(json{{"jobs", json::array({record})}}.dump()), error));

    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });
    BlobCache cache((dir / "cache").string());
    double now = 40.0;
    JobQueue queue(JobQueue::Deps{&client, &cache, jobs.string()}, [&now] { return now; });

    const std::optional<noise::ai::JobRecord> job = queue.find("j1");
    REQUIRE(job.has_value());
    CHECK(job->state == JobState::failed);
    CHECK_EQ(job->failure, std::string("interrupted"));
    CHECK(!job->hint.empty());
    // The rewrite is on disk, so the truth is told the next time too.
    JobQueue again(JobQueue::Deps{&client, &cache, jobs.string()}, [&now] { return now; });
    CHECK(again.find("j1")->state == JobState::failed);
    CHECK_EQ(queue.latency().entries().size(), std::size_t(0));
    std::filesystem::remove_all(dir);
}

// The ETA is a promise about waiting, so it is learned only from real runs of this shape on this box and
// never guessed at.
void test_the_latency_model_learns_only_from_real_runs() {
    LatencyModel latency;
    const Extent size{1024, 1024};
    CHECK(!latency.predict(kModel, size, 4).has_value());
    CHECK_EQ(latency.request_timeout_s(kModel, size, 4, 60), 60);

    // A reply inside the server's own cache ceiling is the cache talking, not the model: learning from
    // it would promise a five-second wait for a job that takes five minutes.
    latency.observe(kModel, size, 4, 0.05);
    CHECK(!latency.predict(kModel, size, 4).has_value());
    CHECK_EQ(latency.entries()[0].cache_hits, 1);

    latency.observe(kModel, size, 4, 60.0);
    REQUIRE(latency.predict(kModel, size, 4).has_value());
    CHECK_NEAR(*latency.predict(kModel, size, 4), 60.0, 1e-9);
    CHECK(latency.request_timeout_s(kModel, size, 4, 60) >= 60);

    // Another shape on the same model is another question: 512x512 at 4 steps does not inherit the
    // 1024x1024 timing.
    CHECK(!latency.predict(kModel, Extent{512, 512}, 4).has_value());
    CHECK(!latency.predict(kUpscaler, size, 4).has_value());

    latency.observe(kModel, size, 4, 120.0);
    const double mixed = *latency.predict(kModel, size, 4);
    CHECK(mixed > 60.0);
    CHECK(mixed < 120.0);
    CHECK_EQ(latency.entries()[0].samples, 2);
}

}  // namespace

// The queue's worker writes the latency model and the blob cache while the thread that draws reads both
// every frame the AI panel is up. The queue's own mutex does not cover them, and a `std::map` walked
// while another thread rebalances it does not fail - it hangs inside `find`, which is how the window
// stopped when a job finished behind the panel. The invariant lives in the two types now, so this asks
// for it from outside: interleaved, both answers stay exact.
void test_the_model_and_the_cache_survive_the_thread_that_reads_them() {
    LatencyModel latency;
    std::atomic<bool> done{false};
    constexpr int kShapes = 48;
    constexpr double kSeconds = 4.0;

    // The reader must see a value that a writer could have produced for the shape it asked about, and
    // never one borrowed from another shape - the failure mode of a torn lookup is a wrong number, not
    // a crash.
    std::atomic<int> wrong{0};
    std::thread reader([&] {
        while (!done.load()) {
            for (int shape = 0; shape < kShapes; ++shape) {
                const std::optional<double> predicted =
                    latency.predict("model", Extent{64 + 16 * shape, 64}, 4);
                if (predicted.has_value() && *predicted != kSeconds) {
                    ++wrong;
                }
            }
        }
    });

    // Two writers on distinct shapes, so a lost update shows as a missing entry and a torn one as a
    // wrong value. Every observation is the same number, which makes the exact final state checkable.
    std::vector<std::thread> writers;
    for (int half = 0; half < 2; ++half) {
        writers.emplace_back([&, half] {
            for (int shape = half * (kShapes / 2); shape < (half + 1) * (kShapes / 2); ++shape) {
                latency.observe("model", Extent{64 + 16 * shape, 64}, 4, kSeconds);
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    done.store(true);
    reader.join();

    CHECK_EQ(wrong.load(), 0);
    const std::vector<LatencyModel::Entry> entries = latency.entries();
    CHECK_EQ(entries.size(), static_cast<std::size_t>(kShapes));
    for (const LatencyModel::Entry& entry : entries) {
        CHECK_EQ(entry.seconds, kSeconds);
        CHECK_EQ(entry.samples, 1);
    }

    // Same shape of access on the cache: the worker stores answers while `cache.stats` is read.
    const std::string directory =
        (std::filesystem::temp_directory_path() / "noise-backend-cache-race")
            .string();
    std::filesystem::remove_all(directory);
    BlobCache cache(directory);
    const Image tile = Image::filled(Extent{4, 4}, 9, 9, 9, 255);
    const std::vector<std::uint8_t> png = noise::io::encode_png(tile);
    std::atomic<bool> counting{false};
    std::atomic<std::size_t> max_entries{0};
    std::thread stats_reader([&] {
        while (!counting.load()) {
            max_entries.store(std::max(max_entries.load(), cache.stats().entries));
        }
    });
    constexpr int kBlobs = 32;
    for (int i = 0; i < kBlobs; ++i) {
        std::string error;
        CHECK(cache.store(noise::sha256_hex(std::span<const std::uint8_t>(
                              std::vector<std::uint8_t>{static_cast<std::uint8_t>(i)})),
                          png, error));
    }
    counting.store(true);
    stats_reader.join();
    const BlobCache::Stats stats = cache.stats();
    CHECK_EQ(stats.entries, static_cast<std::size_t>(kBlobs));
    CHECK(stats.bytes >= png.size() * kBlobs);
    std::filesystem::remove_all(directory);
}

int main() {
    test_discovery_reads_the_recorded_server();
    test_an_answer_comes_back_from_the_recorded_edit();
    test_each_failure_the_server_gives_is_a_different_answer();
    test_cancellation_and_unmatched_requests_are_honest();
    test_the_blob_cache_holds_answers_by_content();
    test_a_job_runs_once_and_the_second_time_is_a_cache_hit();
    test_a_queued_and_a_running_job_are_both_stoppable();
    test_a_job_left_running_is_reported_interrupted();
    test_the_latency_model_learns_only_from_real_runs();
    test_the_model_and_the_cache_survive_the_thread_that_reads_them();
    return noise::test::summary(__FILE__);
}
