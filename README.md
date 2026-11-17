# foo-true-random

A foobar2000 v2 (Windows x64) component that shuffles playlists using **only**
the CPU's hardware random-number instructions — `RDSEED`, falling back to
`RDRAND` — with a provably unbiased Fisher–Yates shuffle.

If the CPU has neither instruction, or both fail, the component **refuses** and
changes nothing. There is no software PRNG, no Windows crypto API, no
`std::random_device`, and no use of foobar2000's built-in shuffle — not even
as a hidden fallback.

It is a random playlist generator. Nothing more: no recommendations, history,
weighting or "smart" ordering.

## Commands

| Where | Command | What it does |
|---|---|---|
| Edit menu | **True Random: Entire Media Library** | New playlist "True Random - Library" with every Media Library track in hardware-random order |
| Edit menu | **True Random: Current Playlist** | Reorders the active playlist in place. **Edit → Undo** restores the previous order |
| Edit menu | **True Random: Hardware RNG Status** | Shows CPUID capabilities and the source actually supplying values |
| Context menu | **True Random: Selected Tracks** | New playlist "True Random - Selection" with the selected tracks in hardware-random order (source playlist untouched) |

Success is logged to the foobar2000 console (View → Console) with the track
count and the source that supplied the values, e.g.
`Randomized 4211 tracks into a new playlist using RDSEED (2105 x 64-bit values).`
Failures show a popup and are also logged.

## Requirements

- foobar2000 v2.x, **64-bit**.
- A CPU with `RDSEED` or `RDRAND` (Intel Ivy Bridge or newer; AMD Zen or newer).
  On older CPUs such as **Intel Sandy Bridge** every command reports
  "hardware RNG unavailable" and does nothing — by design.

## Install

1. Download `foo-true-random.fb2k-component` from the
   [Releases](https://github.com/Echolivity/foo-true-random/releases) page.
2. foobar2000 → File → Preferences → Components → **Install…**, pick the file,
   click **Apply**, let foobar2000 restart.
   (Or double-click the file / drag it onto the Components page.)
3. Run **Edit → True Random: Hardware RNG Status** to confirm which instruction
   your CPU provides.

## How the randomness works (short version)

- `j = uniform_below(i + 1)` by rejection sampling on 32-bit hardware words:
  values below `threshold = 2^32 mod n` are discarded, the rest are reduced
  with `x mod n`. No modulo bias.
- Fisher–Yates: `for i = n-1 … 1: swap(a[i], a[uniform_below(i+1)])`.
- The complete permutation is computed **before** any playlist is touched. A
  failed operation leaves no partial playlist and no changed playlist.

Details: [docs/DESIGN.md](docs/DESIGN.md), [docs/HARDWARE_RNG.md](docs/HARDWARE_RNG.md),
[docs/TESTING.md](docs/TESTING.md).

**Honesty note.** RDSEED/RDRAND are CPU-provided interfaces to an on-chip
random number generator. This project does not access raw physical noise and
does not certify the quality of the CPU's entropy source. What it guarantees,
by exhaustive proof in its tests, is that uniformly distributed hardware words
are turned into uniformly distributed playlist orders.

## Building from source

Prerequisites: Visual Studio 2022 (or newer) with the *Desktop development
with C++* workload (v143 toolset), CMake ≥ 3.20, 7-Zip or Windows `tar`.

From a **Developer PowerShell for VS 2022**:

```powershell
git clone https://github.com/Echolivity/foo-true-random
cd foo-true-random
./scripts/fetch-sdk.ps1      # official foobar2000 SDK 2026-10-01, SHA-256 pinned
./scripts/build.ps1          # Debug|x64 + Release|x64, then packages
# -> dist\foo-true-random.fb2k-component
```

Portable tests (any OS with a C++20 compiler; on x86-64 the real-hardware test
also executes RDSEED/RDRAND if CPUID reports them):

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure -V
```

The same steps run in GitHub Actions on every push
([.github/workflows/build.yml](.github/workflows/build.yml)).

## Repository layout

```
include/ftr/        portable core: random_core.h, hardware_rng.h, playlist_logic.h
src/                hardware_rng.cpp (selection/retry logic), hardware_rng_x86.cpp (CPUID/RDSEED/RDRAND)
tests/              unit, exhaustive, mock-hardware, fake-playlist and real-hardware tests
foobar2000/foo_true_random/   component sources + foo_true_random.vcxproj
scripts/            fetch-sdk.ps1, build.ps1, package.ps1
docs/               DESIGN.md, HARDWARE_RNG.md, TESTING.md
```

## License

MIT — see [LICENSE](LICENSE). The foobar2000 SDK is downloaded at build time
and is not redistributed in this repository.
