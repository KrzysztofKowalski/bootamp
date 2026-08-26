// ui/vis_drivers/terrain.cpp — scrolling side-view mountain range (port of
// cliamp/ui/vis_terrain.go).
//
// The terrain height buffer holds one height per dot column (2x the panel
// width). Each tick scrolls the buffer left by two dot columns and writes two
// new columns at the right from the average smoothed spectrum energy plus a
// small scatterHash noise for organic ridge edges. The renderer fills each dot
// column from its height down to the bottom; spectrum coloring paints green
// valleys, yellow slopes, red peaks (row-bottom tier).
//
// The driver is stateful: the terrain buffer persists across ticks (and across
// mode switches — on_enter/on_leave are no-ops, matching Go). The VisDriver
// contract gives tick() no grid, so the panel width is cached from the last
// render() call; the first tick before any render is a no-op (buffer empty).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// cliamp brailleBit: (row, col) in the 4x2 Braille dot grid -> bit value.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// scatterHash returns a pseudo-random value in [0, 1) for a given dot position
// and frame (cliamp scatterHash). Computed in double like the Go original.
double scatter_hash(int band, int row, int col, std::uint64_t frame) {
  const std::uint64_t f = (frame + static_cast<std::uint64_t>(row * 3 + col)) / 3;
  std::uint64_t h = static_cast<std::uint64_t>(band) * 7919 +
                    static_cast<std::uint64_t>(row) * 6271 +
                    static_cast<std::uint64_t>(col) * 3037 + f * 104729;
  h ^= h >> 16;
  h *= 0x45d9f3b37197344bULL;
  h ^= h >> 16;
  return static_cast<double>(h % 10000) / 10000.0;
}

// resize_terrain_buf grows/shrinks the terrain buffer to dotCols, right-
// aligning the existing tail so the newest (rightmost) history is preserved
// (cliamp resizeTerrainBuf). Returns a fresh buffer of size dotCols, or an
// empty buffer when dotCols <= 0.
std::vector<double> resize_terrain_buf(const std::vector<double>& buf, int dot_cols) {
  if (dot_cols <= 0) {
    return {};
  }
  if (static_cast<int>(buf.size()) == dot_cols) {
    return buf;
  }
  std::vector<double> next(static_cast<std::size_t>(dot_cols));
  const std::size_t copy_len = std::min(buf.size(), static_cast<std::size_t>(dot_cols));
  std::copy(buf.end() - static_cast<std::ptrdiff_t>(copy_len), buf.end(),
            next.begin() + (static_cast<std::size_t>(dot_cols) - copy_len));
  return next;
}

class TerrainDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float>, std::uint64_t, CellGrid& grid) override {
    const int height    = grid.rows();
    const int dot_rows  = height * 4;
    const int dot_cols  = grid.cols() * 2;
    // Cache the panel size for tick() (cliamp reads v.Rows / PanelWidth live).
    rows_ = height;
    cols_ = grid.cols();
    // cliamp Render: a local (possibly freshly-resized) view of the buffer;
    // the resized copy is NOT stored back — only Tick stores it.
    const std::vector<double> buf = resize_terrain_buf(buf_, dot_cols);

    // Render: each dot column is filled from its terrain height down to the
    // bottom. Each terminal line carries the spectrum color of its row-bottom
    // tier (cliamp specWrap).
    for (int row = 0; row < height; ++row) {
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      for (int ch = 0; ch < grid.cols(); ++ch) {
        std::uint32_t braille = 0x2800;
        for (int dc = 0; dc < 2; ++dc) {
          const int x = ch * 2 + dc;
          const double terrain_h = buf[static_cast<std::size_t>(x)];
          // Top dot position — invert so 0 is bottom.
          const int top_dot =
              dot_rows - 1 - static_cast<int>(terrain_h * static_cast<double>(dot_rows - 1));
          for (int dr = 0; dr < 4; ++dr) {
            const int dot_y = row * 4 + dr;
            if (dot_y >= top_dot) {
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                     [static_cast<std::size_t>(dc)];
            }
          }
        }
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

  void tick(const VisTickContext& ctx, std::uint64_t& frame,
            std::span<const float> bands) override {
    // cliamp terrainDriver.Tick: the framework's defaultDriverTick already
    // ran; under an overlay the buffer stays frozen.
    if (ctx.overlay_active) {
      return;
    }

    const int dot_cols = cols_ * 2;
    buf_ = resize_terrain_buf(buf_, dot_cols);
    if (static_cast<int>(buf_.size()) < 2) {
      return;
    }

    // Scroll left by 2 dot columns per frame for visible movement.
    std::copy(buf_.begin() + 2, buf_.end(), buf_.begin());

    // Compute the new rightmost height from the average smoothed spectrum
    // energy so successive scrolled columns glide instead of stepping at the
    // FFT rate.
    double total_energy = 0.0;
    for (const float e : bands) {
      total_energy += static_cast<double>(e);
    }
    const double avg = total_energy / static_cast<double>(std::max(1, static_cast<int>(bands.size())));

    // Two new columns with slight noise for organic ridge edges.
    buf_[static_cast<std::size_t>(dot_cols - 2)] =
        std::min(1.0, avg + scatter_hash(0, 0, 0, frame) * 0.12);
    buf_[static_cast<std::size_t>(dot_cols - 1)] =
        std::min(1.0, avg + scatter_hash(0, 0, 1, frame) * 0.12);
  }

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp defaultDriverTickInterval.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }

  void on_enter() override {}  // cliamp: terrain keeps its buffer across mode switches
  void on_leave() override {}

private:
  std::vector<double> buf_;
  int                 rows_ = 0;  // cached from the last render()
  int                 cols_ = 0;
};

}  // namespace

std::unique_ptr<VisDriver> make_terrain_driver() {
  return std::make_unique<TerrainDriver>();
}

}  // namespace bootamp::ui::vis_drivers
