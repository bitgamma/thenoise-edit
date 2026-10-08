// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

namespace noise {

// Splits a byte stream into '\n'-terminated frames; call pop() until it returns
// false. A frame longer than the configured bound raises overflow(), and the
// caller drops the connection rather than resyncing on garbage.
class LineFramer {
public:
    explicit LineFramer(std::size_t max_line_bytes) : max_line_bytes_(max_line_bytes) {}

    void push(const char* data, std::size_t len) {
        if (overflow_) {
            return;
        }
        buf_.append(data, len);
        if (buf_.size() > max_line_bytes_) {
            overflow_ = true;
            buf_.clear();
        }
    }

    bool pop(std::string& out) {
        const std::size_t newline = buf_.find('\n');
        if (newline == std::string::npos) {
            return false;
        }
        out = buf_.substr(0, newline);
        buf_.erase(0, newline + 1);
        return true;
    }

    bool overflowed() const { return overflow_; }

private:
    std::size_t max_line_bytes_ = 0;
    std::string buf_;
    bool overflow_ = false;
};

}
