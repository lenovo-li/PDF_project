// Test.h — zero-dependency native C++ test framework.
//
// Depends only on the C++ standard library. Tests self-register at static-init
// time via the TEST(suite, name) macro; TestMain.cpp runs all registered tests,
// prints a summary, and returns a process exit code (0 = all passed).
//
// Assertions:
//   EXPECT_TRUE(cond)      EXPECT_FALSE(cond)
//   EXPECT_EQ(a, b)        EXPECT_NE(a, b)
//   EXPECT_NEAR(a, b, tol) — float compare with absolute tolerance
// A failed EXPECT records the failure (file:line + message) and marks the test
// failed, but the test keeps running. Copied from ../Fluent/tests/framework/Test.h
// (same author) so this repo has no build dependency on the Fluent test tree.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fltest {

// One registered test case.
struct TestCase {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

// Global registry (Meyers singleton to avoid static-init-order issues).
inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// Per-test failure state, set by the assertion macros while a test runs.
struct RunState {
    int failures = 0;
    std::string suiteDotName;
};
inline RunState& Current() {
    static RunState state;
    return state;
}

// Registers a test at construction time.
struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        Registry().push_back({suite, name, std::move(fn)});
    }
};

// Record a failure line and increment the current test's failure count.
inline void ReportFailure(const char* file, int line, const std::string& msg) {
    std::printf("  [FAIL] %s(%d): %s\n", file, line, msg.c_str());
    Current().failures++;
}

// Stringify helpers used by EXPECT_EQ/NE for the diagnostic message.
template <typename T>
std::string ToStr(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else {
        return std::string("<value>");
    }
}

}  // namespace fltest

// --- Registration macro ----------------------------------------------------
#define TEST(suite, name)                                                     \
    static void suite##_##name##_body();                                      \
    static ::fltest::Registrar suite##_##name##_reg(                          \
        #suite, #name, suite##_##name##_body);                                \
    static void suite##_##name##_body()

// --- Assertion macros ------------------------------------------------------
#define EXPECT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond))                                                          \
            ::fltest::ReportFailure(__FILE__, __LINE__,                       \
                                    "EXPECT_TRUE(" #cond ")");                \
    } while (0)

#define EXPECT_FALSE(cond)                                                    \
    do {                                                                      \
        if (cond)                                                             \
            ::fltest::ReportFailure(__FILE__, __LINE__,                       \
                                    "EXPECT_FALSE(" #cond ")");               \
    } while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto _va = (a);                                                       \
        auto _vb = (b);                                                       \
        if (!(_va == _vb))                                                    \
            ::fltest::ReportFailure(                                          \
                __FILE__, __LINE__,                                           \
                "EXPECT_EQ(" #a ", " #b ") got " + ::fltest::ToStr(_va) +     \
                    " vs " + ::fltest::ToStr(_vb));                           \
    } while (0)

#define EXPECT_NE(a, b)                                                       \
    do {                                                                      \
        auto _va = (a);                                                       \
        auto _vb = (b);                                                       \
        if (!(_va != _vb))                                                    \
            ::fltest::ReportFailure(__FILE__, __LINE__,                       \
                                    "EXPECT_NE(" #a ", " #b ")");             \
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                                \
    do {                                                                      \
        double _va = (a);                                                     \
        double _vb = (b);                                                     \
        double _t = (tol);                                                    \
        if (std::fabs(_va - _vb) > _t)                                        \
            ::fltest::ReportFailure(                                          \
                __FILE__, __LINE__,                                           \
                "EXPECT_NEAR(" #a ", " #b ", " #tol ") got " +                \
                    std::to_string(_va) + " vs " + std::to_string(_vb));      \
    } while (0)
