// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ai/cache.h"
#include "ai/http.h"
#include "ai/provider.h"
#include "ai/server.h"
#include "core/geom.h"

namespace noise::ai {

// Where an answer goes when it lands. The queue does not touch the document - it keeps the claim about
// where the answer belongs, and the app applies it through the same compositor door `ai.compose` uses.
struct Landing {
    // Document pixels the answer covers: the region compiler's crop.
    Rect crop{};
    // What the request asked for, which is what the answer is resampled from. `answered_as_requested`
    // at landing time compares this to what came back.
    Extent requested{};
    Extent canvas{};
    // Where the coverage the region was compiled from lives: a canvas-sized PNG with the coverage in
    // the alpha channel, written by `enqueue` beside the job. Kept because a result that lands in a
    // later session must composite under the mask that was in force when it was asked for, not under
    // whatever is selected then.
    std::string coverage_png_path;
    // "layer" | "canvas": which extent the compositor is handed. Stored as text so a job reloaded from
    // disk says the same thing it said before the restart.
    std::string extent = "layer";
    // Null is the recipe's `auto`.
    std::optional<int> feather_px;
    bool colour_match = true;
    int lock_band_px = 0;
    int target_layer = 0;
    // What the document's composite was when the job was queued, so a landing that arrives after the
    // user moved on can be seen for what it is.
    std::string document_checksum_before;
};

enum class JobState {
    queued,
    running,
    succeeded,
    failed,
    cancelled,
};

const char* job_state_name(JobState state);
// True when no further pump will change the state.
bool job_state_is_terminal(JobState state);

struct JobRecord {
    std::string id;
    std::string recipe_id;
    std::string role;
    std::string server_name;
    std::string model_id;
    JobState state = JobState::queued;
    // `failure_name()` of the classified error, empty unless failed.
    std::string failure;
    std::string error;
    std::string hint;
    std::string cache_key;
    std::string blob;
    // What was asked, in the form `ai.preview` answers with, and the form a retry reads back. Kept
    // because "what did I send" is the first question about any answer, and because a request must be
    // re-sendable after a restart without the caller holding it.
    nlohmann::json request;
    // The answer's PNG on disk, and its dimensions.
    std::string result_png;
    Extent result_dims{0, 0};
    // Where the uploaded region went, so a retry after a restart sends the same bytes.
    std::string request_png;
    bool from_cache = false;
    // Written from the injected clock, so a test sees the numbers it fed in.
    double submitted_at = 0.0;
    double started_at = 0.0;
    double finished_at = 0.0;
    int attempts = 0;
    Landing landing;

    nlohmann::json json() const;
};

// A queue of AI requests: one worker, because one server holds one image model resident at a time, and
// a durable record, because a twenty-minute refine must not be lost when the app closes.
//
// The queue owns the thread and the files; the app owns the document. Nothing here reaches for the
// document, which is what keeps the whole thing testable against a recorded transport.
class JobQueue {
public:
    struct Deps {
        Client* client = nullptr;
        BlobCache* cache = nullptr;
        // Where `jobs.json`, the results and the request blobs live.
        std::string directory;
    };

    JobQueue(Deps deps, Clock clock);
    ~JobQueue();

    // Queues a request and returns its id. The worker starts on it as soon as it is free. The timeout
    // the request will get is predicted here, at the moment of queueing, so that a job's ceiling cannot
    // change under it while it waits - the latency model itself guards its own contents.
    // `coverage_png` is the canvas-sized PNG the answer lands under; the queue stores it and records
    // where it put it.
    std::string enqueue(const BuiltRequest& request, const std::string& recipe_id,
                        const std::string& role, const Landing& landing,
                        std::vector<std::uint8_t> coverage_png);

    // Reaps whatever the worker finished. Called by the app's tick and by every `job.*` verb, so a wire
    // client never reads a state that has already changed.
    void pump();

    // Asks for cancellation and pumps until this job is terminal or the wait runs out. Cancelling a job
    // that already landed keeps its result: an answer that exists is never discarded.
    bool cancel(const std::string& id, std::string& error);
    // Re-queues a failed or cancelled job with a fresh id, same request bytes.
    std::optional<std::string> retry(const std::string& id, std::string& error);
    bool wait_for(const std::string& id, double timeout_s);

    // The current state as the last pump left it. `pump()` is what notices the worker finished, so a
    // caller that wants the live answer pumps first; `job.*` verbs do.
    std::vector<JobRecord> all() const;
    std::optional<JobRecord> find(const std::string& id) const;
    // What a queued request would be given for a timeout, which is the ETA the UI shows before the user
    // commits to a job.
    int predicted_timeout_s(const std::string& model, Extent size, int steps) const;
    // The answer's bytes for a succeeded job, read from disk (or the cache, which is the same file).
    bool result_png(const std::string& id, std::vector<std::uint8_t>& png, std::string& error);

    const LatencyModel& latency() const { return latency_; }
    nlohmann::json json() const;
    std::string directory() const { return directory_; }

private:
    struct InFlight {
        std::string id;
        BuiltRequest request;
        std::shared_ptr<std::atomic<bool>> cancelled;
        Landing landing;
        int timeout_s = kRequestTimeoutFloorS;
    };

    struct Completion {
        JobRecord record;
        std::vector<std::uint8_t> png;
        bool cancelled = false;
        bool answered = false;
        std::string failure;
        std::string error;
        std::string hint;
        double elapsed_s = 0.0;
    };

    void worker();
    void persist_locked();
    void load_from_disk();
    std::string new_id();

    Deps deps_;
    Clock clock_;
    std::string directory_;
    LatencyModel latency_;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<InFlight> queue_;
    std::map<std::string, JobRecord> records_;
    std::vector<std::string> order_;
    std::deque<Completion> completions_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> flags_;
    std::size_t next_id_ = 1;
    bool running_ = false;
    bool stopping_ = false;

    std::thread worker_;
};

}  // namespace noise::ai
