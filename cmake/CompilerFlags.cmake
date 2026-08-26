# CompilerFlags.cmake — bootamp global compile flags.
# C++23, optimized for the audio hot path, NO -ffast-math (preserves NaN/Inf
# for EQ edge gains), x86-64-v3 baseline for AVX2 + FMA intrinsics.

# NOTE: do NOT set CMAKE_CXX_STANDARD to "2c" here. CMake 4.4's try_compile
# (used by find_package(Threads) via google-benchmark, and by FindCatch2)
# rejects the unknown "CXX2c" dialect and aborts configure. The root
# CMakeLists.txt sets CMAKE_CXX_STANDARD=23 (a CMake-known dialect) so every
# try_compile succeeds; we then force the real dialect on our own targets
# via the -std=c++2c option below. On gcc the last -std flag on the command
# line wins, so our explicit -std=c++2c overrides the CMake-injected one.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++2c>)

# Default to Release unless the caller set CMAKE_BUILD_TYPE.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optimization flags common to all build types.
add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:-O3>
  $<$<COMPILE_LANGUAGE:CXX>:-march=x86-64-v3>
  # NEVER enable -ffast-math — it breaks the EQ's NaN/Inf edge handling and
  # the WSOLA double-precision search ranking. Manual SIMD intrinsics only.
  -Wall
  -Wextra
  -Wpedantic
  -Wno-unused-parameter)

# Debug builds add sanitizers + debug info (TSAN toggled separately by the
# BOOTAMP_ENABLE_TSAN option, which is only applied to audio tests).
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT BOOTAMP_ENABLE_TSAN)
  add_compile_options(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()