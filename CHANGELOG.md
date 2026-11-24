# Changelog

## 1.1.0 — 2026-11-24

Works on CPUs without a hardware random number generator.

- CPUs with neither RDSEED nor RDRAND (Intel Sandy Bridge, 2nd gen Core, and
  older) now use the Windows system random number generator
  (`BCryptGenRandom`) instead of refusing every command. Previously the
  component could not be used on these CPUs at all.
- CPUs with RDSEED or RDRAND behave exactly as before: RDSEED, then RDRAND,
  and a hardware failure is still refused. The Windows generator is never
  used as a fallback for failing hardware.
- *Hardware RNG Status*, the console messages and the component description
  say which source was used.
- The same health checks apply to the Windows generator (discarded self-test
  words, rejection of identical consecutive values).
- Tests: mock tests for every CPU combination, a real `BCryptGenRandom` test,
  and an integration test inside foobar2000 that shuffles with a no-hardware
  CPU model and the real Windows generator.

## 1.0.2 — 2026-11-18

Engineering audit fixes. No change to randomness or shuffling.
Identical code to release candidate 1.0.2-rc1 apart from the version label.

Manually verified on 1.0.2-rc1 (Intel Core i7-13620H, Windows 11 25H2,
foobar2000 2.26.3 x64): install; Entire Media Library with an empty library
(refused, nothing changed) and with a real 23-track library (23 tracks,
RDSEED); Current Playlist followed by Ctrl+Z (exact restore); Selected Tracks
twice ("True Random - Selection" and "(2)").

- Rollback now also runs if an error (exception) occurs after the new playlist
  was created, so no partially filled playlist is left behind.
- Rollback only removes a playlist after confirming it is still the one this
  component created; if another component changed the playlist list meanwhile,
  nothing is removed and the user is told.
- New playlists get a unique name: "True Random - Library (2)", "(3)", ...
  instead of several tabs with an identical name. Existing playlists are never
  reused or overwritten.
- Success messages name the playlist created; activation is verified and a
  failure to activate is reported.
- The single-track message no longer says "selected" when it comes from the
  Entire Media Library command.
- Component compiled at warning level 4 with warnings as errors.
- CI: version consistency check (version.h, CHANGELOG, release tag);
  foobar2000 installer used by the integration test is hash-pinned; the
  real-CPU chi-square diagnostic no longer fails by chance (~1e-4 → ~1e-13).

## 1.0.1 — 2026-11-17

- Clearer message when *Selected Tracks* is used on a single track: it now
  explains that one track has only one possible order (previously it read
  "using no hardware random values drawn"). Behaviour is unchanged.
- Verified on a user machine: Intel Core i7-13620H, Windows 11 25H2,
  foobar2000 2.26.3 x64 — CPUID reports RDSEED and RDRAND; active source
  RDSEED; Current Playlist randomized 23 tracks with 11 RDSEED values.

## 1.0.0 — 2026-11-17

First release.

- Commands: *True Random: Entire Media Library*, *True Random: Current
  Playlist*, *True Random: Hardware RNG Status* (Edit menu) and *True Random:
  Selected Tracks* (context menu).
- Hardware-only randomness: RDSEED preferred, RDRAND fallback, explicit
  failure otherwise. Bounded retries, self-test and stuck-output detection.
  No software/OS fallback.
- Fisher–Yates shuffle with rejection-sampled bounded integers (no modulo
  bias), proven by exhaustive tests.
- Transactional playlist operations: full permutation computed first; failed
  operations leave no partial playlist; in-place shuffle is undoable.
- Built against the official foobar2000 SDK 2026-10-01 for foobar2000 v2 x64.
