// dsp/wsola.hpp - WSOLA time-stretch for the playback-speed control.
//
// Port of cliamp/player/speed.go (speedStreamer). Constants match cliamp
// exactly: tsSeq=3584, tsOvlp=512, tsSearch=1024 (tsWin = tsSeq+tsOvlp = 4096),
// coarse stride 8, top-8 candidates. The alignment score (corr^2/norm) is
// ranked in double precision (plan decision 7); the AVX2 kernel does the
// 4xdouble dot-product + horizontal reduce, with a scalar fallback. The
// search inside compute_best_offset / stretch_one_step uses the scalar kernel
// so candidate tie-breaking is bit-identical to Go. NEVER -ffast-math - NaN/Inf
// must propagate unchanged.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bootamp::dsp {

// WSOLA frame layout (cliamp speed.go). All lengths are in source-rate frames.
inline constexpr std::size_t kTsSeq    = 3584;   // sequence: ~81 ms @44.1 kHz - time between crossfades
inline constexpr std::size_t kTsOvlp   = 512;    // overlap: ~12 ms - crossfade region
inline constexpr std::size_t kTsWin    = kTsSeq + kTsOvlp;   // source window per frame (4096)
inline constexpr std::size_t kTsSearch = 1024;   // search: +/-~23 ms - covers several pitch periods
inline constexpr std::size_t kTsCoarse = 8;      // sample stride of the low-cost correlation pass
inline constexpr std::size_t kTsTop    = 8;      // coarse candidates retained for full refinement

// Pre-computed linear crossfade table: ts_alpha[i] = i / kTsOvlp (port of
// cliamp's tsAlpha). Avoids per-sample division in the hot crossfade loop.
// i/512 is exact in binary floating point, so this is bit-identical to the
// Go init() loop.
inline constexpr std::array<double, kTsOvlp> ts_alpha = [] {
  std::array<double, kTsOvlp> a{};
  for (std::size_t i = 0; i < kTsOvlp; ++i) {
    a[i] = static_cast<double>(i) / static_cast<double>(kTsOvlp);
  }
  return a;
}();

// offset_score ranks a candidate alignment between the previous overlap tail
// and a candidate window: corr^2/norm (port of speedStreamer.offsetScore;
// ranking-equivalent to corr/sqrt(norm), no sqrt needed). corr and norm are
// accumulated in double (plan decision 7); the AVX2 kernel does the 4xdouble
// dot-product + horizontal reduce. Returns 0 for silence (norm < 1e-9) or
// non-positive correlation, exactly like cliamp.
//
// prev_tail / candidate are interleaved `channels` doubles per frame (the
// stereo engine uses channels=2 -> kTsOvlp*2 = 1024 doubles). Both must hold
// at least kTsOvlp*channels doubles for a full overlap.
double offset_score(std::span<const double> prev_tail,
                    std::span<const double> candidate,
                    std::size_t channels = 2);

// offset_score_scalar / offset_score_avx2 are the explicit kernels for
// benchmarking (BM_WsolaOffset_Scalar/AVX2) and golden tests. The scalar
// kernel is bit-identical to Go's loop; the AVX2 kernel matches within
// double rounding (~1e-16 relative).
double offset_score_scalar(std::span<const double> prev_tail,
                           std::span<const double> candidate,
                           std::size_t channels = 2);
double offset_score_avx2(std::span<const double> prev_tail,
                         std::span<const double> candidate,
                         std::size_t channels = 2);

// compute_best_offset finds the source position near `expected` whose start
// best matches the previous tail (port of speedStreamer.searchBestOffset):
// a coarse pass (stride kTsCoarse) over every offset in
// [expected-kTsSearch, expected+kTsSearch] retains the top kTsTop candidates,
// then each is refined at full resolution (+/-(kTsCoarse-1)). If the coarse
// pass finds nothing usable, a full-resolution exhaustive pass runs. The
// fallback (all correlations negative or silent) is `expected` clamped into
// the search range.
//
// `input` is the full available source (interleaved doubles, channels per
// frame) - the same buffer the stretch reads from. `prev_tail` holds
// kTsOvlp*channels doubles. Returns the best frame offset into `input`.
// Scalar scoring only: candidate ranking is bit-identical to Go.
int compute_best_offset(std::span<const double> input,
                        std::span<const double> prev_tail,
                        std::ptrdiff_t expected,
                        std::size_t channels = 2);

