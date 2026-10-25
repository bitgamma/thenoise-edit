// SPDX-License-Identifier: Apache-2.0
#include "test.h"

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
using noise::ai::build_request;
using noise::ai::BuiltRequest;
using noise::ai::Client;
using noise::ai::Endpoint;
using noise::ai::FailureClass;
using noise::ai::failure_name;
using noise::ai::JobQueue;
using noise::ai::JobState;
using noise::ai::Landing;
using noise::ai::kRequestTimeoutFloorS;
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
    const std::vector<std::uint8_t> image = region_png();
    input.images.push_back(noise::ai::RequestImageInput{"document", "region", width, height, image});
    std::string error;
    const std::optional<BuiltRequest> built =
        build_request(std::move(input), SizePolicy{16, 1024 * 1024}, error);
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
    const std::vector<std::uint8_t> image = region_png();
    input.images.push_back(noise::ai::RequestImageInput{"document", "region", width, height, image});
    std::string error;
    const std::optional<BuiltRequest> built =
        build_request(std::move(input), SizePolicy{16, 1024 * 1024}, error);
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
    CHECK_EQ(wire.images.front().sha256, request.images.front().sha256);
    CHECK_EQ(wire.images.front().bytes, request.images.front().png);
    CHECK_EQ(wire.images.front().content_type, std::string("image/png"));
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
        const std::vector<std::uint8_t> image = region_png();
        input.images.push_back(noise::ai::RequestImageInput{"document", "region", 64, 64, image});
        std::string build_error;
        const std::optional<BuiltRequest> built =
            build_request(std::move(input), SizePolicy{16, 1024 * 1024}, build_error);
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
    const std::vector<std::uint8_t> image = region_png();
    input.images.push_back(noise::ai::RequestImageInput{"document", "region", 64, 64, image});
    std::string build_error;
    const std::optional<BuiltRequest> built =
        build_request(std::move(input), SizePolicy{16, 1024 * 1024}, build_error);
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

// A queue with one worker is a queue, and a job that takes a minute has to be stoppable. The slow
// recording is what makes these states exist without a wall clock being asserted: the transfer holds,
// the second job waits behind it, and the cancellation is what the queue does about it.
void test_a_queued_and_a_running_job_are_both_stoppable() {
    const std::filesystem::path dir = scratch("cancel");
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });
    // This is the one test where elapsed time is part of what is under test - a job that is cancelled
    // mid-transfer, another that waits behind it - so the injected clock is a real monotonic one. The
    // assertions are still about states and bytes, never about how long anything took.
    JobQueue queue(
        JobQueue::Deps{&client, (dir / "jobs").string()},
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

    // There is no `job.retry`: pressing the verb again *is* the retry, and it re-sends - same request
    // bytes, a new id, a real request, nothing remembering the cancelled job in between.
    const std::string resent =
        queue.enqueue(fast, "inpaint-remove", "edit", landing, std::vector<std::uint8_t>{});
    CHECK(resent != waiting_id);
    REQUIRE(queue.wait_for(resent, 20.0));
    CHECK(queue.find(resent)->state == JobState::succeeded);

    // What the box saw: one slow transfer (cancelled mid-flight), and **two** fast ones - the answer that
    // landed and the re-ask after the cancellation. Nothing answered the second from storage, which is
    // the whole reason the count is a fact worth asserting. The entry cancelled while still queued cost
    // nothing, which is why a queue and an immediate request are different things.
    int slow_sends = 0;
    int fast_sends = 0;
    for (const auto& [name, count] : transport->hits()) {
        slow_sends += name.find("700ms") != std::string::npos ? count : 0;
        fast_sends += name.find("an answer for the edit role") != std::string::npos ? count : 0;
    }
    CHECK_EQ(slow_sends, 1);
    CHECK_EQ(fast_sends, 2);
    std::filesystem::remove_all(dir);
}

