// Portable core tests: rejection sampling and Fisher–Yates.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "ftr/random_core.h"
#include "test_harness.h"

using namespace ftr;

namespace {

// Scripted word source: returns the listed values in order, then fails.
// Can also be told to fail at a given call index.
template <class Word>
struct scripted {
    std::vector<Word> values;
    std::size_t pos = 0;
    std::size_t fail_at = static_cast<std::size_t>(-1);
    std::size_t calls = 0;
    bool operator()(Word& out) {
        ++calls;
        if (calls - 1 == fail_at || pos >= values.size()) return false;
        out = values[pos++];
        return true;
    }
};

std::uint64_t ref_threshold32(std::uint32_t n) { return (std::uint64_t(1) << 32) % n; }

}  // namespace

// ---------------------------------------------------------------- threshold

TEST(threshold_known_values_32bit) {
    CHECK_EQ(rejection_threshold<std::uint32_t>(1), 0u);
    CHECK_EQ(rejection_threshold<std::uint32_t>(2), 0u);
    CHECK_EQ(rejection_threshold<std::uint32_t>(3), 1u);   // 2^32 = 3*1431655765 + 1
    CHECK_EQ(rejection_threshold<std::uint32_t>(5), 1u);
    CHECK_EQ(rejection_threshold<std::uint32_t>(6), 4u);
    CHECK_EQ(rejection_threshold<std::uint32_t>(7), 4u);
    CHECK_EQ(rejection_threshold<std::uint32_t>(1u << 31), 0u);
    CHECK_EQ(rejection_threshold<std::uint32_t>((1u << 31) + 1), (1u << 31) - 1);
    CHECK_EQ(rejection_threshold<std::uint32_t>(0xFFFFFFFFu), 1u);
}

TEST(threshold_matches_64bit_reference) {
    std::vector<std::uint32_t> ns;
    for (std::uint32_t n = 1; n <= 5000; ++n) ns.push_back(n);
    for (std::uint32_t k = 0; k < 32; ++k) {
        const std::uint32_t p = 1u << k;
        ns.push_back(p);
        if (p > 1) ns.push_back(p - 1);
        if (p < 0x80000000u) ns.push_back(p + 1);
    }
    ns.push_back(0xFFFFFFFFu);
    ns.push_back(0xFFFFFFFEu);
    ns.push_back(3000000000u);
    for (auto n : ns) CHECK_EQ(std::uint64_t(rejection_threshold<std::uint32_t>(n)), ref_threshold32(n));
}

TEST(threshold_narrow_types_no_promotion_bug) {
    // For uint8_t, (0 - n) is computed in int; the implementation must cast back.
    for (unsigned n = 1; n <= 255; ++n)
        CHECK_EQ(unsigned(rejection_threshold<std::uint8_t>(std::uint8_t(n))), 256u % n);
    for (unsigned n = 1; n <= 65535; n += 7)
        CHECK_EQ(unsigned(rejection_threshold<std::uint16_t>(std::uint16_t(n))), 65536u % n);
}

// ---------------------------------------------------------- uniform_below

TEST(rejects_values_below_threshold) {
    // n = 3 -> threshold 1. Value 0 must be rejected; next value 5 -> 5 % 3 = 2.
    scripted<std::uint32_t> src{{0u, 5u}};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 3u, out) == core_status::ok);
    CHECK_EQ(out, 2u);
    CHECK_EQ(src.calls, std::size_t(2));
}

TEST(accepts_value_equal_to_threshold) {
    scripted<std::uint32_t> src{{1u}};  // threshold for 3 is 1
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 3u, out) == core_status::ok);
    CHECK_EQ(out, 1u);
    CHECK_EQ(src.calls, std::size_t(1));
}

TEST(accepts_value_above_threshold_and_max) {
    scripted<std::uint32_t> src{{0xFFFFFFFFu}};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 7u, out) == core_status::ok);
    CHECK_EQ(out, 0xFFFFFFFFu % 7u);
}

TEST(large_bound_rejection_region) {
    // n = 2^31 + 1: threshold = 2^31 - 1. Everything below is rejected.
    const std::uint32_t n = (1u << 31) + 1;
    scripted<std::uint32_t> src{{0u, (1u << 31) - 2, (1u << 31) - 1}};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, n, out) == core_status::ok);
    CHECK_EQ(src.calls, std::size_t(3));
    CHECK_EQ(out, ((1u << 31) - 1) % n);
}

