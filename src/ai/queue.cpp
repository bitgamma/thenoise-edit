// SPDX-License-Identifier: Apache-2.0
#include "ai/queue.h"

#include "core/hash.h"

#include "ai/recipes.h"
#include "core/log.h"
#include "io/file.h"
#include "io/png.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace noise::ai {

namespace {

constexpr const char* kJobFile = "jobs.json";

std::string at(const std::string& directory, const std::string& name) {
    return directory + "/" + name;
}

std::string pad_id(std::size_t number) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "j%04zu", number);
    return buffer;
}

nlohmann::json rect_json(const Rect& rect) {
    return nlohmann::json::array({rect.x, rect.y, rect.w, rect.h});
}

Rect rect_of(const nlohmann::json& value) {
    Rect rect;
    if (value.is_array() && value.size() == 4) {
        rect.x = value[0].get<int>();
        rect.y = value[1].get<int>();
        rect.w = value[2].get<int>();
        rect.h = value[3].get<int>();
    }
    return rect;
}

nlohmann::json extent_json(const Extent& extent) {
    return nlohmann::json::array({extent.w, extent.h});
}

Extent extent_of(const nlohmann::json& value) {
    Extent extent;
    if (value.is_array() && value.size() == 2) {
        extent.w = value[0].get<int>();
        extent.h = value[1].get<int>();
    }
    return extent;
}

// The step count the request carried, for the latency key. It is in the stored preview, which is also
// what a retry re-sends, so the two cannot disagree about what was asked.
int steps_of(const JobRecord& record) {
    if (record.request.is_object() && record.request.contains("steps") &&
        record.request.at("steps").is_number_integer()) {
        return static_cast<int>(record.request.at("steps").get<long long>());
    }
    return 0;
}

}  // namespace

const char* job_state_name(JobState state) {
    switch (state) {
        case JobState::queued:
            return "queued";
        case JobState::running:
            return "running";
        case JobState::succeeded:
            return "succeeded";
        case JobState::failed:
            return "failed";
        case JobState::cancelled:
            return "cancelled";
    }
    return "failed";
}

bool job_state_is_terminal(JobState state) {
    switch (state) {
        case JobState::queued:
        case JobState::running:
            return false;
        case JobState::succeeded:
        case JobState::failed:
        case JobState::cancelled:
            return true;
    }
    return true;
}

nlohmann::json JobRecord::json() const {
    nlohmann::json landing_json{{"crop", rect_json(landing.crop)},
                                {"requested", extent_json(landing.requested)},
                                {"canvas", extent_json(landing.canvas)},
                                {"coverage_png", landing.coverage_png_path},
                                {"colour_match", landing.colour_match},
                                {"lock_band_px", landing.lock_band_px},
                                {"target_layer", landing.target_layer},
                                {"document_checksum_before", landing.document_checksum_before}};
    if (landing.feather_px.has_value()) {
        landing_json["feather_px"] = *landing.feather_px;
    } else {
        landing_json["feather"] = "auto";
    }
    nlohmann::json out{{"id", id},
                       {"recipe", recipe_id},
                       {"role", role},
                       {"server", server_name},
                       {"model", model_id},
                       {"state", job_state_name(state)},
                       {"terminal", job_state_is_terminal(state)},
                       {"failure", failure},
                       {"error", error},
                       {"hint", hint},
                       {"cache_key", cache_key},
                       {"blob", blob},
                       {"result", result_png},
                       {"result_dims", extent_json(result_dims)},
                       {"request_png", request_png},
                       {"from_cache", from_cache},
                       {"attempts", attempts},
                       {"submitted_at", submitted_at},
                       {"started_at", started_at},
                       {"finished_at", finished_at},
                       {"seconds", finished_at > started_at ? finished_at - started_at : 0.0},
                       {"landing", landing_json}};
    if (!request.is_null()) {
        out["request"] = request;
    }
    return out;
}

JobQueue::JobQueue(Deps deps, Clock clock)
    : deps_(deps), clock_(std::move(clock)), directory_(deps.directory) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(directory_), ec);
    load_from_disk();
    worker_ = std::thread([this] { worker(); });
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::string JobQueue::new_id() { return pad_id(next_id_++); }

