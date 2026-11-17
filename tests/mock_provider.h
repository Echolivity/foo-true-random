// Deterministic mock of the CPU instruction provider for unit tests.
// It never executes real RDSEED/RDRAND. It records any call to an instruction
// it was configured as unsupported, so tests can assert that the production
// code never executes unsupported instructions.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>

#include "ftr/hardware_rng.h"

namespace ftr_test {

// Behaviour of one simulated instruction. `fn(call_index, out)` returns CF.
using insn_fn = std::function<bool(std::uint64_t call_index, std::uint64_t& out)>;

// Distinct, never-repeating values (splitmix-style bijective mixing of the
// call index). Deterministic test data, NOT used by production code.
inline std::uint64_t distinct_value(std::uint64_t i, std::uint64_t salt) {
    std::uint64_t z = i + salt * 0x9E3779B97F4A7C15ull + 1;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline insn_fn always_ok(std::uint64_t salt) {
    return [salt](std::uint64_t i, std::uint64_t& out) {
        out = distinct_value(i, salt);
        return true;
    };
}
inline insn_fn always_fail() {
    return [](std::uint64_t, std::uint64_t& out) {
        out = 0;
        return false;
    };
}
// Succeeds for the first `n` calls, then fails forever.
inline insn_fn ok_then_fail(std::uint64_t n, std::uint64_t salt) {
    return [n, salt](std::uint64_t i, std::uint64_t& out) {
        if (i < n) {
            out = distinct_value(i, salt);
            return true;
        }
        out = 0;
        return false;
    };
}
// Returns the same value every time (a stuck generator).
inline insn_fn stuck(std::uint64_t value) {
    return [value](std::uint64_t, std::uint64_t& out) {
        out = value;
        return true;
    };
}

class mock_provider final : public ftr::instruction_provider {
public:
    ftr::cpu_features feats;
    insn_fn rdseed = always_fail();
    insn_fn rdrand = always_fail();
    std::uint64_t rdseed_calls = 0, rdrand_calls = 0, backoffs = 0;
    std::uint64_t illegal_rdseed = 0, illegal_rdrand = 0;  // must stay 0

    ftr::cpu_features features() noexcept override { return feats; }
    bool rdseed64_once(std::uint64_t& out) noexcept override {
        if (!feats.rdseed) {
            ++illegal_rdseed;  // would be #UD on real hardware
            out = 0;
            return false;
        }
        return rdseed(rdseed_calls++, out);
    }
    bool rdrand64_once(std::uint64_t& out) noexcept override {
        if (!feats.rdrand) {
            ++illegal_rdrand;
            out = 0;
            return false;
        }
        return rdrand(rdrand_calls++, out);
    }
    void backoff() noexcept override { ++backoffs; }
};

}  // namespace ftr_test