TEST(bound_one_always_zero) {
    for (std::uint32_t x : {0u, 1u, 12345u, 0xFFFFFFFFu}) {
        scripted<std::uint32_t> src{{x}};
        std::uint32_t out = 99;
        CHECK(uniform_below<std::uint32_t>(src, 1u, out) == core_status::ok);
        CHECK_EQ(out, 0u);
    }
}

TEST(zero_bound_fails_without_consuming) {
    scripted<std::uint32_t> src{{1u, 2u}};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 0u, out) == core_status::zero_bound);
    CHECK_EQ(src.calls, std::size_t(0));
    CHECK_EQ(out, 99u);
}

TEST(retry_exhaustion_is_bounded) {
    // All zeros with n = 3 are always rejected.
    scripted<std::uint32_t> src{std::vector<std::uint32_t>(1000, 0u)};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 3u, out, 10) == core_status::rejection_limit);
    CHECK_EQ(src.calls, std::size_t(10));
    CHECK_EQ(out, 99u);
    scripted<std::uint32_t> src2{std::vector<std::uint32_t>(1000, 0u)};
    CHECK(uniform_below<std::uint32_t>(src2, 3u, out) == core_status::rejection_limit);
    CHECK_EQ(src2.calls, std::size_t(default_rejection_attempts));
}

TEST(rng_failure_propagates) {
    scripted<std::uint32_t> src{{}};
    std::uint32_t out = 99;
    CHECK(uniform_below<std::uint32_t>(src, 10u, out) == core_status::rng_failure);
    CHECK_EQ(out, 99u);
    // Failure after a rejection also propagates.
    scripted<std::uint32_t> src2{{0u}};
    CHECK(uniform_below<std::uint32_t>(src2, 3u, out) == core_status::rng_failure);
}

// Exhaustive proof of exact uniformity of the reduction for narrow words:
// over every possible input word, accepted values map to each residue equally
// often, and the accepted count is a multiple of n.
TEST(exhaustive_uniformity_8bit_all_bounds) {
    for (unsigned n = 1; n <= 255; ++n) {
        std::vector<unsigned> counts(n, 0);
        unsigned accepted = 0;
        for (unsigned x = 0; x <= 255; ++x) {
            scripted<std::uint8_t> src{{std::uint8_t(x)}};
            std::uint8_t out = 0;
            const core_status st = uniform_below<std::uint8_t>(src, std::uint8_t(n), out, 1);
            if (st == core_status::ok) {
                CHECK(out < n);
                ++counts[out];
                ++accepted;
            } else {
                CHECK(st == core_status::rejection_limit);
            }
        }
        CHECK_EQ(accepted, 256u - 256u % n);
        for (unsigned c : counts) CHECK_EQ(c, 256u / n);
    }
}

TEST(exhaustive_uniformity_16bit_selected_bounds) {
    std::vector<unsigned> ns;
    for (unsigned n = 1; n <= 64; ++n) ns.push_back(n);
    for (unsigned n : {100u, 255u, 256u, 257u, 1000u, 4095u, 32767u, 32768u, 32769u, 40000u, 65535u}) ns.push_back(n);
    for (unsigned n : ns) {
        std::vector<unsigned> counts(n, 0);
        unsigned accepted = 0;
        const std::uint16_t thr = rejection_threshold<std::uint16_t>(std::uint16_t(n));
        for (unsigned x = 0; x <= 65535; ++x) {
            if (x < thr) continue;  // rejected (verified separately below)
            ++accepted;
            ++counts[x % n];
        }
        // Verify the rejection decision against uniform_below itself on the boundary.
        if (thr > 0) {
            scripted<std::uint16_t> lo{{std::uint16_t(thr - 1)}};
            std::uint16_t o = 0;
            CHECK(uniform_below<std::uint16_t>(lo, std::uint16_t(n), o, 1) == core_status::rejection_limit);
        }
        scripted<std::uint16_t> at{{thr}};
        std::uint16_t o = 0;
        CHECK(uniform_below<std::uint16_t>(at, std::uint16_t(n), o, 1) == core_status::ok);
        CHECK_EQ(accepted % n, 0u);
        for (unsigned c : counts) CHECK_EQ(c, accepted / n);
    }
}

// ------------------------------------------------------------- Fisher–Yates

namespace {

// Word sequence that makes uniform_below(bound) return exactly `j`:
// x = bound + j is >= threshold (threshold < bound) and x % bound == j.
std::vector<std::uint32_t> choices_to_words(std::size_t n, const std::vector<std::size_t>& js) {
    std::vector<std::uint32_t> w;
    std::size_t k = 0;
    for (std::size_t i = n; i-- > 1;) {
        const std::uint32_t bound = std::uint32_t(i + 1);
        w.push_back(bound + std::uint32_t(js[k++]));
    }
    return w;
}

}  // namespace

