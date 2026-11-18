// Real-hardware test: executes CPUID and, only if CPUID reports support,
// RDSEED / RDRAND on the machine running the test. Prints evidence.
//
// Exit codes: 0 = all checks passed (or hardware absent and fail-closed
// behaviour verified), 1 = failure.
//
// The chi-square check below is a SANITY check that the plumbing is not
// grossly broken. It is not, and cannot be, a proof of unbiasedness; that
// proof is the exhaustive analysis in test_core.cpp.
// SPDX-License-Identifier: MIT
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ftr/hardware_rng.h"
#include "ftr/random_core.h"
#include "test_harness.h"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif

using namespace ftr;

namespace {

std::string cpu_brand() {
    char brand[49] = {0};
#if defined(_MSC_VER) && defined(_M_X64)
    int r[4];
    __cpuid(r, 0x80000000);
    if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
        for (int k = 0; k < 3; ++k) {
            __cpuid(r, 0x80000002 + k);
            std::memcpy(brand + 16 * k, r, 16);
        }
    }
#elif defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u) {
        for (unsigned k = 0; k < 3; ++k) {
            __cpuid(0x80000002u + k, a, b, c, d);
            unsigned r[4] = {a, b, c, d};
            std::memcpy(brand + 16 * k, r, 16);
        }
    }
#endif
    std::string s(brand);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s.empty() ? "unknown" : s;
}

}  // namespace

TEST(real_cpu_detection_and_source) {
    const cpuid_snapshot snap = read_cpuid_snapshot();
    const cpu_features f = decode_cpu_features(snap);
    std::printf("  CPU: %s\n", cpu_brand().c_str());
    std::printf("  CPUID: max basic leaf=0x%X, leaf1.ECX=0x%08X (bit30 RDRAND=%u), leaf7.EBX=0x%08X (bit18 RDSEED=%u)\n",
                snap.max_basic_leaf, snap.leaf1_ecx, (snap.leaf1_ecx >> 30) & 1u, snap.leaf7_0_ebx,
                (snap.leaf7_0_ebx >> 18) & 1u);
    std::printf("  %s\n", describe_features(f).c_str());

    hardware_rng rng(cpu_instruction_provider());
    if (!f.rdseed && !f.rdrand) {
        std::printf("  No hardware RNG on this CPU: verifying fail-closed behaviour.\n");
        CHECK(!rng.usable());
        std::vector<std::size_t> order;
        CHECK(make_permutation<std::uint32_t>(rng, 10, order) == core_status::rng_failure);
        std::printf("  Failure message: %s\n", rng.failure_reason().c_str());
        return;
    }
    CHECK(rng.usable());
    std::printf("  Preferred source after self-test: %s\n", to_string(rng.current_source()));
    std::uint64_t w[4] = {0, 0, 0, 0};
    for (auto& x : w) CHECK(rng.next_u64(x));
    std::printf("  Sample words: %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n", w[0], w[1], w[2], w[3]);
    std::printf("  Actual source of last value: %s\n", to_string(rng.last_source()));
    if (f.rdseed) CHECK(rng.last_source() == rng_source::rdseed || rng.rdseed_failed());
    CHECK(rng.last_source() != rng_source::none);
}

TEST(real_shuffle_large_list) {
    hardware_rng rng(cpu_instruction_provider());
    if (!rng.usable()) {
        std::printf("  skipped: no hardware RNG (fail-closed verified in previous test)\n");
        return;
    }
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(rng, 200000, order) == core_status::ok);
    CHECK(is_permutation_of_iota(order));
    std::printf("  200000-element permutation OK, source usage: %s\n", rng.usage_summary().c_str());
}

TEST(real_sanity_chi_square_n3) {
    hardware_rng rng(cpu_instruction_provider());
    if (!rng.usable()) {
        std::printf("  skipped: no hardware RNG\n");
        return;
    }
    const int trials = 120000;
    std::map<std::vector<std::size_t>, int> counts;
    for (int t = 0; t < trials; ++t) {
        std::vector<std::size_t> order;
        if (make_permutation<std::uint32_t>(rng, 3, order) != core_status::ok) {
            CHECK(false);
            return;
        }
        ++counts[order];
    }
    CHECK_EQ(counts.size(), std::size_t(6));
    const double expected = trials / 6.0;
    double chi2 = 0;
    for (auto& kv : counts) {
        const double d = kv.second - expected;
        chi2 += d * d / expected;
        std::printf("  [%zu %zu %zu]: %d\n", kv.first[0], kv.first[1], kv.first[2], kv.second);
    }
    // Diagnostic only, not a proof (the proof is the exhaustive tests in test_core).
    // The pass/fail threshold is set where a working generator fails with
    // probability ~1e-13 (5 dof, P(chi2 > 70)), so the test is not flaky; it only
    // catches grossly broken plumbing (e.g. a constant or heavily skewed source).
    std::printf("  chi-square (5 dof) = %.3f (fails only above 70, p~1e-13), source usage: %s\n", chi2,
                rng.usage_summary().c_str());
    CHECK(chi2 < 70.0);
}

TEST_MAIN("hardware_real")
