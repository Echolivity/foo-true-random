# Changelog

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