TEST(fy_empty_list_consumes_nothing) {
    scripted<std::uint32_t> src{{}};
    std::vector<std::size_t> order{7, 7};
    CHECK(make_permutation<std::uint32_t>(src, 0, order) == core_status::ok);
    CHECK(order.empty());
    CHECK_EQ(src.calls, std::size_t(0));
}

TEST(fy_single_element_consumes_nothing) {
    scripted<std::uint32_t> src{{}};
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(src, 1, order) == core_status::ok);
    CHECK(order == std::vector<std::size_t>{0});
    CHECK_EQ(src.calls, std::size_t(0));
}

TEST(fy_two_elements_both_outcomes) {
    {
        scripted<std::uint32_t> src{{2u}};  // 2 % 2 = 0 -> swap(1, 0)
        std::vector<std::size_t> order;
        CHECK(make_permutation<std::uint32_t>(src, 2, order) == core_status::ok);
        CHECK((order == std::vector<std::size_t>{1, 0}));
    }
    {
        scripted<std::uint32_t> src{{3u}};  // 3 % 2 = 1 -> no swap
        std::vector<std::size_t> order;
        CHECK(make_permutation<std::uint32_t>(src, 2, order) == core_status::ok);
        CHECK((order == std::vector<std::size_t>{0, 1}));
    }
}

TEST(fy_bounds_requested_are_i_plus_one_descending) {
    // Record the bound for each draw by using a source that remembers the
    // largest value it is allowed to return; we infer bounds by feeding the
    // maximum word and checking results stay in range.
    const std::size_t n = 50;
    std::vector<std::size_t> js;
    for (std::size_t i = n; i-- > 1;) js.push_back(i);  // always choose j = i (identity)
    scripted<std::uint32_t> src{choices_to_words(n, js)};
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(src, n, order) == core_status::ok);
    CHECK_EQ(src.calls, n - 1);
    for (std::size_t k = 0; k < n; ++k) CHECK_EQ(order[k], k);
}

TEST(fy_exhaustive_choice_sequences_n3_gives_six_distinct) {
    const std::size_t n = 3;
    std::map<std::vector<std::size_t>, int> seen;
    int sequences = 0;
    for (std::size_t j2 = 0; j2 <= 2; ++j2)
        for (std::size_t j1 = 0; j1 <= 1; ++j1) {
            scripted<std::uint32_t> src{choices_to_words(n, {j2, j1})};
            std::vector<std::size_t> order;
            CHECK(make_permutation<std::uint32_t>(src, n, order) == core_status::ok);
            CHECK(is_permutation_of_iota(order));
            ++seen[order];
            ++sequences;
        }
    CHECK_EQ(sequences, 6);
    CHECK_EQ(seen.size(), std::size_t(6));
    for (auto& kv : seen) CHECK_EQ(kv.second, 1);
    // Every permutation of {0,1,2} is present.
    std::vector<std::size_t> p{0, 1, 2};
    do { CHECK(seen.count(p) == 1); } while (std::next_permutation(p.begin(), p.end()));
}

TEST(fy_exhaustive_choice_sequences_n1_to_n7_bijective) {
    for (std::size_t n = 1; n <= 7; ++n) {
        std::size_t total = 1;
        for (std::size_t k = 2; k <= n; ++k) total *= k;
        std::set<std::vector<std::size_t>> seen;
        // Mixed-radix enumeration of (j_{n-1}, ..., j_1) with j_i in [0, i].
        std::vector<std::size_t> js(n > 0 ? n - 1 : 0, 0);
        for (std::size_t c = 0; c < total; ++c) {
            std::size_t rem = c;
            for (std::size_t k = 0; k < js.size(); ++k) {
                const std::size_t radix = n - k;  // bound for i = n-1-k is n-k
                js[k] = rem % radix;
                rem /= radix;
            }
            scripted<std::uint32_t> src{choices_to_words(n, js)};
            std::vector<std::size_t> order;
            CHECK(make_permutation<std::uint32_t>(src, n, order) == core_status::ok);
            CHECK(is_permutation_of_iota(order));
            seen.insert(order);
        }
        // total choice sequences -> total distinct permutations => bijection,
        // so each permutation has exactly one choice sequence.
        CHECK_EQ(seen.size(), total);
    }
}

