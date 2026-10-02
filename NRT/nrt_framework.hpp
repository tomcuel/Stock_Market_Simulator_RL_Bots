//=======================================================================
// Non-regression-test framework: no external dependency (no Catch2/gtest)
// self-register test cases and run them with clear PASS/FAIL output and a non-zero exit code on failure (so it plugs straight into CI)
//=======================================================================
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nrt {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct AssertionFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace nrt

#define NRT_CONCAT_INNER(a, b) a##b
#define NRT_CONCAT(a, b) NRT_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                     \
    static void name();                                                                     \
    namespace {                                                                             \
    ::nrt::TestRegistrar NRT_CONCAT(registrar_, __LINE__)(#name, name);                     \
    }                                                                                       \
    static void name()

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::ostringstream oss;                                                         \
            oss << "CHECK failed: " #cond " at " << __FILE__ << ":" << __LINE__;            \
            throw ::nrt::AssertionFailure(oss.str());                                       \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                      \
    do {                                                                                    \
        auto nrt_a = (a);                                                                   \
        auto nrt_b = (b);                                                                   \
        if (!(nrt_a == nrt_b)) {                                                            \
            std::ostringstream oss;                                                         \
            oss << "CHECK_EQ failed: " #a " (" << nrt_a << ") != " #b " (" << nrt_b << ")"  << " at " << __FILE__ << ":" << __LINE__; \
            throw ::nrt::AssertionFailure(oss.str());                                       \
        }                                                                                   \
    } while (0)

#define CHECK_NEAR(a, b, epsilon)                                                           \
    do {                                                                                    \
        auto nrt_a = (a);                                                                   \
        auto nrt_b = (b);                                                                   \
        auto nrt_diff = (nrt_a > nrt_b) ? (nrt_a - nrt_b) : (nrt_b - nrt_a);                \
        if (nrt_diff > (epsilon)) {                                                         \
            std::ostringstream oss;                                                         \
            oss << "CHECK_NEAR failed: " #a " (" << nrt_a << ") != " #b " (" << nrt_b << ")" << " within " << (epsilon) << " at " << __FILE__ << ":" << __LINE__; \
            throw ::nrt::AssertionFailure(oss.str());                                       \
        }                                                                                   \
    } while (0)
