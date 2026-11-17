// Hardware RNG tests with mock instruction providers (deterministic; no real
// RDSEED/RDRAND is executed by this file).
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string>
#include <vector>

#include "ftr/hardware_rng.h"
#include "ftr/random_core.h"
#include "mock_provider.h"
#include "test_harness.h"

using namespace ftr;
using namespace ftr_test;

namespace {
constexpr std::uint32_t bit(unsigned b) { return 1u << b; }
}

// ------------------------------------------------------------ CPUID decode

TEST(cpuid_both_supported) {
    cpuid_snapshot s{0x10, bit(30), bit(18)};
    const auto f = decode_cpu_features(s);
    CHECK(f.rdseed);
    CHECK(f.rdrand);
}

TEST(cpuid_sandy_bridge_neither) {
    // Sandy Bridge: max basic leaf 0xD, no RDRAND (ECX[30]=0), no RDSEED (EBX[18]=0).
    // Other bits set to make sure only the exact bits are consulted.
    cpuid_snapshot s{0xD, 0x1FBAE3FFu & ~bit(30), 0x00000281u & ~bit(18)};
    const auto f = decode_cpu_features(s);
    CHECK(!f.rdseed);
    CHECK(!f.rdrand);
}

TEST(cpuid_ivy_bridge_rdrand_only) {
    cpuid_snapshot s{0xD, bit(30) | bit(25), 0x00000281u};
    const auto f = decode_cpu_features(s);
    CHECK(!f.rdseed);
    CHECK(f.rdrand);
}

TEST(cpuid_bits_not_confused) {
    // RDSEED bit placed in leaf 1 ECX and RDRAND bit placed in leaf 7 EBX
    // must NOT enable anything.
    cpuid_snapshot s{0x10, bit(18), bit(30)};
    const auto f = decode_cpu_features(s);
    CHECK(!f.rdseed);
    CHECK(!f.rdrand);
}

TEST(cpuid_leaf7_ignored_when_not_reported) {
    // A CPU whose max basic leaf is < 7 has no meaningful leaf-7 data.
    cpuid_snapshot s{6, bit(30), bit(18)};
    const auto f = decode_cpu_features(s);
    CHECK(!f.rdseed);
    CHECK(f.rdrand);
    cpuid_snapshot z{0, bit(30), bit(18)};
    const auto g = decode_cpu_features(z);
    CHECK(!g.rdseed);
    CHECK(!g.rdrand);
}

// -------------------------------------------------------- source selection

TEST(both_available_prefers_rdseed) {
    mock_provider m;
    m.feats = {true, true};
    m.rdseed = always_ok(1);
    m.rdrand = always_ok(2);
    hardware_rng rng(m);
    CHECK(rng.usable());
    CHECK(rng.current_source() == rng_source::rdseed);
    CHECK(rng.last_source() == rng_source::none);  // nothing handed out yet
    std::uint32_t v = 0;
    for (int k = 0; k < 100; ++k) CHECK(rng.next_u32(v));
    CHECK(rng.last_source() == rng_source::rdseed);
    CHECK_EQ(rng.words_from_rdseed(), std::uint64_t(50));  // two u32 per 64-bit word
    CHECK_EQ(rng.words_from_rdrand(), std::uint64_t(0));
    // RDRAND only ran for its construction self-test.
    CHECK_EQ(m.rdrand_calls, std::uint64_t(rng_policy{}.self_test_words));
    CHECK_EQ(m.illegal_rdseed + m.illegal_rdrand, std::uint64_t(0));
}

TEST(u32_halves_come_from_one_word) {
    mock_provider m;
    m.feats = {true, false};
    m.rdseed = [](std::uint64_t i, std::uint64_t& out) {
        out = (i < 8) ? distinct_value(i, 7) : 0x1122334455667788ull + i;
        return true;
    };
    hardware_rng rng(m);
    std::uint32_t lo = 0, hi = 0;
    CHECK(rng.next_u32(lo));
    CHECK(rng.next_u32(hi));
    CHECK_EQ(lo, 0x55667788u + 8u);
    CHECK_EQ(hi, 0x11223344u);
}

TEST(rdseed_unsupported_uses_rdrand_without_executing_rdseed) {
    mock_provider m;
    m.feats = {false, true};
    m.rdrand = always_ok(3);
    hardware_rng rng(m);
    CHECK(rng.usable());
    CHECK(rng.current_source() == rng_source::rdrand);
    std::uint32_t v = 0;
    CHECK(rng.next_u32(v));
    CHECK(rng.last_source() == rng_source::rdrand);
    CHECK_EQ(m.rdseed_calls, std::uint64_t(0));
    CHECK_EQ(m.illegal_rdseed, std::uint64_t(0));
}