// End-to-end exhaustive uniformity over the whole pipeline (rejection sampling
// + Fisher–Yates) at 8-bit word width: enumerate every accepted raw word
// sequence and verify each permutation is produced equally often.
TEST(fy_end_to_end_exhaustive_8bit_n3_n4) {
    for (std::size_t n : {std::size_t(3), std::size_t(4)}) {
        std::vector<std::uint8_t> bounds;  // bound per draw, in draw order
        for (std::size_t i = n; i-- > 1;) bounds.push_back(std::uint8_t(i + 1));
        std::map<std::vector<std::size_t>, std::uint64_t> counts;
        std::vector<unsigned> word(bounds.size(), 0);
        std::uint64_t sequences = 0;
        for (;;) {
            bool all_accepted = true;
            for (std::size_t k = 0; k < bounds.size(); ++k)
                if (word[k] < rejection_threshold<std::uint8_t>(bounds[k])) all_accepted = false;
            if (all_accepted) {
                scripted<std::uint8_t> src;
                for (unsigned w : word) src.values.push_back(std::uint8_t(w));
                std::vector<std::size_t> order;
                CHECK(make_permutation<std::uint8_t>(src, n, order, 1) == core_status::ok);
                ++counts[order];
                ++sequences;
            }
            std::size_t k = 0;
            while (k < word.size() && ++word[k] == 256) word[k++] = 0;
            if (k == word.size()) break;
        }
        std::size_t fact = 1;
        for (std::size_t k = 2; k <= n; ++k) fact *= k;
        CHECK_EQ(counts.size(), fact);
        for (auto& kv : counts) CHECK_EQ(kv.second, sequences / fact);
        CHECK_EQ(sequences % fact, std::uint64_t(0));
    }
}

TEST(fy_duplicates_preserved_and_nothing_lost) {
    const std::vector<int> items{5, 5, 5, 1, 2, 2, 9, 9, 9, 9, 0, 7};
    std::vector<std::uint32_t> words;
    for (std::uint32_t k = 0; k < 64; ++k) words.push_back(0x9E3779B9u * (k + 1));
    scripted<std::uint32_t> src{words};
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(src, items.size(), order) == core_status::ok);
    CHECK(is_permutation_of_iota(order));
    std::vector<int> out;
    CHECK(apply_permutation(items, order, out));
    CHECK_EQ(out.size(), items.size());
    auto a = items, b = out;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b);
}

TEST(fy_large_list_indices_in_range) {
    const std::size_t n = 100000;
    std::vector<std::uint32_t> words;
    words.reserve(n);
    std::uint32_t x = 1;
    for (std::size_t k = 0; k < n; ++k) {  // deterministic test data, not production
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        words.push_back(x);
    }
    scripted<std::uint32_t> src{words};
    std::vector<std::size_t> order;
    const core_status st = make_permutation<std::uint32_t>(src, n, order);
    CHECK(st == core_status::ok);
    CHECK_EQ(order.size(), n);
    CHECK(is_permutation_of_iota(order));
}

TEST(fy_rng_failure_midway_leaves_output_empty) {
    scripted<std::uint32_t> src{{10u, 11u, 12u}};  // only 3 words for 9 draws
    std::vector<std::size_t> order{1, 2, 3};
    CHECK(make_permutation<std::uint32_t>(src, 10, order) == core_status::rng_failure);
    CHECK(order.empty());
}

TEST(fy_rejection_limit_midway_leaves_output_empty) {
    // n = 3: first draw bound 3 (threshold 1); feed only zeros -> rejection limit.
    scripted<std::uint32_t> src{std::vector<std::uint32_t>(500, 0u)};
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint32_t>(src, 3, order, 8) == core_status::rejection_limit);
    CHECK(order.empty());
}

TEST(fy_too_many_items_for_word) {
    scripted<std::uint8_t> src{std::vector<std::uint8_t>(1000, 200)};
    std::vector<std::size_t> order;
    CHECK(make_permutation<std::uint8_t>(src, 256, order) == core_status::too_many_items);
    CHECK(order.empty());
    CHECK_EQ(src.calls, std::size_t(0));
    scripted<std::uint8_t> src2{std::vector<std::uint8_t>(1000, 255)};
    CHECK(make_permutation<std::uint8_t>(src2, 255, order) == core_status::ok);
    CHECK(is_permutation_of_iota(order));
}

TEST(apply_permutation_rejects_invalid_orders) {
    const std::vector<int> in{1, 2, 3};
    std::vector<int> out{42};
    CHECK(!apply_permutation(in, {0, 1}, out));
    CHECK(!apply_permutation(in, {0, 1, 1}, out));
    CHECK(!apply_permutation(in, {0, 1, 3}, out));
    CHECK((out == std::vector<int>{42}));
    CHECK(apply_permutation(in, {2, 0, 1}, out));
    CHECK((out == std::vector<int>{3, 1, 2}));
}

TEST_MAIN("core")