int JobQueue::predicted_timeout_s(const std::string& model, Extent size, int steps) const {
    const int floor_seconds = deps_.client != nullptr ? deps_.client->profile().request_timeout_s
                                                      : kRequestTimeoutFloorS;
    return latency_.request_timeout_s(model, size, steps, floor_seconds);
}

std::string JobQueue::enqueue(const BuiltRequest& request, const std::string& recipe_id,
                              const std::string& role, const Landing& landing,
                              std::vector<std::uint8_t> coverage_png) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = new_id();

        // The bytes the job needs go to disk before the record that names them does: a record pointing
        // at a missing blob is unrecoverable, a blob nobody names is only wasted space.
        std::string write_error;
        Landing stored = landing;
        stored.coverage_png_path = at(directory_, id + "-coverage.png");
        if (!coverage_png.empty() &&
            !io::write_file_atomic(stored.coverage_png_path, coverage_png, write_error)) {
            log::error("queue: " + write_error);
        }
        InFlight job;
        job.id = id;
        job.request = request;
        job.cancelled = std::make_shared<std::atomic<bool>>(false);
        job.landing = stored;
        // Predicted here, on the thread that queues: the worker must not read the latency model while
        // this one is writing it, and the number the ETA shows is the number the request gets.
        job.timeout_s = predicted_timeout_s(request.params.model_id, request.params.width > 0
                                                                      ? Extent{request.params.width,
                                                                               request.params.height}
                                                                      : Extent{},
                                            request.params.steps);
        flags_[id] = job.cancelled;
        queue_.push_back(job);

        JobRecord& record = records_[id];
        record.id = id;
        record.recipe_id = recipe_id;
        record.role = role;
        record.server_name = request.params.server_name;
        record.model_id = request.params.model_id;
        record.state = JobState::queued;
        record.cache_key = request.cache_key();
        record.request = request.preview_json();
        record.submitted_at = clock_();
        record.attempts = 0;
        record.landing = stored;
        if (!request.png.empty()) {
            const std::string path = at(directory_, id + "-request.png");
            std::string error;
            if (io::write_file_atomic(path, request.png, error)) {
                record.request_png = path;
            } else {
                log::error("queue: " + error);
            }
        }
        order_.push_back(id);
        persist_locked();
    }
    ready_.notify_all();
    return id;
}

void JobQueue::worker() {
    while (true) {
        InFlight job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            JobRecord& record = records_[job.id];
            record.state = JobState::running;
            record.started_at = clock_();
            record.attempts += 1;
            persist_locked();
        }

        Completion completion;
        completion.record.id = job.id;

        std::string cache_error;
        std::vector<std::uint8_t> cached;
        if (deps_.cache != nullptr &&
            deps_.cache->lookup(job.request.cache_key(), cached, cache_error) && !cached.empty()) {
            completion.answered = true;
            completion.png = std::move(cached);
            completion.record.from_cache = true;
        } else if (deps_.client == nullptr) {
            completion.failure = failure_name(FailureClass::server_down);
            completion.error = "this queue has no server to ask";
            completion.hint = "connect a server first";
        } else {
            AiError error;
            const std::optional<Client::Answer> answer =
                deps_.client->run(job.request, job.timeout_s, *job.cancelled, error);
            if (answer.has_value()) {
                completion.answered = true;
                completion.png = answer->png;
                completion.record.result_dims = answer->image.size();
                completion.elapsed_s = answer->elapsed_s;
            } else {
                completion.failure = failure_name(error.kind);
                completion.error = error.message;
                completion.hint = error.hint;
            }
        }
        completion.cancelled = job.cancelled->load();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            completions_.push_back(std::move(completion));
        }
        ready_.notify_all();
    }
}

