#pragma once
// Tiny inline test framework — plain asserts with counting, no dependencies.

#include <cstdio>
#include <string>

namespace testfw {

inline int g_failures = 0;
inline int g_checks = 0;
inline const char* g_current_test = "";

#define TEST_CASE(name)                                     \
    testfw::g_current_test = name;                          \
    std::printf("--- %s\n", name);

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++testfw::g_checks;                                                      \
        if (!(cond)) {                                                           \
            ++testfw::g_failures;                                                \
            std::printf("FAIL [%s] %s:%d: %s\n", testfw::g_current_test,         \
                        __FILE__, __LINE__, #cond);                              \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++testfw::g_checks;                                                      \
        auto va = (a);                                                           \
        auto vb = (b);                                                           \
        if (!(va == vb)) {                                                       \
            ++testfw::g_failures;                                                \
            std::printf("FAIL [%s] %s:%d: %s == %s\n", testfw::g_current_test,   \
                        __FILE__, __LINE__, #a, #b);                             \
        }                                                                        \
    } while (0)

#define CHECK_STREQ(a, b)                                                        \
    do {                                                                         \
        ++testfw::g_checks;                                                      \
        std::string va = (a);                                                    \
        std::string vb = (b);                                                    \
        if (va != vb) {                                                          \
            ++testfw::g_failures;                                                \
            std::printf("FAIL [%s] %s:%d: %s == %s\n  actual:   \"%s\"\n"        \
                        "  expected: \"%s\"\n",                                  \
                        testfw::g_current_test, __FILE__, __LINE__, #a, #b,      \
                        va.c_str(), vb.c_str());                                 \
        }                                                                        \
    } while (0)

inline int summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("OK: %s — %d checks passed\n", suite, g_checks);
    } else {
        std::printf("FAILED: %s — %d of %d checks failed\n", suite, g_failures, g_checks);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace testfw