TEST(rdrand_only_unsupported_uses_rdseed_without_executing_rdrand) {
    mock_provider m;
    m.feats = {true, false};
    m.rdseed = always_ok(4);
    hardware_rng rng(m);
    std::uint32_t v = 0;
    CHECK(rng.next_u32(v));
    CHECK(rng.last_source() == rng_source::rdseed);
    CHECK_EQ(m.rdrand_calls, std::uint64_t(0));
    CHECK_EQ(m.illegal_rdrand, std::uint64_t(0));
}

TEST(neither_available_fails_closed_executes_nothing) {
    mock_provider m;
    m.feats = {false, false};
    hardware_rng rng(m);
    CHECK(!rng.usable());
    CHECK(rng.state() == rng_state::no_hardware);
    CHECK(rng.current_source() == rng_source::none);
    std::uint32_t v = 12345;
    CHECK(!rng.next_u32(v));
    CHECK(!rng.next_u32(v));
    CHECK_EQ(m.rdseed_calls + m.rdrand_calls, std::uint64_t(0));
    CHECK_EQ(m.illegal_rdseed + m.illegal_rdrand, std::uint64_t(0));
    CHECK(rng.failure_reason().find("neither RDSEED nor RDRAND") != std::string::npos);
    // And the shuffle aborts.
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(rng, 10, order) == core_status::rng_failure);
    CHECK(order.empty());
}

TEST(rdseed_transient_failures_are_retried_with_backoff) {
    mock_provider m;
    m.feats = {true, false};
    // Every call fails except every 5th.
    m.rdseed = [](std::uint64_t i, std::uint64_t& out) {
        if (i % 5 != 4) { out = 0; return false; }
        out = distinct_value(i, 9);
        return true;
    };
    hardware_rng rng(m);
    CHECK(rng.usable());
    std::uint64_t w = 0;
    CHECK(rng.next_u64(w));
    CHECK(rng.last_source() == rng_source::rdseed);
    CHECK(m.backoffs > 0);
}

TEST(rdseed_retry_exhaustion_switches_to_rdrand) {
    mock_provider m;
    m.feats = {true, true};
    rng_policy pol;
    pol.rdseed_attempts = 16;
    // RDSEED passes self-test (8 words) plus 3 production words, then fails forever.
    m.rdseed = ok_then_fail(pol.self_test_words + 3, 5);
    m.rdrand = always_ok(6);
    hardware_rng rng(m, pol);
    std::uint64_t w = 0;
    for (int k = 0; k < 3; ++k) {
        CHECK(rng.next_u64(w));
        CHECK(rng.last_source() == rng_source::rdseed);
    }
    const std::uint64_t seed_calls_before = m.rdseed_calls;
    CHECK(rng.next_u64(w));  // RDSEED exhausts 16 attempts, RDRAND supplies
    CHECK_EQ(m.rdseed_calls - seed_calls_before, std::uint64_t(16));
    CHECK(rng.last_source() == rng_source::rdrand);
    CHECK(rng.rdseed_failed());
    CHECK(rng.current_source() == rng_source::rdrand);
    // RDSEED is not retried again for this instance.
    const std::uint64_t seed_calls_after = m.rdseed_calls;
    for (int k = 0; k < 10; ++k) CHECK(rng.next_u64(w));
    CHECK_EQ(m.rdseed_calls, seed_calls_after);
    CHECK(rng.usage_summary().find("RDSEED (3") != std::string::npos);
    CHECK(rng.usage_summary().find("then RDRAND (11") != std::string::npos);
}

TEST(rdseed_failing_self_test_never_supplies_values) {
    mock_provider m;
    m.feats = {true, true};
    m.rdseed = always_fail();
    m.rdrand = always_ok(8);
    hardware_rng rng(m);
    CHECK(rng.usable());
    CHECK(rng.rdseed_failed());
    CHECK(rng.current_source() == rng_source::rdrand);
    CHECK_EQ(m.rdseed_calls, std::uint64_t(rng_policy{}.rdseed_attempts));  // one bounded attempt sequence
}

