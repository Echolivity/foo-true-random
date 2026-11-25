# Hardware random-number generation

## What the component uses

foo-true-random takes every random bit from one of two x86 instructions,
or, only on a CPU that has neither, from Windows:

| Priority | Source | Used when | Retry policy |
|---|---|---|---|
| 1 | `RDSEED` | `CPUID.(EAX=7, ECX=0):EBX[18]` (only read if `CPUID.0:EAX >= 7`) | up to 1024 attempts, `PAUSE` between attempts |
| 2 | `RDRAND` | `CPUID.(EAX=1):ECX[30]` | up to 10 attempts, `PAUSE` between attempts |
| 3 | — | the CPU has RDSEED or RDRAND, but they failed | **explicit failure** |
| — | `BCryptGenRandom` (`BCRYPT_USE_SYSTEM_PREFERRED_RNG`) | the CPU has **neither** instruction | one call per value; failure is final |

The 64-bit forms (`_rdseed64_step`, `_rdrand64_step`) are used. Each 64-bit
word supplies two 32-bit values to the shuffler.

The Windows generator is not a fallback for failing hardware: on a CPU that
has RDSEED or RDRAND it is never called, even if both fail. It replaces the
instructions only on CPUs that do not have them. There is no software PRNG,
no `rand()`, no `std::random_device` and no foobar2000 built-in shuffle, and no
setting can enable one.

## Source selection and failure handling (per operation)

Every menu command creates a fresh `hardware_rng` instance:

1. CPUID is read once per process (the provider is immutable and thread-safe).
2. **Self-test**: for each supported instruction, 8 words are drawn and
   discarded. A source that exhausts its retry budget, or returns two identical
   consecutive 64-bit words, is marked unavailable for this operation before any
   of its output is used.
3. Values are drawn from RDSEED. If RDSEED exhausts its retry budget during
   the operation, it is marked unavailable for the rest of the operation and
   RDRAND supplies subsequent values.
4. If RDRAND exhausts its budget too, the operation fails closed.
5. If a source returns two identical consecutive 64-bit words *after* its
   output has been used (probability 2^-64 for a working generator), the
   operation is aborted rather than switched, because values already consumed
   are suspect.

Unsupported instructions are never executed: the selection code checks CPUID
before every call, and the instruction wrapper re-checks it (defence in depth).
On GCC/Clang the instructions are enabled only per function via
`__attribute__((target(...)))`, so the compiler cannot emit them elsewhere.

Status reporting shows the source that **actually supplied** the values
(e.g. `RDSEED (512 x 64-bit values)` or `RDSEED (3 …), then RDRAND (509 …)`),
not merely what CPUID advertises.

### Why the repeated-value check

Real silicon has shipped with RNG defects where the instruction reports
success (CF=1) but returns a constant — e.g. some AMD processors returning
all-ones from `RDRAND` until a firmware update, and an AMD advisory in 2025
about `RDSEED` returning zero with CF=1 on some Zen 5 parts (reported for the
16- and 32-bit forms). The check is not a statistical test of quality; it is
a cheap guard against this specific "stuck output" failure mode.

## CPU compatibility

| CPU | RDSEED | RDRAND | Result |
|---|---|---|---|
| Intel Sandy Bridge (2nd gen Core) | no | no | Uses the **Windows system RNG** (`BCryptGenRandom`) |
| Intel Ivy Bridge, Haswell | no | yes | Uses RDRAND |
| Intel Broadwell and newer | yes | yes | Uses RDSEED |
| AMD Zen 1 and newer (incl. Ryzen 9 7950X3D) | yes | yes | Uses RDSEED (expected; verify with *Hardware RNG Status*) |

Do not take this table as a substitute for runtime detection — the component
always checks CPUID on the machine it runs on. Virtual machines may hide
these CPUID bits even on capable hosts.

Up to version 1.0.2, a CPU without RDSEED/RDRAND refused every operation.
Since 1.1.0 it uses `BCryptGenRandom`, Windows' cryptographic generator, which
needs no extra runtime and exists on every Windows version foobar2000 v2 runs
on. The same health checks apply: 8 words are drawn and discarded first, and
two identical consecutive 64-bit values reject the output.

## What this does and does not guarantee

- On CPUs without RDSEED/RDRAND, values come from `BCryptGenRandom`. Windows
  documents it as its cryptographically secure generator; this project relies
  on that and does not test or certify it either.
- RDSEED and RDRAND are CPU-provided interfaces to an on-chip random number
  generator. RDSEED is specified by the vendors as returning conditioned
  entropy-source output intended for seeding; RDRAND returns output of a
  hardware DRBG that is reseeded from the same source.
- This project does **not** access raw physical noise, and it does **not**
  certify, test, or vouch for the quality of the CPU's internal entropy source.
  It has no independent cryptographic certification.
- "Hardware instruction available" (from CPUID) is a different statement from
  "entropy quality independently verified". Only the former is established by
  this component.
- What the project *does* establish, by exhaustive proof in the test suite, is
  that the mapping from uniformly distributed 32-bit words to playlist
  permutations is exactly unbiased. See [TESTING.md](TESTING.md).
