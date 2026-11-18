# Testing

Four test executables (CMake/CTest) plus one in-foobar2000 integration test.
All of them run in GitHub Actions on every push
([build.yml](../.github/workflows/build.yml)); the latest run's log is the
authoritative evidence.

## 1. `test_core` — randomization core (portable)

| Area | What is checked |
|---|---|
| Rejection threshold | Known values (n = 1, 2, 3, 5, 6, 7, 2^31, 2^31+1, 2^32−1); `(0−n) mod n` equals `2^32 mod n` computed in 64-bit for ~5,100 bounds; no integer-promotion bug for 8/16-bit words |
| `uniform_below` | Rejects values below the threshold, accepts the threshold itself and values above, n = 1 always 0, n = 0 fails without consuming input, retry budget is exact and bounded, RNG failure propagates |
| **Exhaustive uniformity** | For **every** 8-bit bound 1…255 and every 8-bit input, accepted inputs map to each residue exactly equally often; same for 75 selected 16-bit bounds over all 65,536 inputs |
| Fisher–Yates | Empty and single-element lists consume no randomness; both 2-element outcomes; the draws request bounds n, n−1, …, 2 |
| **Exhaustive permutations** | For n = 3 all 6 choice sequences give 6 distinct permutations, each exactly once, none missing; for n = 1…7, the n! choice sequences map bijectively onto the n! permutations |
| **End-to-end exhaustive** | At 8-bit word width, n = 3 and n = 4: enumerate every accepted raw word sequence through rejection sampling + Fisher–Yates; every permutation occurs exactly equally often |
| Integrity | Duplicates preserved, nothing lost (multiset equality); 100,000-element permutation validity; failure mid-shuffle leaves output empty; list too long for the word width rejected |

The exhaustive tests are the proof of unbiasedness: production uses the
identical templates at 32-bit width, where the same counting argument holds
(accepted range is an exact multiple of n). No statistical test is used as a
proof.

## 2. `test_hardware_rng` — hardware RNG logic with mock instructions

A mock instruction provider scripts CF/value results and **counts any call to
an instruction it was told is unsupported** (on real hardware that would be
`#UD`). Every test asserts zero such calls where relevant.

- CPUID decode: both bits; Sandy Bridge (neither); Ivy Bridge (RDRAND only);
  swapped bits (RDSEED bit in leaf 1 / RDRAND bit in leaf 7) enable nothing;
  leaf 7 ignored when max leaf < 7.
- Both available → RDSEED preferred; RDSEED unsupported → RDRAND, RDSEED never
  executed; RDRAND unsupported → RDRAND never executed.
- Transient RDSEED failures retried with backoff.
- RDSEED retry exhaustion (exactly the budget) → marked unavailable,
  RDRAND supplies the rest, RDSEED not retried; usage summary reports both.
- RDRAND retry exhaustion (exactly 10 attempts) → fail closed, no further
  instruction executions.
- Neither available → fail closed, no instructions executed, shuffle aborts.
- Stuck source in self-test (RDSEED returning 0, RDRAND returning all-ones)
  → disqualified before use; stuck at runtime → operation aborted, no switch.
- Active-source reporting reflects the source that supplied the last value.

## 3. `test_playlist_logic` — transactions on a fake playlist manager

The fake models indexed playlists, active playlist, append, remove,
`new[k] = old[order[k]]` reorder, locks and an undo stack, with failure
injection (create fails, insert fails, partial insert reported as success,
remove fails, reorder refused, locked playlist).

Checked: counts, identities, duplicates, source playlists untouched, new
playlist activated on success (and activation failure reported), unique
names on collision, exceptions after creation (rollback still happens and the
error propagates), **another component changing the playlist list
mid-operation (nothing is removed blindly)**, 50,000-entry inputs, **no playlist created when the RNG is
unavailable or fails mid-way**, rollback removes the incomplete playlist and
restores the active playlist, in-place shuffle leaves the playlist untouched
(and creates no undo point) on RNG failure, undo restores the exact previous
order.

