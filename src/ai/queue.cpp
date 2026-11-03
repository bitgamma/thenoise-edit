// SPDX-License-Identifier: Apache-2.0
#include "ai/queue.h"
#include "platform/paths.h"

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
// what the model was asked, so the two cannot disagree about what was asked.
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
                                {"mask_bounds", rect_json(landing.mask_bounds)},
                                {"requested", extent_json(landing.requested)},
                                {"resampled", landing.resampled},
                                {"canvas", extent_json(landing.canvas)},
                                {"upscale_target", extent_json(landing.upscale_target)},
                                {"generative_upscale", landing.generative_upscale},
                                {"coverage_png", landing.coverage_png_path},
                                {"colour_match", landing.colour_match},
                                {"lock_band_px", landing.lock_band_px},
                                {"target_layer", landing.target_layer},
                                {"document_checksum_before", landing.document_checksum_before},
                                {"document_epoch", landing.document_epoch},
                                {"landed", landing.landed},
                                {"landed_layer", landing.landed_layer},
                                {"report", landing.report},
                                {"land_refused", landing.land_refused},
                                {"land_error", landing.land_error}};
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
                       {"blob", blob},
                       {"result", result_png},
                       {"result_dims", extent_json(result_dims)},
                       {"request_png", request_png},
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
    std::filesystem::create_directories(platform::path_from_utf8(directory_), ec);
#ifndef __EMSCRIPTEN__
    worker_ = std::thread([this] { worker(); });
#endif
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
#ifndef __EMSCRIPTEN__
    if (worker_.joinable()) {
        worker_.join();
    }
#endif
}

std::string JobQueue::new_id() { return pad_id(next_id_++); }

int JobQueue::predicted_timeout_s(const std::string& model, Extent size, int steps) const {
    const int floor_seconds = deps_.client != nullptr ? deps_.client->profile().request_timeout_s
                                                      : kRequestTimeoutFloorS;
    return latency_.request_timeout_s(model, size, steps, floor_seconds);
}

std::string JobQueue::enqueue(BuiltRequest request, const std::string& recipe_id,
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
        job.request = std::move(request);
        job.cancelled = std::make_shared<std::atomic<bool>>(false);
        job.landing = stored;
        // Predicted here, on the thread that queues: the worker must not read the latency model while
        // this one is writing it, and the number the ETA shows is the number the request gets.
        job.timeout_s = predicted_timeout_s(job.request.params.model_id,
                                            job.request.params.width > 0
                                                ? Extent{job.request.params.width,
                                                         job.request.params.height}
                                                                      : Extent{},
                                            job.request.params.steps);
        flags_[id] = job.cancelled;
        queue_.push_back(std::move(job));

        JobRecord& record = records_[id];
        record.id = id;
        record.recipe_id = recipe_id;
        record.role = role;
        record.server_name = queue_.back().request.params.server_name;
        record.model_id = queue_.back().request.params.model_id;
        record.state = JobState::queued;
        record.request = outgoing_preview(deps_.client != nullptr ? deps_.client->profile()
                                                                  : ServerProfile{},
                                          queue_.back().request);
        record.submitted_at = clock_();
        record.attempts = 0;
        record.landing = stored;
        if (!queue_.back().request.images.empty()) {
            const std::string path = at(directory_, id + "-request.png");
            std::string error;
            if (io::write_file_atomic(path, queue_.back().request.images.front().png, error)) {
                record.request_png = path;
            } else {
                log::error("queue: " + error);
            }
        }
        order_.push_back(id);
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
        }

        Completion completion;
        completion.record.id = job.id;

        if (deps_.client == nullptr) {
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
        record.result_dims = completion.record.result_dims;
        if (record.result_dims.w <= 0) {
            // The transport does not always report dims, and `answered_as_requested` at landing time
            // compares them: reading them off the bytes keeps that comparison real.
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
        // Keyed on what was *asked*: the box's time for a 512x512 four-step edit is the fact the
        // next such job needs, whatever dims the engine answered with.
        latency_.observe(record.model_id, record.landing.requested, steps_of(record),
                         completion.elapsed_s);
        log::info("job " + record.id + " " + record.model_id + " " +
                  std::to_string(record.result_dims.w) + "x" +
                  std::to_string(record.result_dims.h) + " steps=" +
                  std::to_string(steps_of(record)) + " in " +
                  std::to_string(completion.elapsed_s) + "s");
    }
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
        return true;
    }
    const auto flag = flags_.find(id);
    if (flag != flags_.end()) {
        flag->second->store(true);
    }
    // In flight: the transport polls the flag and the next pump records the cancellation.
    return true;
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

std::vector<JobRecord> JobQueue::pending_landings(std::uint64_t document_epoch) const {
    std::vector<JobRecord> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& id : order_) {
        const auto found = records_.find(id);
        if (found == records_.end()) {
            continue;
        }
        const JobRecord& record = found->second;
        if (record.state == JobState::succeeded && !record.landing.landed &&
            !record.landing.land_refused && record.landing.document_epoch == document_epoch) {
            out.push_back(record);
        }
    }
    return out;
}

void JobQueue::set_landed(const std::string& id, const std::string& layer_name,
                          const nlohmann::json& report) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return;
    }
    found->second.landing.landed = true;
    found->second.landing.land_refused = false;
    found->second.landing.land_error.clear();
    found->second.landing.landed_layer = layer_name;
    found->second.landing.report = report;
}

void JobQueue::set_land_refusal(const std::string& id, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return;
    }
    found->second.landing.land_refused = true;
    found->second.landing.land_error = error;
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



}  // namespace noise::ai