// WsolaState holds the running state for pull-based stretching via
// stretch_one_step: the previous sequence's tail (for the next crossfade),
// the speed ratio, and the fractional analysis cursor.
struct WsolaState {
  std::vector<double> prev_overlap;  // interleaved tail, kTsOvlp*channels doubles; empty => first frame
  double              speed       = 1.0;  // ratio; 1.0 = normal speed
  double              input_pos   = 0.0;  // fractional analysis cursor (Go's inPos)
};

// stretch_one_step advances the time-stretch by one sequence (port of
// wsolaFrame): finds the best alignment of state.prev_overlap inside `input`,
// crossfades (or verbatim-copies on the first frame), and writes
// kTsSeq*channels doubles to `output` (which must hold at least that many).
// `search_window` restricts where candidate frame starts may lie - pass
// `input` itself for the full cliamp range (it is ignored unless it is a
// sub-span of `input`). On success state.prev_overlap is updated to the new
// tail and state.input_pos advances by kTsSeq*state.speed. Returns the number
// of input frames consumed (round(kTsSeq*state.speed)), or 0 when `input`
// cannot supply a full sequence (nothing written).
std::size_t stretch_one_step(WsolaState& state,
                             std::span<const double> input,
                             std::span<const double> search_window,
                             std::span<double> output,
                             std::size_t channels = 2);

// WsolaStretcher - stateful WSOLA time-stretcher, the direct port of cliamp's
// speedStreamer. Wraps a pull source (duck-typed Source):
//
//   std::pair<std::size_t, bool> stream(std::span<std::array<float, 2>>);
//   std::string err() const;
//
// (audio::Streamer satisfies this - the frame type is the same
// std::array<float, 2>.) The audio engine feeds float32 stereo frames; the
// stretch itself runs in double exactly like Go, converted at the buffer
// boundaries. `speed` is a pointer to an atomic the UI thread writes
// (cliamp's *atomic.Uint64 Float64bits; bootamp uses std::atomic<double>
// directly); a null pointer means 1.0 (passthrough). Speed <= 0 or exactly
// 1.0 selects passthrough (cliamp semantics). Hot path: no locks, no
// allocation, no exceptions.
template <class Source>
class WsolaStretcher {
 public:
  explicit WsolaStretcher(Source& src, const std::atomic<double>* speed)
      : src_(src), speed_(speed) {
    in_.resize(static_cast<std::size_t>(kInitialInFrames));   // cliamp: make([][2]float64, 16384)
    out_.resize(static_cast<std::size_t>(kInitialOutFrames)); // cliamp: make([][2]float64, 8192)
  }

  // stream produces output frames. At speed 1.0x (or <= 0) it passes through
  // directly; otherwise it applies WSOLA time-stretching. Returns
  // (frames_written, more) with more == (frames_written > 0) - cliamp's
  // `return n, n > 0` contract.
  std::pair<std::size_t, bool> stream(std::span<std::array<float, 2>> dst) {
    const double speed = speed_ ? speed_->load(std::memory_order_relaxed) : 1.0;
    if (speed <= 0.0 || speed == 1.0) {
      return passthrough(dst);
    }
    while (out_wr_ - out_rd_ < static_cast<std::ptrdiff_t>(dst.size())) {
      if (!wsola_frame(speed)) {
        break;
      }
    }
    const std::size_t n = drain_out(dst);
    return {n, n > 0};
  }

  // err forwards to the wrapped source (cliamp's Err()).
  std::string err() const { return src_.err(); }

  // Test/telemetry accessors mirroring cliamp's package-internal ss.tail and
  // ss.tailValid (speed_test.go reads them directly).
  std::span<const double, 2 * kTsOvlp> prev_tail() const { return tail_; }
  bool tail_valid() const { return tail_valid_; }

 private:
  static constexpr std::ptrdiff_t kInitialInFrames  = 16384;
  static constexpr std::ptrdiff_t kInitialOutFrames = 8192;

