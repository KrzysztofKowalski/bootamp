// ui/vis_drivers/binary.cpp — "Binary" streaming 0/1 data columns
// (port of cliamp/ui/vis_binary.go).
//
// Streaming columns of 0s and 1s that scroll at speeds proportional to each
// band's energy. Higher energy produces more 1s (active data) and brighter
// coloring, creating a raw data-stream aesthetic. Bit values come from a
// position hash gated by a per-band probability (1s on high-energy bands glow
// bright; 0s stay dim); scrolling is time-independent — the per-column scroll
// offset from the frame counter creates the motion. Render-only driver; the
// framework drives analysis + smoothing; cadence is the default (kTickFast
// while playing, kTickSlow otherwise).
#include "ui/vis_drivers/registry.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// scatterHash returns a pseudo-random value in [0, 1) for a given dot position
// and frame (Go scatterHash; shared with the logo/pulse drivers). The
// int64-typed variant matches Go's int (64-bit) arithmetic for the large
// scroll offsets the binary driver feeds in.
double scatter_hash(int64_t band, int64_t row, int64_t col, std::uint64_t frame) {
  const std::uint64_t f =
      (frame + static_cast<std::uint64_t>(row * 3 + col)) / 3;
  std::uint64_t h = static_cast<std::uint64_t>(band) * 7919 +
                    static_cast<std::uint64_t>(row) * 6271 +
                    static_cast<std::uint64_t>(col) * 3037 + f * 104729;
  h ^= h >> 16;
  h *= 0x45d9f3b37197344bULL;
  h ^= h >> 16;
  return static_cast<double>(h % 10000) / 10000.0;
}

// visBandWidth returns the character width for band b of `total` bands within
// `width` terminal cells (Go visBandWidth). At narrow widths only the leading
// visible bands receive columns; the final frame fitting clips the legacy
// inter-band gaps emitted by older renderers.
int vis_band_width(int total, int b, int width) {
  if (total <= 0 || b < 0 || b >= total || width <= 0) {
    return 0;
  }
  const int visible = std::min(total, width);
  if (b >= visible) {
    return 0;
  }
  const int gap_count = std::min(visible - 1, std::max(0, width - visible));
  const int band_cols = width - gap_count;
  const int base      = band_cols / visible;
  const int extra     = band_cols % visible;
  if (b < extra) {
    return base + 1;
  }
  return base;
}

class BinaryDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0 || bands.empty()) {
      return;  // the framework always supplies 10 bands; empty = safety
    }
    const std::size_t band_count = bands.size();

    for (int row = 0; row < height; ++row) {
      int col = 0;
      for (std::size_t b = 0; b < band_count; ++b) {
        const int w = vis_band_width(static_cast<int>(band_count),
                                     static_cast<int>(b), width);
        for (int i = 0; i < w; ++i) {
          const double energy = static_cast<double>(bands[b]);

          // Scroll speed per column: higher energy = faster data flow.
          const int    speed  = std::max(1, 4 - static_cast<int>(energy * 3.0));
          const int64_t scroll = static_cast<int64_t>(frame) / speed;

          // Bit value from position hash (time-independent; scroll creates motion).
          const double h = scatter_hash(static_cast<int64_t>(b),
                                        static_cast<int64_t>(row) + scroll, col, 0);
          const double one_prob = energy * 0.6 + 0.15;
          const char   ch       = (h < one_prob) ? '1' : '0';

          // 1s on high-energy bands glow bright; 0s stay dim.
          int tag;
          if (ch == '1' && energy > 0.4) {
            tag = 2;
          } else if (ch == '1' || energy > 0.3) {
            tag = 1;
          } else {
            tag = 0;
          }
          const Color color = (tag == 2) ? kColorSpecHigh
                              : (tag == 1) ? kColorSpecMid
                                           : kColorSpecLow;
          grid.set(row, col, Cell{static_cast<char32_t>(ch), color});
          ++col;
        }
        if (b < band_count - 1) {
          // Inter-band gap: unstyled space (Go flushStyleRun + raw ' ').
          grid.set(row, col, Cell{U' ', kColorDefault});
          ++col;
        }
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp renderOnlyDriver -> defaultDriverTickInterval: overlays and
    // stopped playback tick slowly; actively playing ticks fast.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_binary_driver() {
  return std::make_unique<BinaryDriver>();
}

}  // namespace bootamp::ui::vis_drivers
