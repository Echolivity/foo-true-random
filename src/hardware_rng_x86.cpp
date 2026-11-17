// foo-true-random — real CPU instruction provider (CPUID / RDSEED / RDRAND).
//
// Production target: MSVC, Windows x64. GCC/Clang x86-64 is supported so the
// same code can be exercised by the portable test suite on Linux.
// On any other architecture the provider reports no features, so the
// component fails closed.
//
// SPDX-License-Identifier: MIT
#include "ftr/hardware_rng.h"

#if defined(_MSC_VER) && defined(_M_X64)
#define FTR_X86_MSVC 1
#include <intrin.h>
#include <immintrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#define FTR_X86_GNU 1
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace ftr {

cpuid_snapshot read_cpuid_snapshot() noexcept {
    cpuid_snapshot s;
#if defined(FTR_X86_MSVC)
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    s.max_basic_leaf = static_cast<std::uint32_t>(r[0]);
    if (s.max_basic_leaf >= 1) {
        __cpuid(r, 1);
        s.leaf1_ecx = static_cast<std::uint32_t>(r[2]);
    }
    if (s.max_basic_leaf >= 7) {
        __cpuidex(r, 7, 0);
        s.leaf7_0_ebx = static_cast<std::uint32_t>(r[1]);
    }
#elif defined(FTR_X86_GNU)
    unsigned a = 0, b = 0, c = 0, d = 0;
    s.max_basic_leaf = __get_cpuid_max(0, nullptr);
    if (s.max_basic_leaf >= 1) {
        __cpuid(1, a, b, c, d);
        s.leaf1_ecx = c;
    }
    if (s.max_basic_leaf >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        s.leaf7_0_ebx = b;
    }
#endif
    return s;
}

namespace {

#if defined(FTR_X86_GNU)
// Per-function target attributes: the rest of the program is compiled without
// -mrdseed/-mrdrnd, so the compiler can never emit these instructions on an
// unguarded path.
__attribute__((target("rdseed"))) bool exec_rdseed64(std::uint64_t& out) noexcept {
    unsigned long long v = 0;
    const int cf = _rdseed64_step(&v);
    out = cf ? static_cast<std::uint64_t>(v) : 0;
    return cf != 0;
}
__attribute__((target("rdrnd"))) bool exec_rdrand64(std::uint64_t& out) noexcept {
    unsigned long long v = 0;
    const int cf = _rdrand64_step(&v);
    out = cf ? static_cast<std::uint64_t>(v) : 0;
    return cf != 0;
}
#elif defined(FTR_X86_MSVC)
bool exec_rdseed64(std::uint64_t& out) noexcept {
    unsigned __int64 v = 0;
    const int cf = _rdseed64_step(&v);
    out = cf ? static_cast<std::uint64_t>(v) : 0;
    return cf != 0;
}
bool exec_rdrand64(std::uint64_t& out) noexcept {
    unsigned __int64 v = 0;
    const int cf = _rdrand64_step(&v);
    out = cf ? static_cast<std::uint64_t>(v) : 0;
    return cf != 0;
}
#endif

class cpu_provider final : public instruction_provider {
public:
    cpu_provider() noexcept : m_features(decode_cpu_features(read_cpuid_snapshot())) {}

    cpu_features features() noexcept override { return m_features; }

    bool rdseed64_once(std::uint64_t& out) noexcept override {
        out = 0;
#if defined(FTR_X86_MSVC) || defined(FTR_X86_GNU)
        // Defence in depth: hardware_rng already guards, but never execute an
        // instruction this CPU did not advertise.
        if (!m_features.rdseed) return false;
        return exec_rdseed64(out);
#else
        return false;
#endif
    }

    bool rdrand64_once(std::uint64_t& out) noexcept override {
        out = 0;
#if defined(FTR_X86_MSVC) || defined(FTR_X86_GNU)
        if (!m_features.rdrand) return false;
        return exec_rdrand64(out);
#else
        return false;
#endif
    }

    void backoff() noexcept override {
#if defined(FTR_X86_MSVC) || defined(FTR_X86_GNU)
        _mm_pause();
#endif
    }

private:
    const cpu_features m_features;  // immutable after construction => thread-safe
};

}  // namespace

instruction_provider& cpu_instruction_provider() noexcept {
    // C++11 guarantees thread-safe initialization of function-local statics.
    static cpu_provider instance;
    return instance;
}

}  // namespace ftr
