# Portable core library and test suite.
#
# This CMake project builds and tests everything that does NOT depend on the
# foobar2000 SDK (random core, hardware RNG layer, playlist transaction logic).
# The foobar2000 component itself is built with MSBuild from
# foobar2000/foo_true_random/foo_true_random.vcxproj (see README.md).
cmake_minimum_required(VERSION 3.20)
project(foo_true_random_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ftr_core STATIC
    src/hardware_rng.cpp
    src/hardware_rng_x86.cpp
    src/system_rng.cpp)
target_include_directories(ftr_core PUBLIC include)

if(MSVC)
    target_compile_options(ftr_core PRIVATE /W4 /WX /permissive-)
else()
    # Note: no -mrdrnd / -mrdseed. The instructions are enabled per function
    # with target attributes, behind the CPUID check.
    target_compile_options(ftr_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(FTR_SANITIZE "Build tests with ASan/UBSan (GCC/Clang)" OFF)

enable_testing()
foreach(t test_core test_hardware_rng test_playlist_logic test_hardware_real)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE ftr_core)
    if(MSVC)
        # C4127 (constant conditional) fires on compile-time-constant test expressions.
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /bigobj /wd4127)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(FTR_SANITIZE)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()

if(FTR_SANITIZE AND NOT MSVC)
    target_compile_options(ftr_core PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()
