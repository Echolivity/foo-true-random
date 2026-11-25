// foo-true-random — random-number source abstraction.
//
// Values come from the CPU's RDSEED, then RDRAND. Only on a CPU that supports
// NEITHER instruction (e.g. Intel Sandy Bridge and older) may an operating-system
// generator be used instead (on Windows: BCryptGenRandom), and only if the caller
// passes one in. A CPU that has RDSEED/RDRAND never falls back: if its hardware
// fails, every call fails and the caller must abort. There is no software PRNG
// and no std::random_device anywhere in this project.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace ftr {

enum class rng_source { none, rdseed, rdrand, system };
const char* to_string(rng_source s) noexcept;

struct cpu_features {
    bool rdseed = false;
    bool rdrand = false;
};

// The raw CPUID register values that feature detection depends on.
struct cpuid_snapshot {
    std::uint32_t max_basic_leaf = 0;  // CPUID(0).EAX
    std::uint32_t leaf1_ecx = 0;       // CPUID(1).ECX
    std::uint32_t leaf7_0_ebx = 0;     // CPUID(7, 0).EBX (only meaningful if max_basic_leaf >= 7)
};

inline constexpr unsigned cpuid_rdrand_bit = 30;  // CPUID.(EAX=1):ECX[30]
inline constexpr unsigned cpuid_rdseed_bit = 18;  // CPUID.(EAX=7,ECX=0):EBX[18]

// Pure decoding of CPUID registers into features. Leaf 7 is only consulted if
// the CPU reports it (max basic leaf >= 7); otherwise RDSEED is unsupported.
constexpr cpu_features decode_cpu_features(const cpuid_snapshot& s) noexcept {
    cpu_features f;
    f.rdrand = s.max_basic_leaf >= 1 && ((s.leaf1_ecx >> cpuid_rdrand_bit) & 1u) != 0;
    f.rdseed = s.max_basic_leaf >= 7 && ((s.leaf7_0_ebx >> cpuid_rdseed_bit) & 1u) != 0;
    return f;
}

// One-shot access to the instructions. Implementations must not retry; retry
// policy lives in hardware_rng so it can be unit-tested with mock providers.
//
// Contract: rdseed64_once / rdrand64_once are only called after features()
// reported the instruction as supported. hardware_rng enforces this.
class instruction_provider {
public:
    virtual ~instruction_provider() = default;
    virtual cpu_features features() noexcept = 0;
    // Execute the instruction once. Returns the carry flag (true = valid value).
    virtual bool rdseed64_once(std::uint64_t& out) noexcept = 0;
    virtual bool rdrand64_once(std::uint64_t& out) noexcept = 0;
    // Called between failed attempts (PAUSE on real hardware).
    virtual void backoff() noexcept {}
};

// The real CPU. On x86-64 this executes CPUID / RDSEED / RDRAND; on other
// architectures it reports no features (and therefore fails closed).
// The returned object is stateless and safe to use from any thread.
instruction_provider& cpu_instruction_provider() noexcept;

// Raw CPUID values of the running CPU (all zero on non-x86-64 builds).
cpuid_snapshot read_cpuid_snapshot() noexcept;

// The operating system's cryptographic random number generator. Used ONLY on
// CPUs that report neither RDSEED nor RDRAND (see hardware_rng).
class system_rng_provider {
public:
    virtual ~system_rng_provider() = default;
    // One request for 64 random bits. Returns false on failure; never retries.
    virtual bool random64(std::uint64_t& out) noexcept = 0;
};

// The real OS generator: BCryptGenRandom on Windows (getrandom on Linux, used by
// the portable test suite). Stateless and safe to use from any thread.
system_rng_provider& os_system_rng_provider() noexcept;