void JobQueue::pump() {
    std::deque<Completion> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(completions_);
    }
    if (drained.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (Completion& completion : drained) {
        const auto found = records_.find(completion.record.id);
        if (found == records_.end()) {
            continue;  // forgotten while the request was in flight; nothing to update
        }
        JobRecord& record = found->second;
        record.finished_at = clock_();
        if (completion.cancelled) {
            record.state = JobState::cancelled;
            record.failure = failure_name(FailureClass::cancelled);
            record.error = "cancelled in flight";
            log::info("job " + record.id + " cancelled after " +
                      std::to_string(record.finished_at - record.started_at) + "s");
            continue;
        }
        if (!completion.answered) {
            record.state = JobState::failed;
            record.failure = completion.failure;
            record.error = completion.error;
            record.hint = completion.hint;
            log::warn("job " + record.id + " failed: " + completion.failure + " - " +
                      completion.error);
            continue;
        }

        record.state = JobState::succeeded;
        record.from_cache = completion.record.from_cache;
        record.result_dims = completion.record.result_dims;
        if (record.result_dims.w <= 0) {
            // A cached blob carries no dims on the record, and `answered_as_requested` at landing time
            // compares dims: reading them back keeps that comparison real.
            std::string decode_error;
            record.result_dims = io::decode_png(completion.png, decode_error).size();
        }
        // What came back, by content. The request's own blob is already in `record.request`; this is the
        // one fact about the answer that exists nowhere else.
        record.blob = sha256_hex(std::span<const std::uint8_t>(completion.png));
        record.result_png = at(directory_, record.id + "-result.png");
        std::string write_error;
        if (!io::write_file_atomic(record.result_png, completion.png, write_error)) {
            log::error("job " + record.id + ": " + write_error);
        }
        if (!record.from_cache) {
            if (deps_.cache != nullptr) {
                std::string cache_error;
                if (!deps_.cache->store(record.cache_key, completion.png, cache_error)) {
                    log::error("job " + record.id + ": cache: " + cache_error);
                }
            }
            // Keyed on what was *asked*: the box's time for a 512x512 four-step edit is the fact the
            // next such job needs, whatever dims the engine answered with.
            latency_.observe(record.model_id, record.landing.requested, steps_of(record),
                             completion.elapsed_s);
        }
        log::info("job " + record.id + " " + record.model_id + " " +
                  std::to_string(record.result_dims.w) + "x" +
                  std::to_string(record.result_dims.h) + " steps=" +
                  std::to_string(steps_of(record)) + " " +
                  (record.from_cache ? "answered from cache"
                                     : "in " + std::to_string(completion.elapsed_s) + "s"));
    }
    persist_locked();
    ready_.notify_all();
}

bool JobQueue::cancel(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        error = "job: no job '" + id + "'";
        return false;
    }
    if (job_state_is_terminal(found->second.state)) {
        // An answer that exists is never discarded, so cancelling a landed job says so instead of
        // pretending to take it back.
        error = "job " + id + " is already " + job_state_name(found->second.state) +
                (found->second.result_png.empty()
                     ? ""
                     : "; its result stays at " + found->second.result_png);
        return false;
    }
    const auto queued = std::find_if(queue_.begin(), queue_.end(),
                                     [&id](const InFlight& job) { return job.id == id; });
    if (queued != queue_.end()) {
        queue_.erase(queued);
        found->second.state = JobState::cancelled;
        found->second.failure = failure_name(FailureClass::cancelled);
        found->second.error = "cancelled before it started";
        persist_locked();
        return true;
    }
    const auto flag = flags_.find(id);
    if (flag != flags_.end()) {
        flag->second->store(true);
    }
    // In flight: the transport polls the flag and the next pump records the cancellation.
    return true;
}

