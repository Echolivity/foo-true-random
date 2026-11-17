// foo-true-random — portable hardware RNG selection / retry / health logic.
// The actual instructions live in hardware_rng_x86.cpp.
//
// SPDX-License-Identifier: MIT
#include "ftr/hardware_rng.h"
#include "ftr/random_core.h"

namespace ftr {

const char* to_string(rng_source s) noexcept {
    switch (s) {
        case rng_source::rdseed: return "RDSEED";
        case rng_source::rdrand: return "RDRAND";
        case rng_source::none: break;
    }
    return "none";
}

const char* to_string(rng_state s) noexcept {
    switch (s) {
        case rng_state::ready: return "ready";
        case rng_state::no_hardware: return "hardware RNG unavailable (CPU supports neither RDSEED nor RDRAND)";
        case rng_state::sources_exhausted: return "hardware RNG failed (every supported instruction exceeded its retry limit or failed its self-test)";
        case rng_state::stuck_output: return "hardware RNG returned identical consecutive values; output rejected";
    }
    return "unknown";
}

const char* to_string(core_status s) noexcept {
    switch (s) {
        case core_status::ok: return "ok";
        case core_status::zero_bound: return "zero bound";
        case core_status::rng_failure: return "hardware random number generation failed";
        case core_status::rejection_limit: return "rejection-sampling attempt limit exhausted";
        case core_status::too_many_items: return "too many items";
    }
    return "unknown";
}

bool is_permutation_of_iota(const std::vector<std::size_t>& order) noexcept {
    std::vector<unsigned char> seen(order.size(), 0);
    for (std::size_t idx : order) {
        if (idx >= order.size() || seen[idx]) return false;
        seen[idx] = 1;
    }
    return true;
}

std::string describe_features(const cpu_features& f) {
    std::string s = "RDSEED: ";
    s += f.rdseed ? "supported" : "not supported";
    s += ", RDRAND: ";
    s += f.rdrand ? "supported" : "not supported";
    return s;
}

hardware_rng::hardware_rng(instruction_provider& provider, rng_policy policy) noexcept
    : m_provider(provider), m_policy(policy), m_features(provider.features()) {
    if (!m_features.rdseed && !m_features.rdrand) {
        m_state = rng_state::no_hardware;
        return;
    }
    // Never execute an instruction CPUID did not report.
    if (m_features.rdseed && !self_test(rng_source::rdseed)) m_rdseed_failed = true;
    if (m_features.rdrand && !self_test(rng_source::rdrand)) m_rdrand_failed = true;
    update_state();
}

void hardware_rng::update_state() noexcept {
    if (m_state == rng_state::stuck_output || m_state == rng_state::no_hardware) return;
    const bool seed_ok = m_features.rdseed && !m_rdseed_failed;
    const bool rand_ok = m_features.rdrand && !m_rdrand_failed;
    m_state = (seed_ok || rand_ok) ? rng_state::ready : rng_state::sources_exhausted;
}

rng_source hardware_rng::current_source() const noexcept {
    if (m_state != rng_state::ready) return rng_source::none;
    if (m_features.rdseed && !m_rdseed_failed) return rng_source::rdseed;
    if (m_features.rdrand && !m_rdrand_failed) return rng_source::rdrand;
    return rng_source::none;
}

bool hardware_rng::self_test(rng_source src) noexcept {
    // Words drawn here are discarded; they are never used for shuffling.
    bool ok = true;
    for (unsigned k = 0; k < m_policy.self_test_words && ok; ++k) {
        std::uint64_t v = 0;
        ok = draw64(src, v);
    }
    // A stuck detection during self-test only disqualifies the source (draw64
    // already marked it failed); it is not an operation-level abort, because
    // none of its output has been used. update_state() decides the final state.
    if (m_state == rng_state::stuck_output) m_state = rng_state::ready;
    return ok;
}

bool hardware_rng::draw64(rng_source src, std::uint64_t& out) noexcept {
    const bool is_seed = (src == rng_source::rdseed);
    // Hard guard: never execute an instruction CPUID did not report.
    if (is_seed ? !m_features.rdseed : !m_features.rdrand) return false;
    const unsigned attempts = is_seed ? m_policy.rdseed_attempts : m_policy.rdrand_attempts;
    for (unsigned a = 0; a < attempts; ++a) {
        std::uint64_t v = 0;
        const bool cf = is_seed ? m_provider.rdseed64_once(v) : m_provider.rdrand64_once(v);
        if (cf) {
            bool& have = is_seed ? m_have_last_rdseed : m_have_last_rdrand;
            std::uint64_t& last = is_seed ? m_last_rdseed : m_last_rdrand;
            if (have && last == v) {
                // Two identical consecutive 64-bit outputs: probability 2^-64
                // for a healthy source. Treat as a hardware fault.
                (is_seed ? m_rdseed_failed : m_rdrand_failed) = true;
                m_state = rng_state::stuck_output;
                return false;
            }
            have = true;
            last = v;
            out = v;
            return true;
        }
        m_provider.backoff();
    }
    return false;
}

bool hardware_rng::next_u64(std::uint64_t& out) noexcept {
    if (m_state != rng_state::ready) return false;
    if (m_features.rdseed && !m_rdseed_failed) {
        if (draw64(rng_source::rdseed, out)) {
            m_last_source = rng_source::rdseed;
            ++m_words_rdseed;
            return true;
        }
        if (m_state == rng_state::stuck_output) return false;  // fail closed, no switching
        m_rdseed_failed = true;  // retry budget exhausted: unavailable for this instance
    }
    if (m_features.rdrand && !m_rdrand_failed) {
        if (draw64(rng_source::rdrand, out)) {
            m_last_source = rng_source::rdrand;
            ++m_words_rdrand;
            return true;
        }
        if (m_state == rng_state::stuck_output) return false;
        m_rdrand_failed = true;
    }
    update_state();
    return false;
}

bool hardware_rng::next_u32(std::uint32_t& out) noexcept {
    if (m_have_buffer) {
        // The buffered half came from a word that was valid when drawn.
        if (m_state != rng_state::ready) {
            m_have_buffer = false;
            return false;
        }
        out = m_buffer;
        m_have_buffer = false;
        m_last_source = m_buffer_source;
        return true;
    }
    std::uint64_t w = 0;
    if (!next_u64(w)) return false;
    out = static_cast<std::uint32_t>(w);
    m_buffer = static_cast<std::uint32_t>(w >> 32);
    m_buffer_source = m_last_source;
    m_have_buffer = true;
    return true;
}

std::string hardware_rng::usage_summary() const {
    std::string s;
    if (m_words_rdseed == 0 && m_words_rdrand == 0) return "no hardware random values drawn";
    if (m_words_rdseed != 0) {
        s += "RDSEED (" + std::to_string(m_words_rdseed) + " x 64-bit values)";
    }
    if (m_words_rdrand != 0) {
        if (!s.empty()) s += ", then ";
        s += "RDRAND (" + std::to_string(m_words_rdrand) + " x 64-bit values)";
    }
    return s;
}

std::string hardware_rng::failure_reason() const {
    if (m_state == rng_state::ready) return {};
    std::string s = to_string(m_state);
    s += ". CPU: " + describe_features(m_features) + ".";
    if (m_features.rdseed && m_rdseed_failed) s += " RDSEED marked unavailable.";
    if (m_features.rdrand && m_rdrand_failed) s += " RDRAND marked unavailable.";
    s += " No software or operating-system fallback is used.";
    return s;
}

}  // namespace ftr
