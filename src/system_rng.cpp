// foo-true-random — the operating system's random number generator.
//
// Used ONLY on CPUs that support neither RDSEED nor RDRAND (Intel Sandy Bridge
// and older); hardware_rng enforces that. On Windows this is BCryptGenRandom
// with the system-preferred RNG, which needs no extra runtime and is present on
// every Windows version foobar2000 v2 supports. Linux uses getrandom(2) so the
// portable test suite can exercise this path; other platforms report failure.
//
// SPDX-License-Identifier: MIT
#include "ftr/hardware_rng.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace ftr {

namespace {

class os_rng final : public system_rng_provider {
public:
    bool random64(std::uint64_t& out) noexcept override {
#if defined(_WIN32)
        const NTSTATUS st = BCryptGenRandom(nullptr, static_cast<PUCHAR>(static_cast<void*>(&out)),
                                            static_cast<ULONG>(sizeof out), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return BCRYPT_SUCCESS(st);
#elif defined(__linux__)
        unsigned char* p = static_cast<unsigned char*>(static_cast<void*>(&out));
        std::size_t got = 0;
        while (got < sizeof out) {
            const ssize_t r = getrandom(p + got, sizeof out - got, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
#else
        (void)out;
        return false;
#endif
    }
};

}  // namespace

system_rng_provider& os_system_rng_provider() noexcept {
    static os_rng provider;
    return provider;
}

}  // namespace ftr