TEST(rdrand_retry_exhaustion_fails_closed) {
    mock_provider m;
    m.feats = {false, true};
    rng_policy pol;
    m.rdrand = ok_then_fail(pol.self_test_words + 2, 10);
    hardware_rng rng(m, pol);
    std::uint64_t w = 0;
    CHECK(rng.next_u64(w));
    CHECK(rng.next_u64(w));
    const std::uint64_t before = m.rdrand_calls;
    CHECK(!rng.next_u64(w));
    CHECK_EQ(m.rdrand_calls - before, std::uint64_t(pol.rdrand_attempts));  // bounded, exactly 10
    CHECK(rng.state() == rng_state::sources_exhausted);
    CHECK(rng.rdrand_failed());
    CHECK(rng.current_source() == rng_source::none);
    const std::uint64_t after = m.rdrand_calls;
    CHECK(!rng.next_u64(w));
    CHECK_EQ(m.rdrand_calls, after);  // no further instruction executions
}

TEST(both_exhaust_fails_closed) {
    mock_provider m;
    m.feats = {true, true};
    rng_policy pol;
    pol.rdseed_attempts = 4;
    m.rdseed = ok_then_fail(pol.self_test_words + 1, 11);
    m.rdrand = ok_then_fail(pol.self_test_words + 1, 12);
    hardware_rng rng(m, pol);
    std::uint64_t w = 0;
    CHECK(rng.next_u64(w));   // RDSEED
    CHECK(rng.last_source() == rng_source::rdseed);
    CHECK(rng.next_u64(w));   // RDSEED exhausted -> RDRAND
    CHECK(rng.last_source() == rng_source::rdrand);
    CHECK(!rng.next_u64(w));  // both exhausted
    CHECK(rng.state() == rng_state::sources_exhausted);
    CHECK(rng.failure_reason().find("No software or operating-system fallback") != std::string::npos);
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(rng, 5, order) == core_status::rng_failure);
    CHECK(order.empty());
}

TEST(buffered_half_not_released_after_failure) {
    mock_provider m;
    m.feats = {false, true};
    rng_policy pol;
    m.rdrand = ok_then_fail(pol.self_test_words + 1, 13);
    hardware_rng rng(m, pol);
    std::uint32_t v = 0;
    CHECK(rng.next_u32(v));   // low half of word 1
    CHECK(rng.next_u32(v));   // buffered high half of word 1
    CHECK(!rng.next_u32(v));  // word 2 fails
    CHECK(!rng.next_u32(v));
}

TEST(stuck_source_detected_in_self_test_is_skipped) {
    mock_provider m;
    m.feats = {true, true};
    m.rdseed = stuck(0);  // e.g. a faulty RDSEED returning 0 with CF=1
    m.rdrand = always_ok(14);
    hardware_rng rng(m);
    CHECK(rng.usable());
    CHECK(rng.rdseed_failed());
    std::uint32_t v = 0;
    CHECK(rng.next_u32(v));
    CHECK(rng.last_source() == rng_source::rdrand);
    CHECK_EQ(rng.words_from_rdseed(), std::uint64_t(0));
}

TEST(stuck_rdrand_all_ones_detected) {
    mock_provider m;
    m.feats = {false, true};
    m.rdrand = stuck(~0ull);  // known failure mode: RDRAND returning all ones with CF=1
    hardware_rng rng(m);
    CHECK(!rng.usable());
    CHECK(rng.state() == rng_state::sources_exhausted);
}

TEST(stuck_at_runtime_aborts_without_switching) {
    mock_provider m;
    m.feats = {true, true};
    rng_policy pol;
    m.rdseed = [&pol](std::uint64_t i, std::uint64_t& out) {
        out = (i < pol.self_test_words + 2) ? distinct_value(i, 15) : 0xDEADBEEFull;
        return true;
    };
    m.rdrand = always_ok(16);
    hardware_rng rng(m, pol);
    std::uint64_t w = 0;
    CHECK(rng.next_u64(w));
    CHECK(rng.next_u64(w));
    CHECK(rng.next_u64(w));   // first 0xDEADBEEF: not yet a repeat
    CHECK(!rng.next_u64(w));  // second identical value: fault
    CHECK(rng.state() == rng_state::stuck_output);
    const std::uint64_t rd = m.rdrand_calls;
    CHECK(!rng.next_u64(w));  // stays failed; no switch to RDRAND mid-operation
    CHECK_EQ(m.rdrand_calls, rd);
}

TEST(shuffle_through_hardware_rng_with_mock) {
    mock_provider m;
    m.feats = {true, true};
    m.rdseed = always_ok(17);
    m.rdrand = always_ok(18);
    hardware_rng rng(m);
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(rng, 1000, order) == core_status::ok);
    CHECK(is_permutation_of_iota(order));
    CHECK(rng.last_source() == rng_source::rdseed);
    CHECK_EQ(m.illegal_rdseed + m.illegal_rdrand, std::uint64_t(0));
}

TEST_MAIN("hardware_rng")
