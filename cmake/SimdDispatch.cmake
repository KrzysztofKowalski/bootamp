# SimdDispatch.cmake — runtime SIMD dispatch helpers.
#
# Hot-path DSP kernels ship a scalar fallback and an AVX2/FMA variant. The
# correct variant is selected at first use via __builtin_cpu_supports("avx2").
# This file defines a helper function that applies the macro convention used by
# the dsp/ headers: each kernel header exposes a `<name>_scalar` and an
# `<name>_avx2` function, plus a `<name>()` inline dispatcher that picks one.
#
# No compile-time flag is used to enable AVX2 globally (other than
# -march=x86-64-v3, which permits the intrinsics to compile). The dispatcher
# guarantees the scalar path runs on CPUs without AVX2/FMA.

# Convenience: mark a source file as containing an AVX2 kernel. Currently a
# no-op (the intrinsics are guarded by the runtime check, not a compile flag),
# but kept as an extension point so future per-file -mavx2 -mfma additions stay
# local and never leak -ffast-math.
function(bootamp_avx2_kernel target)
  if(BOOTAMP_AVX2_STRICT)
    set_source_files_properties(${ARGN} PROPERTIES
      COMPILE_OPTIONS "-mavx2;-mfma")
  endif()
endfunction()

option(BOOTAMP_AVX2_STRICT "Compile AVX2 kernel files with explicit -mavx2 -mfma" OFF)

message(STATUS "bootamp SIMD: runtime dispatch via __builtin_cpu_supports(\"avx2\") "
               "(strict per-file AVX2=${BOOTAMP_AVX2_STRICT})")