  // ---- passthrough (cliamp speedStreamer.passthrough) -----------------------
  std::pair<std::size_t, bool> passthrough(std::span<std::array<float, 2>> dst) {
    std::size_t d = drain_out(dst);
    if (d == dst.size()) {
      return {d, true};
    }
    // Drain unconsumed source samples before switching to direct reads.
    const std::ptrdiff_t src_start = static_cast<std::ptrdiff_t>(std::round(in_pos_));
    if (const std::ptrdiff_t src_avail = in_n_ - src_start; src_avail > 0) {
      const std::size_t n = std::min(dst.size() - d, static_cast<std::size_t>(src_avail));
      for (std::size_t i = 0; i < n; ++i) {
        dst[d + i][0] = static_cast<float>(
            in_[static_cast<std::size_t>(2 * (src_start + static_cast<std::ptrdiff_t>(i)))]);
        dst[d + i][1] = static_cast<float>(
            in_[static_cast<std::size_t>(2 * (src_start + static_cast<std::ptrdiff_t>(i)) + 1)]);
      }
      d += n;
      in_pos_ += static_cast<double>(n);
      if (d == dst.size()) {
        return {d, true};
      }
    }
    // Reset WSOLA state for clean re-entry.
    out_rd_ = 0;
    out_wr_ = 0;
    in_n_ = 0;
    in_pos_ = 0;
    tail_valid_ = false;

    const auto [n, ok] = src_.stream(dst.subspan(d));
    const std::size_t total = d + n;
    return {total, ok || total > 0};
  }

  // ---- output ring drain (cliamp drainOut) ----------------------------------
  std::size_t drain_out(std::span<std::array<float, 2>> dst) {
    const std::ptrdiff_t avail = out_wr_ - out_rd_;
    const std::ptrdiff_t n = std::min(static_cast<std::ptrdiff_t>(dst.size()), avail);
    if (n <= 0) {
      return 0;
    }
    for (std::ptrdiff_t i = 0; i < n; ++i) {
      dst[static_cast<std::size_t>(i)][0] = static_cast<float>(
          out_[static_cast<std::size_t>(2 * (out_rd_ + i))]);
      dst[static_cast<std::size_t>(i)][1] = static_cast<float>(
          out_[static_cast<std::size_t>(2 * (out_rd_ + i) + 1)]);
    }
    out_rd_ += n;
    if (out_rd_ > kInitialOutFrames) {  // cliamp's hardcoded 8192 compaction point
      const std::ptrdiff_t rem = out_wr_ - out_rd_;
      if (rem > 0) {
        std::memmove(out_.data(), out_.data() + 2 * out_rd_,
                     static_cast<std::size_t>(rem) * 2 * sizeof(double));
      }
      out_rd_ = 0;
      out_wr_ = rem;
    }
    return static_cast<std::size_t>(n);
  }

  // ---- source fill (cliamp fillSource) --------------------------------------
  bool fill_source(std::ptrdiff_t need) {
    if (std::ptrdiff_t drop = static_cast<std::ptrdiff_t>(in_pos_) -
                              static_cast<std::ptrdiff_t>(kTsSearch);
        drop > 0) {
      drop = std::min(drop, in_n_);
      const std::ptrdiff_t keep = in_n_ - drop;
      if (keep > 0) {
        std::memmove(in_.data(), in_.data() + 2 * drop,
                     static_cast<std::size_t>(keep) * 2 * sizeof(double));
      }
      in_n_ = keep;
      in_pos_ -= static_cast<double>(drop);
      need = std::max<std::ptrdiff_t>(0, need - drop);
    }
    while (in_n_ < need) {
      const std::ptrdiff_t to_read = std::max<std::ptrdiff_t>(need - in_n_, 4096);
      if (in_n_ + to_read > static_cast<std::ptrdiff_t>(in_.size())) {
        in_.resize(static_cast<std::size_t>(in_n_ + to_read));
      }
      chunk_.resize(static_cast<std::size_t>(to_read));
      const auto [n, ok] = src_.stream(
          std::span<std::array<float, 2>>(chunk_.data(), static_cast<std::size_t>(to_read)));
      (void)ok;  // cliamp ignores ok here: `n, _ := s.Stream(...)`
      for (std::size_t i = 0; i < n; ++i) {
        in_[static_cast<std::size_t>(2 * (in_n_ + i))] =
            static_cast<double>(chunk_[static_cast<std::size_t>(i)][0]);
        in_[static_cast<std::size_t>(2 * (in_n_ + i) + 1)] =
            static_cast<double>(chunk_[static_cast<std::size_t>(i)][1]);
      }
      in_n_ += n;
      if (n == 0) {
        return in_n_ >= need;
      }
    }
    return true;
  }

