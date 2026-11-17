// foo-true-random — portable randomization core.
//
// No foobar2000 SDK dependencies. No platform dependencies. No software PRNG.
// All randomness comes from a caller-supplied word source (in production: the
// hardware RNG in hardware_rng.h; in tests: deterministic scripted sources).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace ftr {

enum class core_status {
    ok,
    zero_bound,         // uniform_below(0) is undefined: there is no value in [0, 0).
    rng_failure,        // the word source reported failure; propagated unchanged.
    rejection_limit,    // the bounded rejection-sampling attempt budget was exhausted.
    too_many_items,     // list length does not fit the source's word width.
};

const char* to_string(core_status s) noexcept;

// Maximum rejection-sampling attempts per bounded draw.
//
// For any bound n, the rejection probability of one attempt is
// (2^W mod n) / 2^W < 1/2, so for a healthy uniform source the probability of
// exhausting 64 attempts is below 2^-64. Hitting the limit therefore indicates a
// broken source, and the operation is aborted (fail closed).
inline constexpr unsigned default_rejection_attempts = 64;

// Rejection threshold for a bound n over a W-bit word: (2^W mod n), computed
// without overflow as (0 - n) mod n in W-bit unsigned arithmetic.
//
// Values x >= threshold form a range of size 2^W - (2^W mod n), an exact
// multiple of n, so x mod n is exactly uniform over [0, n) on that range.
template <class Word>
constexpr Word rejection_threshold(Word n) noexcept {
    static_assert(std::is_unsigned_v<Word>);
    // Integer promotion: for Word narrower than int, (Word)0 - n would be a
    // negative int. Cast back to Word to get the modular W-bit result.
    return static_cast<Word>(static_cast<Word>(Word(0) - n) % n);
}

// Draw an exactly uniform integer in [0, n) by rejection sampling.
//
// `source` must be callable as `bool source(Word& out)`: it returns false on
// failure, true with `out` set to a uniformly distributed W-bit word on success.
// Never uses naive modulo reduction without rejection.
template <class Word, class Source>
core_status uniform_below(Source& source, Word n, Word& out,
                          unsigned max_attempts = default_rejection_attempts) {
    static_assert(std::is_unsigned_v<Word>);
    if (n == 0) return core_status::zero_bound;
    const Word threshold = rejection_threshold<Word>(n);
    for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
        Word x{};
        if (!source(x)) return core_status::rng_failure;
        if (x >= threshold) {
            out = static_cast<Word>(x % n);
            return core_status::ok;
        }
    }
    return core_status::rejection_limit;
}

// Produce a uniformly random permutation of size n using Fisher–Yates
// (Durstenfeld, descending form):
//
//   order = [0, 1, ..., n-1]
//   for i = n-1 down to 1:
//       j = uniform integer in [0, i]
//       swap(order[i], order[j])
//
// On success `order` holds the permutation: order[k] is the original index of
// the element that ends up at position k. On failure `order` is left empty and
// the caller must not modify any external state.
//
// The whole permutation is computed before the caller touches any playlist,
// which is what makes the playlist operations transactional.
template <class Word, class Source>
core_status make_permutation(Source& source, std::size_t n, std::vector<std::size_t>& order,
                             unsigned max_attempts = default_rejection_attempts) {
    static_assert(std::is_unsigned_v<Word>);
    order.clear();
    // Bounds are i+1 for i in [1, n-1], so the largest bound is n; it must be
    // representable in Word.
    if (static_cast<std::uintmax_t>(n) >
        static_cast<std::uintmax_t>((std::numeric_limits<Word>::max)())) {
        return core_status::too_many_items;
    }
    std::vector<std::size_t> work(n);
    for (std::size_t k = 0; k < n; ++k) work[k] = k;
    for (std::size_t i = n; i-- > 1;) {
        Word j{};
        const core_status st =
            uniform_below<Word>(source, static_cast<Word>(i + 1), j, max_attempts);
        if (st != core_status::ok) return st;  // `order` stays empty.
        const std::size_t jj = static_cast<std::size_t>(j);
        if (jj != i) {
            const std::size_t tmp = work[i];
            work[i] = work[jj];
            work[jj] = tmp;
        }
    }
    order.swap(work);
    return core_status::ok;
}

// Apply a permutation produced by make_permutation: out[k] = in[order[k]].
// Returns false (and leaves `out` untouched) if `order` is not a permutation of
// [0, in.size()).
template <class T>
bool apply_permutation(const std::vector<T>& in, const std::vector<std::size_t>& order,
                       std::vector<T>& out) {
    if (order.size() != in.size()) return false;
    std::vector<unsigned char> seen(in.size(), 0);
    for (std::size_t idx : order) {
        if (idx >= in.size() || seen[idx]) return false;
        seen[idx] = 1;
    }
    std::vector<T> tmp;
    tmp.reserve(in.size());
    for (std::size_t idx : order) tmp.push_back(in[idx]);
    out.swap(tmp);
    return true;
}

// Validity check usable by both the core and the playlist layer.
bool is_permutation_of_iota(const std::vector<std::size_t>& order) noexcept;

}  // namespace ftr
