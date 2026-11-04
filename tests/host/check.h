// Minimal check macros for the DuckSleep host tests: no framework to install.
#pragma once

#include <cstdio>
#include <cstring>

namespace check {
inline int failures = 0;
inline int checks = 0;
}  // namespace check

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++check::checks;                                                         \
        if (!(cond)) {                                                           \
            ++check::failures;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++check::checks;                                                         \
        const long long va_ = static_cast<long long>(a);                         \
        const long long vb_ = static_cast<long long>(b);                         \
        if (va_ != vb_) {                                                        \
            ++check::failures;                                                   \
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,       \
                        __LINE__, #a, #b, va_, vb_);                             \
        }                                                                        \
    } while (0)

#define CHECK_STREQ(a, b)                                                        \
    do {                                                                         \
        ++check::checks;                                                         \
        if (std::strcmp((a), (b)) != 0) {                                        \
            ++check::failures;                                                   \
            std::printf("FAIL %s:%d: %s\n  got:      %s\n  expected: %s\n",      \
                        __FILE__, __LINE__, #a, (a), (b));                       \
        }                                                                        \
    } while (0)

/// Prints the summary and returns the process exit code.
inline int checkSummary(const char* suite) {
    if (check::failures == 0) {
        std::printf("PASS %s: %d checks\n", suite, check::checks);
        return 0;
    }
    std::printf("FAIL %s: %d of %d checks failed\n", suite, check::failures, check::checks);
    return 1;
}