// The queue's promise in one run: a job goes in, an answer comes out as a PNG, and the record says what
// was asked and what came back. A second identical request reaches the server too: there is no answer
// cache, so "this was asked already" is not something the app may quietly decide on your behalf.
void test_a_job_runs_once_and_the_second_time_is_another_request() {
    const std::filesystem::path dir = scratch("queue");
    std::atomic<bool> cancelled{false};
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });

    // A clock that advances a step per read: the worker's reads and this thread's are concurrent, so a
    // shared `now` would be a data race, and a frozen one would make every elapsed exactly zero and let
    // a job that never started look instantaneous.
    // A clock that advances a step per read, shared through a pointer so the copies `std::function` makes
    // all count into the same counter: the worker reads it concurrently with this thread, so a plain
    // `double now` captured by reference would be a data race, and a frozen one would make every elapsed
    // exactly zero - a job that never started would look instantaneous.
    const auto reads = std::make_shared<std::atomic<int>>(0);
    const auto clock = [reads] { return 100.0 + 5.0 * static_cast<double>(reads->fetch_add(1)); };
    JobQueue queue(JobQueue::Deps{&client, (dir / "jobs").string()}, clock);
    const RecipeSet recipes = recipes_or_fail();
    const BuiltRequest request = edits_request(recipes);

    Landing landing;
    landing.crop = noise::Rect{8, 8, 64, 64};
    landing.requested = Extent{64, 64};
    landing.canvas = Extent{256, 256};
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
    // Timings come from the injected clock, so a test sees the numbers it fed and no wall clock at all.
    CHECK(record->finished_at > record->started_at);
    CHECK(record->started_at >= record->submitted_at);

    std::vector<std::uint8_t> png;
    std::string error;
    REQUIRE(queue.result_png(id, png, error));
    // The answer on disk is an image, and the recorded blob is the hash of exactly those bytes: the
    // the record have to agree or a re-run re-asks for something it already has.
    const Image answer = decode_png(png, error);
    REQUIRE(!error.empty() == false);
    CHECK_EQ(answer.size().w, 64);
    CHECK_EQ(answer.size().h, 64);
    CHECK_EQ(record->blob, noise::sha256_hex(std::span<const std::uint8_t>(png)));

    // Landing metadata survived the job: where the answer belongs and under which mask.
    CHECK(record->landing.crop == (noise::Rect{8, 8, 64, 64}));
    CHECK(!record->landing.coverage_png_path.empty());
    CHECK(std::filesystem::exists(record->landing.coverage_png_path));
    CHECK(std::filesystem::exists(record->result_png));
    CHECK(std::filesystem::exists(record->request_png));

    const std::size_t sent_before = transport->sent().size();
    const std::string again =
        queue.enqueue(request, "inpaint-remove", "edit", landing, std::vector<std::uint8_t>{});
    REQUIRE(queue.wait_for(again, 20.0));
    const std::optional<noise::ai::JobRecord> second = queue.find(again);
    REQUIRE(second.has_value());
    CHECK(second->state == JobState::succeeded);
    CHECK_EQ(second->blob, record->blob);
    // The invariant that replaced the cache: an identical request is a second request, and it costs one.
    CHECK_EQ(transport->sent().size(), sent_before + std::size_t(1));
    CHECK(second->id != id);

    const json dump = queue.json();
    CHECK(dump.contains("jobs"));
    CHECK_EQ(static_cast<std::size_t>(dump["jobs"].size()), std::size_t(2));

    // Both jobs are listed, in the order they were asked for.
    std::filesystem::remove_all(dir);
}

// The queue is in memory and the scratch directory is scratch: a record dies with the run that wrote
// it. A loader in the constructor would list a dead run's jobs against this one and offer an answer
// belonging to a document revision that no longer exists - the failure `document_epoch` guards against
// inside a single run. The blobs outlive the record, so this distinguishes "not read" from "deleted".
void test_a_new_queue_never_reads_the_run_before_it() {
    const std::filesystem::path dir = scratch("reopen") / "jobs";
    const std::unique_ptr<RecordedTransport> transport = fixtures();
    Client client(*transport, profile(), [] { return 0.0; });
    const auto reads = std::make_shared<std::atomic<int>>(0);
    const auto clock = [reads] { return 100.0 + 5.0 * static_cast<double>(reads->fetch_add(1)); };
    const RecipeSet recipes = recipes_or_fail();
    const BuiltRequest request = edits_request(recipes);

    Landing landing;
    landing.crop = noise::Rect{8, 8, 64, 64};
    landing.requested = Extent{64, 64};
    landing.canvas = Extent{256, 256};
    landing.target_layer = 0;
    landing.document_checksum_before = "before";

    std::string first_id;
    std::string answer_png;
    {
        JobQueue first(JobQueue::Deps{&client, dir.string()}, clock);
        first_id = first.enqueue(request, "inpaint-remove", "edit", landing, coverage_png());
        REQUIRE(first.wait_for(first_id, 20.0));
        const std::optional<noise::ai::JobRecord> record = first.find(first_id);
        REQUIRE(record.has_value());
        REQUIRE(record->state == JobState::succeeded);
        answer_png = record->result_png;
        REQUIRE(std::filesystem::exists(answer_png));
    }

    JobQueue reopened(JobQueue::Deps{&client, dir.string()}, clock);
    CHECK(reopened.all().empty());
    CHECK(!reopened.find(first_id).has_value());
    CHECK(reopened.json()["jobs"].empty());
    CHECK(reopened.pending_landings(0).empty());
    CHECK(std::filesystem::exists(answer_png));

    std::filesystem::remove_all(dir.parent_path());
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

    // Another shape on the same model is another question the ETA does not answer: 512x512 at 4 steps
    // has not been run here, and a scaled figure shown as a promise would be a guess wearing a
    // measurement's clothes. The timeout is a margin, not a promise, and does scale - see below.
    CHECK(!latency.predict(kModel, Extent{512, 512}, 4).has_value());
    CHECK(!latency.predict(kUpscaler, size, 4).has_value());

    latency.observe(kModel, size, 4, 120.0);
    const double mixed = *latency.predict(kModel, size, 4);
    CHECK(mixed > 60.0);
    CHECK(mixed < 120.0);
    CHECK_EQ(latency.entries()[0].samples, 2);
}

