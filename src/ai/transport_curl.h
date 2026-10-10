// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include "ai/transport.h"

namespace noise::ai {

// libcurl: connect timeouts, an in-flight abort for cancel, chunked bodies and TLS verification are the
// things the failure table branches on, and they are not a slice to reimplement. One easy handle per
// call, so no handle outlives the request it belongs to.
std::unique_ptr<Transport> make_curl_transport();

}  // namespace noise::ai
