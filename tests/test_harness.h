// Minimal dependency-free test harness.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ftr_test {

struct test_case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<test_case>& registry() {
    static std::vector<test_case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline int& checks() {
    static int c = 0;
    return c;
}

struct registrar {
    registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all(const char* suite) {
    int failed_tests = 0;
    for (auto& t : registry()) {
        const int before = failures();
        t.fn();
        const bool ok = failures() == before;
        if (!ok) ++failed_tests;
        std::printf("[%s] %s :: %s\n", ok ? "PASS" : "FAIL", suite, t.name);
    }
    std::printf("%s: %zu tests, %d checks, %d failed tests, %d failed checks\n", suite,
                registry().size(), checks(), failed_tests, failures());
    std::fflush(stdout);
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace ftr_test

#define FTR_CAT2(a, b) a##b
#define FTR_CAT(a, b) FTR_CAT2(a, b)
#define TEST(name)                                                                      \
    static void FTR_CAT(test_fn_, name)();                                              \
    static ::ftr_test::registrar FTR_CAT(test_reg_, name)(#name, &FTR_CAT(test_fn_, name)); \
    static void FTR_CAT(test_fn_, name)()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++::ftr_test::checks();                                                           \
        if (!(cond)) {                                                                    \
            ++::ftr_test::failures();                                                     \
            std::printf("  check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        ++::ftr_test::checks();                                                           \
        const auto ftr_a_ = (a);                                                          \
        const auto ftr_b_ = (b);                                                          \
        if (!(ftr_a_ == ftr_b_)) {                                                        \
            ++::ftr_test::failures();                                                     \
            std::printf("  check failed: %s == %s (%s:%d) [%s vs %s]\n", #a, #b, __FILE__, \
                        __LINE__, std::to_string(ftr_a_).c_str(),                         \
                        std::to_string(ftr_b_).c_str());                                  \
        }                                                                                 \
    } while (0)

#define TEST_MAIN(suite) \
    int main() { return ::ftr_test::run_all(suite); }