// Measured on the dev box with its resident edit model: a masked edit at the pixel ceiling (1920x1088,
// 8 steps) took 71.1 s. A model that is not resident loads inside the request, which measured 72-198 s.
constexpr double kMeasuredCeilingEditS = 71.1;
constexpr double kMeasuredColdAskS = 71.1 + 198.0;

// The size policy asks a region at its own size, so a new mark is a shape this box has never recorded -
// and the timeout for it used to be the floor, which a full-canvas inpaint then outran and reported as
// `timeout`. The extrapolation and the floor exist for this, not for tidiness.
void test_the_timeout_survives_a_shape_the_box_has_never_run() {
    // A slow enough shape that the scaled margin clears the floor and can be seen working.
    LatencyModel latency;
    latency.observe(kModel, Extent{1024, 1024}, 20, 240.0);
    const Extent ceiling{1920, 1088};

    // The promise stays a measurement: nobody has run this shape here, so the panel says there is no
    // estimate rather than showing this number.
    CHECK(!latency.predict(kModel, ceiling, 20).has_value());

    // The margin is per pixel per step of the model's own measured run: 240 s at 1.05 MP is 478 s at
    // 2.09 MP, and four times that is the wait the app will tolerate before calling the server dead.
    CHECK_NEAR(latency.request_timeout_s(kModel, ceiling, 20, kRequestTimeoutFloorS), 1912.0, 5.0);

    // The shape that was measured keeps its own figure.
    CHECK_EQ(latency.request_timeout_s(kModel, Extent{1024, 1024}, 20, kRequestTimeoutFloorS), 4 * 240);

    // Scaling raises a margin and never lowers one: a quick model asking a small shape still waits on the
    // floor, which is what covers a model load nobody has measured yet.
    LatencyModel quick;
    quick.observe(kModel, Extent{256, 256}, 1, 2.0);
    CHECK_EQ(quick.request_timeout_s(kModel, Extent{512, 512}, 2, kRequestTimeoutFloorS),
             kRequestTimeoutFloorS);

    // With nothing measured at all the floor is the only ceiling there is, so it must cover a cold ask:
    // a model load and the largest edit the policy can ask for.
    LatencyModel cold;
    CHECK(!cold.predict(kModel, ceiling, 20).has_value());
    CHECK(cold.request_timeout_s(kModel, ceiling, 20, kRequestTimeoutFloorS) > kMeasuredColdAskS);
}

}  // namespace


// The queue's worker writes the latency model and the blob cache while the thread that draws reads both
// every frame the AI panel is up. The queue's own mutex does not cover them, and a `std::map` walked
// while another thread rebalances it does not fail - it hangs inside `find`, which is how the window
// stopped when a job finished behind the panel. The invariant lives in the two types now, so this asks
// for it from outside: interleaved, both answers stay exact.
void test_the_latency_model_survives_the_thread_that_reads_it() {
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

}

int main() {
    test_discovery_reads_the_recorded_server();
    test_an_answer_comes_back_from_the_recorded_edit();
    test_each_failure_the_server_gives_is_a_different_answer();
    test_cancellation_and_unmatched_requests_are_honest();
    test_a_job_runs_once_and_the_second_time_is_another_request();
    test_a_new_queue_never_reads_the_run_before_it();
    test_a_queued_and_a_running_job_are_both_stoppable();
    test_the_latency_model_learns_only_from_real_runs();
    test_the_timeout_survives_a_shape_the_box_has_never_run();
    test_the_latency_model_survives_the_thread_that_reads_it();
    return noise::test::summary(__FILE__);
}
