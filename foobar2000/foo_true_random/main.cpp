// foo_true_random — component declaration.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include "version.h"

DECLARE_COMPONENT_VERSION(
    "True Random",
    FTR_VERSION_STRING,
    "foo_true_random " FTR_VERSION_STRING "\n"
    "Hardware-randomized playlists for foobar2000.\n"
    "\n"
    "Randomness comes exclusively from the CPU's RDSEED instruction (preferred) or RDRAND "
    "instruction (fallback). If neither is available or both fail, operations are refused. "
    "There is no software PRNG, operating-system, or built-in shuffle fallback.\n"
    "\n"
    "Shuffling uses Fisher-Yates with rejection sampling (no modulo bias).\n"
    "\n"
    "RDSEED/RDRAND are CPU-provided interfaces to an on-chip random number generator. This "
    "component does not certify the quality of that entropy source.\n"
    "\n"
    "MIT License. https://github.com/Echolivity/foo-true-random");

// Prevents renaming the DLL or loading multiple instances.
VALIDATE_COMPONENT_FILENAME("foo_true_random.dll");