struct rng_policy {
    // RDSEED may legitimately fail when its entropy buffer is drained; retry
    // with PAUSE in between. After this many consecutive failures the source
    // is marked unavailable for this hardware_rng instance.
    unsigned rdseed_attempts = 1024;
    // Intel's guidance for RDRAND is that 10 consecutive failures indicate a
    // hardware problem.
    unsigned rdrand_attempts = 10;
    // Number of 64-bit words drawn (and discarded) from each supported source
    // when the instance is created, to detect a stuck source before any of
    // its output is used.
    unsigned self_test_words = 8;
};

enum class rng_state {
    ready,              // at least one source is usable
    no_hardware,        // CPUID reports neither RDSEED nor RDRAND, and no system fallback was given
    sources_exhausted,  // every supported source failed (retry limit or self-test)
    stuck_output,       // a source returned identical consecutive words; operation aborted
};
const char* to_string(rng_state s) noexcept;

// RNG with source selection: RDSEED, then RDRAND, then failure.
// If `system_fallback` is given AND the CPU supports neither instruction, the
// operating-system generator is the (only) source instead. It is never used
// on a CPU that has RDSEED or RDRAND, not even when their hardware fails.
//
// Usage pattern: create one instance per operation. The instance is NOT
// thread-safe (it holds selection state and a one-word buffer); separate
// instances on separate threads are independent.
//
// Satisfies the Source concept of random_core.h: `bool operator()(uint32_t&)`.
class hardware_rng {
public:
    explicit hardware_rng(instruction_provider& provider, rng_policy policy = {},
                          system_rng_provider* system_fallback = nullptr) noexcept;

    bool next_u32(std::uint32_t& out) noexcept;
    bool next_u64(std::uint64_t& out) noexcept;
    bool operator()(std::uint32_t& out) noexcept { return next_u32(out); }

    rng_state state() const noexcept { return m_state; }
    bool usable() const noexcept { return m_state == rng_state::ready; }

    cpu_features features() const noexcept { return m_features; }
    // Source that will be tried first for the next value (none if failed).
    rng_source current_source() const noexcept;
    // Source that supplied the most recent value handed out (none if no value
    // has been produced yet). This is what status reporting should display.
    rng_source last_source() const noexcept { return m_last_source; }

    // True when this CPU has neither instruction and the OS generator is the source.
    bool using_system_rng() const noexcept { return m_use_system; }
    bool rdseed_failed() const noexcept { return m_rdseed_failed; }
    bool rdrand_failed() const noexcept { return m_rdrand_failed; }
    // Number of 64-bit hardware words consumed from each source (excluding
    // the discarded self-test words).
    std::uint64_t words_from_rdseed() const noexcept { return m_words_rdseed; }
    std::uint64_t words_from_rdrand() const noexcept { return m_words_rdrand; }
    std::uint64_t words_from_system() const noexcept { return m_words_system; }

    // Human-readable account of which hardware sources supplied values.
    std::string usage_summary() const;
    // Human-readable failure explanation (empty when usable()).
    std::string failure_reason() const;

private:
    bool draw64(rng_source src, std::uint64_t& out) noexcept;
    bool self_test(rng_source src) noexcept;
    void update_state() noexcept;

    instruction_provider& m_provider;
    system_rng_provider* m_system = nullptr;
    rng_policy m_policy;
    cpu_features m_features;
    rng_state m_state = rng_state::ready;
    bool m_rdseed_failed = false;
    bool m_rdrand_failed = false;
    bool m_use_system = false, m_system_failed = false;
    bool m_have_last_system = false;
    std::uint64_t m_last_system = 0;
    bool m_have_last_rdseed = false, m_have_last_rdrand = false;
    std::uint64_t m_last_rdseed = 0, m_last_rdrand = 0;
    bool m_have_buffer = false;
    std::uint32_t m_buffer = 0;
    rng_source m_buffer_source = rng_source::none;
    rng_source m_last_source = rng_source::none;
    std::uint64_t m_words_rdseed = 0, m_words_rdrand = 0, m_words_system = 0;
};

// Human-readable capability line for the running CPU, e.g.
// "RDSEED: supported, RDRAND: supported".
std::string describe_features(const cpu_features& f);

}  // namespace ftr
