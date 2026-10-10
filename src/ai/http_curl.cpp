// SPDX-License-Identifier: Apache-2.0
#include "ai/transport_curl.h"

#include <curl/curl.h>

#include <string>

namespace noise::ai {

namespace {

std::size_t write_to_vector(void* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<std::vector<std::uint8_t>*>(user);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    body->insert(body->end(), bytes, bytes + size * count);
    return size * count;
}

struct Progress {
    const std::atomic<bool>* cancelled;
};

int abort_if_cancelled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* progress = static_cast<Progress*>(user);
    return progress->cancelled->load() ? 1 : 0;
}

// The libcurl lifetime. `curl_global_init` is not thread-safe and must happen before any handle exists,
// which in this app means before any job runs; a static initializer is the one place that can promise it.
struct CurlGlobal {
    CurlGlobal() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlGlobal() { curl_global_cleanup(); }
    CURLcode code;
};

CURLcode global_init() {
    static const CurlGlobal kGlobal;
    return kGlobal.code;
}

}  // namespace

class CurlTransport final : public Transport {
public:
    IncomingResponse send(const OutgoingRequest& request,
                          const std::atomic<bool>& cancelled) override {
        IncomingResponse response;
        if (const CURLcode code = global_init(); code != CURLE_OK) {
            response.transport_error = curl_easy_strerror(code);
            return response;
        }
        if (cancelled.load()) {
            response.transport_error = kErrorCancelled;
            return response;
        }
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            response.transport_error = "curl: no handle";
            return response;
        }

        struct HandleGuard {
            CURL* handle;
            ~HandleGuard() { curl_easy_cleanup(handle); }
        } guard{handle};

        struct curl_slist* headers = nullptr;
        if (!request.bearer.empty()) {
            const std::string header = "Authorization: Bearer " + request.bearer;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (!request.content_type.empty()) {
            const std::string header = "Content-Type: " + request.content_type;
            headers = curl_slist_append(headers, header.c_str());
        }
        struct SlistGuard {
            curl_slist** list;
            ~SlistGuard() {
                if (*list != nullptr) {
                    curl_slist_free_all(*list);
                }
            }
        } slist_guard{&headers};

        Progress progress{&cancelled};
        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,
                         static_cast<long>(request.connect_timeout_s));  // NOLINT
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(request.timeout_s));  // NOLINT
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_to_vector);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, abort_if_cancelled);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, request.verify_tls ? 1L : 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, request.verify_tls ? 2L : 0L);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        if (headers != nullptr) {
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        }
        if (request.method != "GET") {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body.size()));  // NOLINT
            curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS,
                             reinterpret_cast<const char*>(request.body.data()));
        }

        const CURLcode code = curl_easy_perform(handle);
        if (code != CURLE_OK) {
            response.transport_error = code == CURLE_ABORTED_BY_CALLBACK
                                           ? kErrorCancelled
                                           : (code == CURLE_OPERATION_TIMEDOUT ? kErrorTimeout
                                                                               : curl_easy_strerror(code));
            return response;
        }
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        response.status = status;
        return response;
    }
};

std::unique_ptr<Transport> make_curl_transport() {
    return std::make_unique<CurlTransport>();
}

}  // namespace noise::ai