  // ---- one synthesis frame (cliamp wsolaFrame) -------------------------------
  // Frame layout in source:
  //   [crossfade tsOvlp][direct copy tsSeq-tsOvlp][tail tsOvlp]
  //   |<----------- tsSeq (output) ------------>||<-- saved -->|
  //   |<------------------ tsWin (source read) ---------------->|
  bool wsola_frame(double speed) {
    std::ptrdiff_t expected = static_cast<std::ptrdiff_t>(std::round(in_pos_));
    const std::ptrdiff_t needed =
        expected + static_cast<std::ptrdiff_t>(kTsWin) +
        static_cast<std::ptrdiff_t>(kTsSearch) + 1;
    const bool filled = fill_source(needed);
    expected = static_cast<std::ptrdiff_t>(std::round(in_pos_));  // fill_source may have compacted
    if (!filled && expected + static_cast<std::ptrdiff_t>(kTsSeq) > in_n_) {
      return false;
    }

    const bool first = !tail_valid_;
    std::ptrdiff_t src_off = expected;
    if (!first) {
      src_off = static_cast<std::ptrdiff_t>(compute_best_offset(
          std::span<const double>(in_.data(), static_cast<std::size_t>(in_n_) * 2),
          tail_, expected, 2));
    }
    if (src_off + static_cast<std::ptrdiff_t>(kTsWin) > in_n_) {
      src_off = std::max<std::ptrdiff_t>(0, in_n_ - static_cast<std::ptrdiff_t>(kTsWin));
    }
    if (src_off + static_cast<std::ptrdiff_t>(kTsSeq) > in_n_) {
      return false;
    }

    // Grow the output ring if needed (cliamp: grow to outWr+tsSeq+4096).
    if (out_wr_ + static_cast<std::ptrdiff_t>(kTsSeq) > static_cast<std::ptrdiff_t>(out_.size())) {
      out_.resize(static_cast<std::size_t>(out_wr_ + static_cast<std::ptrdiff_t>(kTsSeq) + 4096));
    }

    if (first) {
      // Verbatim copy of the first sequence - no crossfade.
      std::copy_n(in_.data() + 2 * src_off, 2 * static_cast<std::ptrdiff_t>(kTsSeq),
                  out_.data() + 2 * out_wr_);
    } else {
      // Crossfade the overlap region using the pre-computed alpha table.
      for (std::size_t i = 0; i < kTsOvlp; ++i) {
        const double a = ts_alpha[i];
        const double b = 1.0 - a;
        const std::ptrdiff_t o = out_wr_ + static_cast<std::ptrdiff_t>(i);
        out_[static_cast<std::size_t>(2 * o)] =
            b * tail_[2 * i] +
            a * in_[static_cast<std::size_t>(2 * (src_off + static_cast<std::ptrdiff_t>(i)))];
        out_[static_cast<std::size_t>(2 * o + 1)] =
            b * tail_[2 * i + 1] +
            a * in_[static_cast<std::size_t>(2 * (src_off + static_cast<std::ptrdiff_t>(i)) + 1)];
      }
      // Direct copy of the rest - unmodified source samples.
      std::copy_n(in_.data() + 2 * (src_off + static_cast<std::ptrdiff_t>(kTsOvlp)),
                  2 * static_cast<std::ptrdiff_t>(kTsSeq - kTsOvlp),
                  out_.data() + 2 * (out_wr_ + static_cast<std::ptrdiff_t>(kTsOvlp)));
    }
    out_wr_ += static_cast<std::ptrdiff_t>(kTsSeq);

    // Save the tail for the next frame's crossfade.
    std::copy_n(in_.data() + 2 * (src_off + static_cast<std::ptrdiff_t>(kTsSeq)),
                2 * static_cast<std::ptrdiff_t>(kTsOvlp), tail_.data());
    tail_valid_ = true;

    in_pos_ += static_cast<double>(kTsSeq) * speed;
    return true;
  }

  Source& src_;
  const std::atomic<double>* speed_;

  std::vector<double> in_;                    // source buffer, interleaved doubles
  std::ptrdiff_t      in_n_ = 0;              // valid frame count
  double              in_pos_ = 0.0;          // fractional analysis cursor
  std::vector<std::array<float, 2>> chunk_;   // float staging for source reads

  std::vector<double> out_;                   // output ring, interleaved doubles
  std::ptrdiff_t      out_rd_ = 0;
  std::ptrdiff_t      out_wr_ = 0;

  std::array<double, 2 * kTsOvlp> tail_{};    // previous frame's trailing samples
  bool tail_valid_ = false;
};

}  // namespace bootamp::dsp