std::optional<std::string> JobQueue::retry(const std::string& id, std::string& error) {
    pump();
    BuiltRequest request;
    Landing landing;
    std::string recipe_id;
    std::string role;
    std::vector<std::uint8_t> coverage;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(id);
        if (found == records_.end()) {
            error = "job: no job '" + id + "'";
            return std::nullopt;
        }
        const JobRecord& record = found->second;
        if (record.state == JobState::succeeded) {
            error = "job " + id + " already answered - apply it with job.apply; re-sending the same "
                            "request can only be a cache hit";
            return std::nullopt;
        }
        if (!job_state_is_terminal(record.state)) {
            error = "job " + id + " is " + job_state_name(record.state) + ", not finished";
            return std::nullopt;
        }
        recipe_id = record.recipe_id;
        role = record.role;
        landing = record.landing;
        std::vector<std::uint8_t> png;
        if (!record.request_png.empty() && !io::read_file(record.request_png, png, error)) {
            return std::nullopt;
        }
        const std::optional<BuiltRequest> rebuilt =
            request_from_preview(record.request, png, error);
        if (!rebuilt.has_value()) {
            return std::nullopt;
        }
        request = *rebuilt;
    }
    std::string read_error;
    if (!landing.coverage_png_path.empty()) {
        io::read_file(landing.coverage_png_path, coverage, read_error);
    }
    // `retry` re-sends the same bytes under the same coverage, which is what makes a retry comparable to
    // the job it replaces; the id differs so both records survive.
    return enqueue(request, recipe_id, role, landing, std::move(coverage));
}

bool JobQueue::wait_for(const std::string& id, double timeout_s) {
    const double deadline = clock_() + timeout_s;
    while (true) {
        pump();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto found = records_.find(id);
            if (found == records_.end()) {
                return false;
            }
            if (job_state_is_terminal(found->second.state)) {
                return true;
            }
            ready_.wait_for(lock, std::chrono::milliseconds(5),
                            [this, &id] { return !completions_.empty() || stopping_; });
        }
        if (clock_() >= deadline) {
            return false;
        }
    }
}

std::vector<JobRecord> JobQueue::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobRecord> out;
    out.reserve(order_.size());
    for (const std::string& id : order_) {
        const auto found = records_.find(id);
        if (found != records_.end()) {
            out.push_back(found->second);
        }
    }
    return out;
}

std::optional<JobRecord> JobQueue::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool JobQueue::result_png(const std::string& id, std::vector<std::uint8_t>& png,
                          std::string& error) {
    pump();
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(id);
        if (found == records_.end()) {
            error = "job: no job '" + id + "'";
            return false;
        }
        if (found->second.state != JobState::succeeded) {
            error = "job " + id + " is " + job_state_name(found->second.state) +
                    ", so there is no answer to read";
            return false;
        }
        path = found->second.result_png;
    }
    return io::read_file(path, png, error);
}

nlohmann::json JobQueue::json() const {
    nlohmann::json jobs = nlohmann::json::array();
    for (const JobRecord& record : all()) {
        jobs.push_back(record.json());
    }
    return nlohmann::json{{"jobs", jobs},
                          {"directory", directory_},
                          {"latency", latency_.json()}};
}

void JobQueue::persist_locked() {
    nlohmann::json jobs = nlohmann::json::array();
    for (const std::string& id : order_) {
        const auto found = records_.find(id);
        if (found != records_.end()) {
            jobs.push_back(found->second.json());
        }
    }
    const std::string text = nlohmann::json{{"format", 1}, {"jobs", jobs}}.dump(1);
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    std::string error;
    if (!io::write_file_atomic(at(directory_, kJobFile), bytes, error)) {
        log::error("queue: " + error);
    }
}

