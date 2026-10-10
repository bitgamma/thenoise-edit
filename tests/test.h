// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <type_traits>

namespace noise::test {

template <typename T>
inline std::string describe(const T& value) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (requires(const T& v) {
                              v.size();
                              std::begin(v);
                          }) {
        // Byte buffers and spans are what this project compares most often, and they have no
        // useful `to_string`. Length plus the head of the data is enough to see which side is
        // the odd one out; elements that are not numbers just get the length.
        std::string described = "size=" + std::to_string(value.size());
        using element = std::decay_t<decltype(*std::begin(value))>;
        if constexpr (std::is_arithmetic_v<element>) {
            int shown = 0;
            for (const element& item : value) {
                if (shown++ == 8) {
                    described += " ...";
                    break;
                }
                described += " " + std::to_string(static_cast<long long>(item));
            }
        }
        return described;
    } else if constexpr (requires { std::to_string(value); }) {
        return std::to_string(value);
    } else {
        // Rect, Extent, Mask and friends: the expression the check printed already names both
        // sides, which is enough to see which one is the odd one out.
        return "<not printable>";
    }
}

template <typename A, typename B>
inline std::string diff(const A& a, const B& b) {
    return "lhs=" + describe(a) + " rhs=" + describe(b);
}

inline int failures = 0;
inline int checks = 0;

inline void report(const char* expression, const char* file, int line, const std::string& detail) {
    ++failures;
    std::fprintf(stderr, "FAIL %s:%d  %s  %s\n", file, line, expression, detail.c_str());
}

inline void expect(bool condition, const char* expression, const char* file, int line,
                   const std::string& detail = {}) {
    ++checks;
    if (!condition) {
        report(expression, file, line, detail);
    }
}

inline void expect_near(double a, double b, double epsilon, const char* expression,
                        const char* file, int line) {
    ++checks;
    if (!(std::fabs(a - b) <= epsilon)) {
        report(expression, file, line, "expected " + std::to_string(a) + " ~= " + std::to_string(b));
    }
}

// Both operands evaluated exactly once, so a side-effecting argument (a command call) is
// safe here: a comparison that runs the code twice reports a difference that is not there and
// changes state twice.
template <typename A, typename B>
inline void expect_eq(const A& a, const B& b, const char* expression, const char* file, int line) {
    ++checks;
    if (!(a == b)) {
        report(expression, file, line, diff(a, b));
    }
}

template <typename A, typename B>
inline void expect_ne(const A& a, const B& b, const char* expression, const char* file, int line) {
    ++checks;
    if (a == b) {
        report(expression, file, line, diff(a, b));
    }
}

// Anything with `.ok` and `.error` (CommandResult): evaluated once, so the argument
// may be a call.
template <typename R>
inline void expect_ok(const R& result, const char* expression, const char* file, int line) {
    ++checks;
    if (!result.ok) {
        report(expression, file, line, result.error);
    }
}

// A precondition, not an assertion: continuing after one would compare against a state the test never
// set up, which reports a failure that did not happen.
inline void require(bool condition, const char* expression, const char* file, int line) {
    ++checks;
    if (!condition) {
        report(expression, file, line, "PRECONDITION");
        std::fflush(stderr);
        std::exit(1);
    }
}

inline int summary(const char* suite) {
    std::fprintf(stderr, "%s: %d checks, %d failures\n", suite, checks, failures);
    return failures == 0 ? 0 : 1;
}

}

#define REQUIRE(...) ::noise::test::require((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK(...) ::noise::test::expect((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_OK(expr) ::noise::test::expect_ok((expr), #expr " ok", __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::noise::test::expect_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NE(a, b) ::noise::test::expect_ne((a), (b), #a " != " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::noise::test::expect_near((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)