## 4. `test_hardware_real` — the real CPU

Executes CPUID and — only if reported — RDSEED/RDRAND on the machine running
the test. Prints CPU brand, raw CPUID bits, the source actually used, sample
words; builds a 200,000-element permutation; and runs a chi-square
**diagnostic** over 120,000 three-element shuffles. It fails only above 70
(p ≈ 1e-13 for a working generator), so it catches grossly broken plumbing
without failing by chance. On a CPU without either
instruction it verifies the fail-closed path instead.

## 5. Integration test inside real foobar2000

`scripts/integration-test.ps1`:

1. Silently installs foobar2000 v2.26.3 x64 into a throw-away **portable**
   folder (never touches an existing installation/profile).
2. Installs the shipped `foo_true_random.dll` **extracted from
   `dist/foo-true-random.fb2k-component`**, plus a separate driver component
   `foo_true_random_itest.dll` (built with `/p:FTR_ITEST=true`; never shipped).
3. Starts foobar2000. The driver locates the shipped component's commands
   through foobar2000's menu services by GUID and checks:
   - all four commands are registered under the exact names;
   - `playlist_reorder_items` semantics (`new[k] == old[order[k]]`);
   - **Selected Tracks** (via the registered context-menu item): one new
     playlist named "True Random - Selection", 42 entries incl. a 3× duplicate,
     identical identities, different order, source playlist unchanged;
   - **Current Playlist** (via `mainmenu_commands::g_execute`): same playlist,
     same identities, order changed, undo point exists, native
     `playlist_undo_restore` returns the exact previous order;
   - **Entire Media Library** (via `g_execute`): on the CI machine the Media
     Library is not configured, so this verifies the "not configured → nothing
     created" path; with a configured library it verifies every library track
     appears exactly once;
   - running Selected Tracks again creates "True Random - Selection (2)";
   - rollback against the real `playlist_manager` (insert refused; partial
     insert; exception after a partial insert) leaves no playlist behind and
     restores the active playlist;
   - a no-hardware CPU model refuses without creating anything.
4. Writes a report and exits foobar2000; the script fails unless it ends with
   `RESULT: PASS`.

To run it on your own machine (e.g. the Ryzen 9 7950X3D) from a Developer
PowerShell in the repository root:

```powershell
./scripts/build.ps1
msbuild foobar2000\foo_true_random\foo_true_random.vcxproj /p:Configuration=Release /p:Platform=x64 /p:FTR_ITEST=true
./scripts/integration-test.ps1
cmake -S . -B build-tests; cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release -V      # includes test_hardware_real on your CPU
```

## What is not covered automatically

- Runs on the specific Ryzen 9 7950X3D and the Sandy Bridge machine: CI uses
  GitHub's Azure-hosted CPUs. The Sandy Bridge behaviour is covered by the
  CPUID-decode tests (exact Sandy Bridge register model) and the no-hardware
  paths, but has not been executed on that physical machine.
- Entire Media Library with a populated library inside foobar2000 (CI profile
  has no library folders configured). Covered manually, see below.
- Interactive UI checks (menu placement, popup appearance) are not automated.

## Manual verification on a user machine

Intel Core i7-13620H, Windows 11 25H2 (build 26200), foobar2000 2.26.3 x64,
component 1.0.2-rc1 (identical code to 1.0.2), 2026-11-18:

| Check | Result |
|---|---|
| Hardware RNG Status | RDSEED and RDRAND supported; active source RDSEED |
| Entire Media Library, library folder with no music | "The Media Library is empty. Nothing was changed." No playlist created |
| Entire Media Library, 23-track library | `Randomized 23 tracks into new playlist "True Random - Library" using RDSEED (11 x 64-bit values).` |
| Current Playlist, then Ctrl+Z | Order changed; Ctrl+Z restored the exact previous order |
| Selected Tracks, twice | "True Random - Selection" and "True Random - Selection (2)" created |
