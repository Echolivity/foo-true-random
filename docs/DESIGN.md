# Design

## Layers

```
foobar2000/foo_true_random/   foobar2000-specific (SDK 2026-10-01)
  main.cpp                    DECLARE_COMPONENT_VERSION, VALIDATE_COMPONENT_FILENAME
  commands.cpp                main-menu and context-menu commands, status/reporting
  fb2k_backend.{h,cpp}        playlist_backend implemented on playlist_manager

include/ftr/, src/            portable, no SDK dependency
  random_core.h               rejection sampling + Fisher–Yates (header-only templates)
  hardware_rng.h/.cpp         RDSEED → RDRAND → fail selection, retries, health checks;
                              the Windows RNG only on CPUs with neither instruction
  hardware_rng_x86.cpp        the only file that executes CPUID/RDSEED/RDRAND
  system_rng.cpp              the only file that calls the OS generator (BCryptGenRandom)
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
2. Pick a unique name: `True Random - Library`, or `… (2)`, `… (3)` if taken
   (ASCII case-insensitive comparison). Existing playlists are never reused.
3. Remember the active playlist; `create_playlist(name, SIZE_MAX, SIZE_MAX)`
   (append at end). A `created_playlist_guard` now owns the new playlist.
4. `playlist_insert_items(...)`, then read the playlist back and compare it
   element-by-element with the shuffled sequence.
5. On failure, **or if an exception escapes** (the guard's destructor), roll
   back: `remove_playlists(mask)` with a `pfc::bit_array_bittable` sized to the
   current count and only the new playlist's bit set, then restore the
   previously active playlist. Before removing, the guard confirms that the
   playlist count, the index and the stored name are unchanged since creation;
   otherwise it removes nothing and the user is asked to delete the leftover
   playlist manually. Deleting the wrong playlist is worse than leaving one.
6. On success: commit, activate the new playlist, verify it became active
   (reported if not), report track count, playlist name and RNG source.

**Not atomic.** foobar2000 has no playlist transaction API, so this is
compensating rollback. Other components receive callbacks for the creation
and the removal. The guarantee is that no partial playlist *remains* and no
existing playlist is touched.

**Current playlist (in place)**

1. Refuse if there is no active playlist, or a playlist lock filters reordering
   (`playlist_lock_get_filter_mask & playlist_lock::filter_reorder`).
2. Compute the permutation. On RNG failure: report; the playlist is untouched
   and no undo point is created.
3. `playlist_undo_backup(playlist)` then `playlist_reorder_items(playlist,
   order, count)`. Items are moved, never deleted and re-inserted, so
   selection/handles are preserved and Edit → Undo restores the previous order.
4. Read the playlist back and compare with the expected order.

`playlist_reorder_items` places old item `order[k]` at new position `k`. This
is verified inside real foobar2000 by the integration test. (A uniformly random
permutation's inverse is also uniformly random, so the distribution would not
depend on it anyway; the read-back check does.)

If foobar2000 refuses the reorder after the undo point was created, the undo
stack holds a restore point identical to the current order. Undoing it is
harmless.

**Why Current Playlist reorders in place.** The other two commands create a
new playlist. Current Playlist deliberately reorders the playlist you are
looking at, so playlist-specific settings and its tab stay put, and Edit → Undo
restores it exactly. Making a shuffled copy of a playlist is already covered:
select all (Ctrl+A), then use *Selected Tracks*.

## Threading

All commands run on foobar2000's main thread (menu callbacks), which is what
`playlist_manager` and `library_manager` require. A new `hardware_rng` is
created per operation, so there is no shared mutable RNG state. The CPU
provider is a function-local static whose only state (decoded CPUID features)
is immutable after thread-safe initialization.

## Deliberate non-features

No settings, no preferences page, no software PRNG, no fallback for failing
hardware (the Windows RNG only replaces instructions a CPU does not have), no weighting, history,
recommendation or "smart" shuffle. Playback is never started or changed by the
component: after a shuffle, playback order is whatever foobar2000's
*Playback → Order* says (use *Default* to hear the shuffled order as is).