void JobQueue::load_from_disk() {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!io::read_file(at(directory_, kJobFile), bytes, error)) {
        return;  // a first run has no queue, which is not an error
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(bytes);
    } catch (const nlohmann::json::parse_error& exception) {
        log::error("queue: " + at(directory_, kJobFile) + ": " + exception.what());
        return;
    }
    if (!body.is_object() || !body.contains("jobs") || !body.at("jobs").is_array()) {
        log::error("queue: " + at(directory_, kJobFile) + " has no 'jobs' array");
        return;
    }
    for (const nlohmann::json& entry : body.at("jobs")) {
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()) {
            log::warn("queue: an entry without an id in " + at(directory_, kJobFile) + " is skipped");
            continue;
        }
        const auto text = [&entry](const char* key) {
            if (entry.contains(key) && entry.at(key).is_string()) {
                return entry.at(key).get<std::string>();
            }
            return std::string();
        };
        const auto number = [&entry](const char* key) {
            return entry.contains(key) && entry.at(key).is_number() ? entry.at(key).get<double>()
                                                                   : 0.0;
        };
        JobRecord record;
        record.id = entry.at("id").get<std::string>();
        record.recipe_id = text("recipe");
        record.role = text("role");
        record.server_name = text("server");
        record.model_id = text("model");
        record.failure = text("failure");
        record.error = text("error");
        record.hint = text("hint");
        record.cache_key = text("cache_key");
        record.blob = text("blob");
        record.result_png = text("result");
        record.request_png = text("request_png");
        record.from_cache = entry.contains("from_cache") && entry.at("from_cache").is_boolean()
                                ? entry.at("from_cache").get<bool>()
                                : false;
        record.submitted_at = number("submitted_at");
        record.started_at = number("started_at");
        record.finished_at = number("finished_at");
        record.attempts = static_cast<int>(number("attempts"));
        if (entry.contains("result_dims")) {
            record.result_dims = extent_of(entry.at("result_dims"));
        }
        if (entry.contains("request") && entry.at("request").is_object()) {
            record.request = entry.at("request");
        }
        if (entry.contains("landing") && entry.at("landing").is_object()) {
            const nlohmann::json& landing = entry.at("landing");
            record.landing.crop = rect_of(landing.contains("crop") ? landing.at("crop")
                                                                  : nlohmann::json());
            record.landing.requested =
                extent_of(landing.contains("requested") ? landing.at("requested")
                                                        : nlohmann::json());
            record.landing.canvas = extent_of(landing.contains("canvas") ? landing.at("canvas")
                                                                        : nlohmann::json());
            if (landing.contains("feather_px") && landing.at("feather_px").is_number_integer()) {
                record.landing.feather_px =
                    static_cast<int>(landing.at("feather_px").get<long long>());
            }
            if (landing.contains("colour_match") && landing.at("colour_match").is_boolean()) {
                record.landing.colour_match = landing.at("colour_match").get<bool>();
            }
            if (landing.contains("lock_band_px") && landing.at("lock_band_px").is_number_integer()) {
                record.landing.lock_band_px =
                    static_cast<int>(landing.at("lock_band_px").get<long long>());
            }
            if (landing.contains("target_layer") && landing.at("target_layer").is_number_integer()) {
                record.landing.target_layer =
                    static_cast<int>(landing.at("target_layer").get<long long>());
            }
            if (landing.contains("coverage_png") && landing.at("coverage_png").is_string()) {
                record.landing.coverage_png_path = landing.at("coverage_png").get<std::string>();
            } else {
                record.landing.coverage_png_path = at(directory_, record.id + "-coverage.png");
            }
            if (landing.contains("document_checksum_before") &&
                landing.at("document_checksum_before").is_string()) {
                record.landing.document_checksum_before =
                    landing.at("document_checksum_before").get<std::string>();
            }
        }
        const std::string state = text("state");
        // A job the last session left in flight cannot be in flight now. Saying so is the point of a
        // durable record: the answer either landed before the stop or it did not.
        if (state == "succeeded") {
            record.state = JobState::succeeded;
        } else if (state == "cancelled") {
            record.state = JobState::cancelled;
        } else {
            record.state = JobState::failed;
            if (state == "queued" || state == "running") {
                record.failure = failure_name(FailureClass::interrupted);
                record.error = "the app stopped while this job was " + state;
                record.hint = "retry it; if the answer had arrived it would be on disk";
            }
        }
        records_[record.id] = record;
        order_.push_back(record.id);
        // Ids are `j%04zu`, so the digits after the letter are the sequence to continue from. Anything
        // else sorts before the first id this run would hand out, which is safe: a collision with a
        // reloaded record is impossible because those are keyed by their own text id.
        const std::size_t digits = record.id.find_first_not_of("0123456789");
        const std::size_t sequence =
            digits == std::string::npos ? static_cast<std::size_t>(std::atoll(record.id.c_str() + 1))
                                        : 0;
        next_id_ = std::max(next_id_, sequence + 1);
    }
}

}  // namespace noise::ai
