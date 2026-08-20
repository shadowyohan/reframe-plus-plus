#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void ReportFailure(const char* file, int line, const std::string& text) {
    std::printf("    FAIL %s:%d  %s\n", file, line, text.c_str());
    ++Failures();
}

inline int RunAll() {
    int failed_cases = 0;
    for (const auto& test : Registry()) {
        const int before = Failures();
        std::printf("  %s\n", test.name);
        test.fn();
        if (Failures() != before) ++failed_cases;
    }
    std::printf("\n%zu tests, %d failed\n", Registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}

#define TEST(name)                                                          \
    static void name();                                                     \
    static ::testing::Registrar reg_##name(#name, &name);                   \
    static void name()

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) ::testing::ReportFailure(__FILE__, __LINE__, #cond);   \
    } while (0)

#define SKIP(reason) std::printf("    skipped: %s\n", reason)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        const auto lhs_ = (a);                                              \
        const auto rhs_ = (b);                                              \
        if (!(lhs_ == rhs_))                                                \
            ::testing::ReportFailure(__FILE__, __LINE__,                    \
                                     std::string(#a " == " #b) + " (got " + \
                                         std::to_string(lhs_) + " vs " +    \
                                         std::to_string(rhs_) + ")");       \
    } while (0)
