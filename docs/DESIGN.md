# Design

## Layers

```
foobar2000/foo_true_random/   foobar2000-specific (SDK 2026-10-01)
  main.cpp                    DECLARE_COMPONENT_VERSION, VALIDATE_COMPONENT_FILENAME
  commands.cpp                main-menu and context-menu commands, status/reporting
  fb2k_backend.{h,cpp}        playlist_backend implemented on playlist_manager

include/ftr/, src/            portable, no SDK dependency
  random_core.h               rejection sampling + Fisher–Yates (header-only templates)
  hardware_rng.h/.cpp         RDSEED → RDRAND → fail selection, retries, health checks
  hardware_rng_x86.cpp        the only file that executes CPUID/RDSEED/RDRAND
  playlist_logic.h            transactional playlist operations over an abstract backend
```

Everything that decides *what* happens (randomization, validation, rollback,
messages) lives in the portable layer and is unit-tested on Linux and Windows.
The SDK layer only translates calls to `playlist_manager` / `library_manager`.

## Randomization

`make_permutation<uint32_t>(rng, n, order)`:

```
order = [0 .. n-1]
for i = n-1 down to 1:
    j = uniform_below(i + 1)          # exactly uniform in [0, i]
    swap(order[i], order[j])
```

`uniform_below(n)` uses rejection sampling on 32-bit words:

```
threshold = (0 - n) mod n             # = 2^32 mod n, computed in uint32 arithmetic
repeat up to 64 times:
    x = next 32-bit hardware word
    if x >= threshold: return x mod n
fail
```

The accepted range `[threshold, 2^32)` has `2^32 - (2^32 mod n)` values, an
exact multiple of `n`, so `x mod n` is exactly uniform. The per-attempt
rejection probability is below 1/2 for every `n`, so 64 attempts fail only for
a broken source (probability < 2^-64 otherwise), and the operation is aborted.

Lists longer than 2^32 − 1 entries are rejected (`too_many_items`).

The output of the core is an index permutation. The playlist layer applies it
(`new[k] = old[order[k]]`), so duplicates are preserved exactly and nothing
can be lost or invented: `apply_permutation` validates that `order` is a
permutation of `[0, n)` before use.

## Transactions

The complete permutation is computed **before** any playlist is touched.

**New playlist (Entire Media Library, Selected Tracks)**

1. Shuffle in memory. On RNG failure: report, create nothing.
2. Remember the active playlist; `create_playlist(name, SIZE_MAX, SIZE_MAX)`
   (append at end).
3. `playlist_insert_items(...)`, then read the playlist back and compare it
   element-by-element with the shuffled sequence.
4. On any failure: `remove_playlists(mask)` with a `pfc::bit_array_bittable`
   sized to the current playlist count and exactly the new playlist's bit set;
   restore the previously active playlist; report.
5. On success: activate the new playlist; report track count and source.

**Current playlist (in place)**

1. Refuse if there is no active playlist, or a playlist lock filters reordering
   (`playlist_lock_get_filter_mask & playlist_lock::filter_reorder`).
2. Compute the permutation. On RNG failure: report; the playlist is untouched
   and no undo point is created.
3. `playlist_undo_backup(playlist)` then `playlist_reorder_items(playlist,
   order, count)`. Items are moved, never deleted and re-inserted, so
   selection/handles are preserved and Edit → Undo restores the previous order.
4. Read the playlist back and compare with the expected order.

A uniformly random permutation's inverse is also uniformly random, so the
distribution does not depend on whether foobar2000 interprets `order` as
"new position k takes old item order[k]" or its inverse; the read-back check
does depend on it, and is written for the former interpretation, which is the
one `pfc` uses for reorder permutations.

## Threading

All commands run on foobar2000's main thread (menu callbacks), which is what
`playlist_manager` and `library_manager` require. A new `hardware_rng` is
created per operation, so there is no shared mutable RNG state. The CPU
provider is a function-local static whose only state (decoded CPUID features)
is immutable after thread-safe initialization.

## Deliberate non-features

No settings, no preferences page, no software fallback, no weighting, history,
recommendation or "smart" shuffle.
