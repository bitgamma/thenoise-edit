// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    // The bounds of the mask it was compiled against, which the crop is grown to the size policy from.
    // Reported because a person checks the artefacts against this box, and on an aspect frame the two
    // differ: `[0, 32, 420, 236]` painted inside `[0, 32, 420, 240]` asked for.
    Rect mask_bounds{};
    // What the request asked for, which is what the answer is resampled from. `answered_as_requested`
    // at landing time compares this to what came back.
    Extent requested{};
    // The compile shrank the ask to the model's pixel ceiling, so the answer is resampled over a region
    // it holds fewer pixels of. Carried rather than re-derived because the landing rebuilds its plan from
    // this record, and a plan assembled from two rectangles has no way to know the ask was cut - which is
    // how a rescale went unreported on the path that actually lands answers.
    bool resampled = false;
    Extent canvas{};
    // Where the coverage the region was compiled from lives: a canvas-sized PNG with the coverage in
    // the alpha channel, written by `enqueue` beside the job. Kept because a result that lands in a
    // later session must composite under the mask that was in force when it was asked for, not under
    // whatever is selected then.
    std::string coverage_png_path;
    // Null is the recipe's `auto`.
    std::optional<int> feather_px;
    bool colour_match = true;
    int lock_band_px = 0;
    int target_layer = 0;
    // What the document's composite was when the job was queued, so a landing that arrives after the
    // user moved on can be seen for what it is.
    std::string document_checksum_before;
    // Which document this answer belongs to, as `AppState::document_epoch` had it when the request was
    // queued. An answer lands by itself, and "by itself" cannot mean "into whatever file is open when
    // the model finally answers": the epoch is what a job may land in, and nothing else.
    std::uint64_t document_epoch = 0;
    // The answer is on the document, under this layer name. Durable, because reopening the project must
    // not stack a second copy of an answer that already landed - and because a beat that lands what is
    // finished is only safe if "finished" includes "already put there".
    bool landed = false;
    std::string landed_layer;
    // What the composite reported when the answer went on the document - counts, policy, the crop, and
    // whether anything outside the coverage moved. Kept because the claim "the answer reached no pixel
    // the mask did not claim" has to be checkable after the landing, whoever asked for it.
    nlohmann::json report;
    // The landing refused, and why, once. Retrying a refusal per frame would spend a composite per
    // frame and bury the sentence in a log.
    bool land_refused = false;
    std::string land_error;
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
    std::string blob;
    // What was asked, exactly as `ai.preview` shows it, so a job's own record answers "what did we
    // send?". Kept
    // because "what did I send" is the first question about any answer, and because a request must be
    // re-sendable after a restart without the caller holding it.
    nlohmann::json request;
    // The answer's PNG on disk, and its dimensions.
    std::string result_png;
    Extent result_dims{0, 0};
    // Where the uploaded region went: the bytes the model actually saw, kept for the probe and for
    // `prep_proof.py`, which is where a wrong upload shows up.
    std::string request_png;
    // Written from the injected clock, so a test sees the numbers it fed in.
    double submitted_at = 0.0;
    double started_at = 0.0;
    double finished_at = 0.0;
    int attempts = 0;
    Landing landing;

    nlohmann::json json() const;
};

// A queue of AI requests: one worker, because one server holds one image model resident at a time.
// Records live in memory and die with the run - a job belongs to a document revision a restart cannot
// guarantee, so pressing the verb again, which recompiles against the document that exists then, is
// the retry.
//
// The queue owns the thread and the scratch files; the app owns the document. Nothing here reaches for
// the document, which is what keeps the whole thing testable against a recorded transport.
class JobQueue {
public:
    struct Deps {
        Client* client = nullptr;
        // Scratch for this run's blobs - the coverage, the upload and each answer. Nothing here is read
        // back: a job is a fact about a socket, not about a document, and the queue dies with the run.
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
    bool wait_for(const std::string& id, double timeout_s);

    // The current state as the last pump left it. `pump()` is what notices the worker finished, so a
    // caller that wants the live answer pumps first; `job.*` verbs do.
    std::vector<JobRecord> all() const;
    std::optional<JobRecord> find(const std::string& id) const;
    // What a queued request would be given for a timeout, which is the ETA the UI shows before the user
    // commits to a job.
    int predicted_timeout_s(const std::string& model, Extent size, int steps) const;
    // Answers that belong on the document and are not there yet: succeeded, not landed, and queued
    // against the document open now. Landing them is the app's job - this only says which.
    std::vector<JobRecord> pending_landings(std::uint64_t document_epoch) const;
    // What the app reports once the answer is on the document, and once more when a landing refuses.
    void set_landed(const std::string& id, const std::string& layer_name,
                    const nlohmann::json& report);
    void set_land_refusal(const std::string& id, const std::string& error);

    // The answer's bytes for a succeeded job, read from this run's scratch directory.
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
