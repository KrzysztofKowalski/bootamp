# bootamp golden files

Golden reference outputs generated from the Go cliamp test suite, used to
verify the C++23 + SIMD port matches the Go semantics bit-for-bit (within a
small floating-point tolerance).

## Generation

Add a throwaway `TestExport*Golden` in `cliamp/` that dumps each golden fixture
to `golden/<name>.json`. The C++ Catch2 tests load the matching golden and
compare with `epsilon = 1e-5`:

```
golden/biquad_sine1k_gain3db.json   — 10-band EQ applied to a 1 kHz sine, +3 dB
golden/fft_sine440_n2048.json        — FFTW3f r2c power spectrum of a 440 Hz sine, n=2048
golden/wsola_1p5x.json               — WSOLA time-stretch at 1.5x speed
```

## Comparison rule

Golden-file comparisons in `tests/{dsp,audio,ui}/` MUST be guarded: if the
golden file does not exist, SKIP the test and verify an analytic invariant
instead (e.g. energy preservation, silence gate, peak position). Never fail a
test on a missing golden — goldens are host-generated and may not be checked in.

The comparison tolerance is `epsilon = 1e-5` for float32 DSP paths and
`epsilon = 1e-9` for the double-precision WSOLA offset_score.