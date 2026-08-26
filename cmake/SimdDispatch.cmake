# SimdDispatch.cmake — runtime SIMD dispatch helpers.
#
# Hot-path DSP kernels ship a scalar fallback and an AVX2/FMA variant. The
# correct variant is selected at first use via __builtin_cpu_supports("avx2").
# This file defines a helper function that applies the macro convention used by
# the dsp/ headers: each kernel header exposes a `<name>_scalar` and an
# `<name>_avx2` function, plus a `<name>()` inline dispatcher that picks one.
#
# AVX2/FMA are ALWAYS enabled at the compiler level: CompilerFlags.cmake passes
# -march=x86-64-v3 globally, so every TU can use the intrinsics. Nothing in this
# file can "disable" AVX2 — the dispatcher only decides at runtime whether the
# scalar or the AVX2 variant actually runs (scalar on CPUs without AVX2/FMA).
# BOOTAMP_AVX2_STRICT below additionally tags kernel files with per-file
# -mavx2 -mfma, redundant with -march but explicit in the build.

# Mark a source file as containing an AVX2/FMA kernel. With BOOTAMP_AVX2_STRICT=ON
# (the default) the tagged files are compiled with explicit per-file -mavx2 -mfma;
# with OFF the call is a clean no-op — the file still compiles under the global
# -march=x86-64-v3. The flags stay local to the tagged files and never leak
# -ffast-math.
function(bootamp_avx2_kernel target)
  if(BOOTAMP_AVX2_STRICT)
    set_source_files_properties(${ARGN} PROPERTIES
      COMPILE_OPTIONS "-mavx2;-mfma")
  endif()
endfunction()

option(BOOTAMP_AVX2_STRICT
  "Tag AVX2 kernel files with per-file -mavx2 -mfma (redundant with global -march=x86-64-v3; makes the AVX2 requirement explicit)"
  ON)

message(STATUS "bootamp SIMD: AVX2/FMA enabled globally via -march=x86-64-v3, "
               "runtime dispatch via __builtin_cpu_supports(\"avx2\") "
               "(BOOTAMP_AVX2_STRICT=${BOOTAMP_AVX2_STRICT})")