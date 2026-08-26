// tests/ui/test_vis_bricks_group.cpp — driver tests for the bricks, mosaic,
// matrix, rain, and retro visualizers.
//
// Ports the cliamp driver behaviors (vis_bricks.go / vis_mosaic.go /
// vis_matrix.go / vis_rain.go / vis_retro.go): analysis specs, tick cadence
// (Bricks opts into the animation cadence; the others use the default driver
// interval), deterministic rendering, per-driver structure (brick rows and
// tiers, matrix/rain character alphabets + energy density, retro braille
// scene with wave/sun/grid priority colors), and the mosaic state machine
// (seeded cell generation, ignite/decay lifecycle, on_enter reset). Drivers
// are created through the real factories (all_vis_modes registry).
#include "ui/vis_drivers/registry.hpp"

#include "ui/cell.hpp"
#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace bootamp::ui;

namespace {

using Clock = std::chrono::steady_clock;
namespace vd = bootamp::ui::vis_drivers;

VisTickContext playing_ctx() {
  VisTickContext ctx;
  ctx.now     = Clock::time_point(std::chrono::milliseconds(1000));
  ctx.playing = true;
  return ctx;
}

VisTickContext idle_ctx() {
  VisTickContext ctx;
  ctx.now     = Clock::time_point(std::chrono::milliseconds(1000));
  ctx.playing = false;
  return ctx;
}

std::vector<float> uniform_bands(std::size_t n, float v) {
  return std::vector<float>(n, v);
}

// count_cells returns how many cells in `grid` satisfy `pred`.
template <typename Pred>
std::size_t count_cells(const CellGrid& grid, Pred pred) {
  std::size_t n = 0;
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (pred(grid.at(r, c))) {
        ++n;
      }
    }
  }
  return n;
}

bool is_ascii_space(const Cell& c) { return c.rune == U' '; }

}  // namespace

// ---------------------------------------------------------------------------
// Analysis spec + tick cadence (cliamp visModes dispatch entries)
// ---------------------------------------------------------------------------

TEST_CASE("bricks/mosaic/matrix/rain/retro declare the default 10-band spec") {
  const std::array<std::unique_ptr<VisDriver>, 5> drivers = {
      vd::make_bricks_driver(), vd::make_mosaic_driver(),
      vd::make_matrix_driver(), vd::make_rain_driver(),
      vd::make_retro_driver()};
  for (const auto& driver : drivers) {
    REQUIRE(driver != nullptr);
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    const auto spec = driver->analysis_spec();
    REQUIRE(spec.band_count == 10);
    REQUIRE(spec.fft_size == 2048);
    // Band-driven drivers: the framework decides paused settling.
    REQUIRE(driver->pause_settled());
  }
}

TEST_CASE("bricks ticks at the animation cadence while playing") {
  auto driver = vd::make_bricks_driver();
  // cliamp newFastRenderOnlyDriver(TickAnim).
  REQUIRE(driver->tick_interval(playing_ctx()) == kTickAnim);
  REQUIRE(driver->tick_interval(idle_ctx()) == kTickSlow);

  VisTickContext overlay = playing_ctx();
  overlay.overlay_active = true;
  REQUIRE(driver->tick_interval(overlay) == kTickSlow);
}

TEST_CASE("mosaic/matrix/rain/retro use the default driver interval") {
  const std::array<std::unique_ptr<VisDriver>, 4> drivers = {
      vd::make_mosaic_driver(), vd::make_matrix_driver(),
      vd::make_rain_driver(), vd::make_retro_driver()};
  for (const auto& driver : drivers) {
    // cliamp defaultDriverTickInterval: fast only while playing live.
    REQUIRE(driver->tick_interval(playing_ctx()) == kTickFast);
    REQUIRE(driver->tick_interval(idle_ctx()) == kTickSlow);

    VisTickContext overlay = playing_ctx();
    overlay.overlay_active = true;
    REQUIRE(driver->tick_interval(overlay) == kTickSlow);
  }
}

// ---------------------------------------------------------------------------
// Bricks (cliamp vis_bricks.go renderBricks)
// ---------------------------------------------------------------------------

TEST_CASE("bricks fills brick cells above the row threshold") {
  auto driver = vd::make_bricks_driver();

  CellGrid loud(5, 40);
  driver->render(uniform_bands(10, 1.0f), 0, loud);
  // level 1.0 > every row threshold -> every cell is a half-block brick.
  REQUIRE(count_cells(loud, [](const Cell& c) { return c.rune == U'▄'; }) == 5 * 40);
  // Every line carries the spectrum color of its row-bottom tier (Go specWrap):
  // top rows high, bottom rows low.
  REQUIRE(loud.at(0, 0).color == kColorSpecHigh);   // threshold 0.8
  REQUIRE(loud.at(1, 0).color == kColorSpecHigh);   // threshold 0.6
  REQUIRE(loud.at(2, 0).color == kColorSpecMid);    // threshold 0.4
  REQUIRE(loud.at(3, 0).color == kColorSpecLow);    // threshold 0.2
  REQUIRE(loud.at(4, 0).color == kColorSpecLow);    // threshold 0.0

  CellGrid quiet(5, 40);
  driver->render(uniform_bands(10, 0.0f), 0, quiet);
  // level 0.0 > threshold never holds -> all spaces (color tier still applied).
  REQUIRE(count_cells(quiet, is_ascii_space) == 5 * 40);
  REQUIRE(quiet.at(0, 0).color == kColorSpecHigh);
}

TEST_CASE("bricks respects band widths and inter-band gaps") {
  auto driver = vd::make_bricks_driver();

  // Narrow panel: visBandWidth hands the leading bands a single column each.
  CellGrid grid(3, 4);
  driver->render(uniform_bands(10, 1.0f), 0, grid);
  REQUIRE(grid.at(0, 0).rune == U'▄');  // band 0
  REQUIRE(grid.at(0, 1).rune == U' ');  // gap
  REQUIRE(grid.at(0, 2).rune == U'▄');  // band 1
  REQUIRE(grid.at(0, 3).rune == U' ');  // gap

  // A 1-column panel gets exactly one visible band, no gap.
  CellGrid narrow(3, 1);
  driver->render(uniform_bands(10, 1.0f), 0, narrow);
  REQUIRE(narrow.at(0, 0).rune == U'▄');
}

TEST_CASE("bricks rendering is deterministic") {
  auto driver = vd::make_bricks_driver();
  CellGrid a(5, 40), b(5, 40);
  const std::vector<float> bands = {0.0f, 0.1f, 0.3f, 0.5f, 0.7f,
                                    0.9f, 0.4f, 0.2f, 0.8f, 0.6f};
  driver->render(bands, 7, a);
  driver->render(bands, 7, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
}

// ---------------------------------------------------------------------------
// Matrix (cliamp vis_matrix.go renderMatrix)
// ---------------------------------------------------------------------------

TEST_CASE("matrix renders only matrix characters and spaces") {
  auto driver = vd::make_matrix_driver();
  CellGrid grid(5, 40);
  driver->render(uniform_bands(10, 1.0f), 0, grid);  // all columns active
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      const char32_t ch = grid.at(r, c).rune;
      const bool is_char =
          (ch >= U'ｦ' && ch <= U'ﾄ') || (ch >= U'0' && ch <= U'9');
      REQUIRE((is_char || ch == U' '));
      // Drop-position colors: head high, upper trail mid, lower trail low.
      REQUIRE((grid.at(r, c).color == kColorSpecHigh ||
               grid.at(r, c).color == kColorSpecMid ||
               grid.at(r, c).color == kColorSpecLow ||
               grid.at(r, c).color == kColorDefault));
    }
  }
}

TEST_CASE("matrix energy raises the active-column density") {
  auto driver = vd::make_matrix_driver();
  CellGrid loud(5, 40), quiet(5, 40);
  driver->render(uniform_bands(10, 1.0f), 100, loud);
  driver->render(uniform_bands(10, 0.0f), 100, quiet);

  // Gate: scatterHash > energy*1.5+0.1 — 1.0 keeps ~all columns active,
  // 0.0 leaves only the ~10% with hash <= 0.1.
  const std::size_t loud_chars  = count_cells(loud, [](const Cell& c) { return c.rune != U' '; });
  const std::size_t quiet_chars = count_cells(quiet, [](const Cell& c) { return c.rune != U' '; });
  REQUIRE(loud_chars > 0);
  REQUIRE(quiet_chars > 0);
  REQUIRE(loud_chars > quiet_chars * 4);
  // The gate cadence (frame/20) advances the pattern over frames.
  CellGrid other(5, 40);
  driver->render(uniform_bands(10, 0.0f), 120, other);
  REQUIRE(dump_grid(other) != dump_grid(quiet));
}

TEST_CASE("matrix rendering is deterministic for a given frame") {
  auto driver = vd::make_matrix_driver();
  CellGrid a(5, 40), b(5, 40);
  const std::vector<float> bands = {0.9f, 0.7f, 0.5f, 0.3f, 0.1f,
                                    0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
  driver->render(bands, 42, a);
  driver->render(bands, 42, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
}

// ---------------------------------------------------------------------------
// Rain (cliamp vis_rain.go renderRain)
// ---------------------------------------------------------------------------

TEST_CASE("rain renders only drop characters and spaces") {
  auto driver = vd::make_rain_driver();
  CellGrid grid(5, 40);
  driver->render(uniform_bands(10, 1.0f), 0, grid);  // bars cover all rows
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      const char32_t ch = grid.at(r, c).rune;
      REQUIRE((ch == U'┃' || ch == U'│' || ch == U':' || ch == U' '));
      REQUIRE((grid.at(r, c).color == kColorSpecHigh ||
               grid.at(r, c).color == kColorSpecMid ||
               grid.at(r, c).color == kColorSpecLow ||
               grid.at(r, c).color == kColorDefault));
    }
  }
}

TEST_CASE("rain bar height follows the band level") {
  auto driver = vd::make_rain_driver();
  // A single loud band: above the bar the cells are empty, inside the bar
  // drops fall. Level 1.0 covers every row.
  CellGrid grid(5, 20);
  const std::vector<float> one_loud = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                       0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  driver->render(one_loud, 0, grid);
  const std::size_t drops = count_cells(grid, [](const Cell& c) {
    return c.rune == U'┃' || c.rune == U'│' || c.rune == U':';
  });
  REQUIRE(drops > 0);

  // Silence renders empty.
  CellGrid quiet(5, 40);
  driver->render(uniform_bands(10, 0.0f), 0, quiet);
  REQUIRE(count_cells(quiet, is_ascii_space) == 5 * 40);
}

TEST_CASE("rain rendering is deterministic and frame-driven") {
  auto driver = vd::make_rain_driver();
  CellGrid a(5, 40), b(5, 40);
  const std::vector<float> bands = {0.8f, 0.6f, 0.4f, 0.2f, 0.1f,
                                    0.3f, 0.5f, 0.7f, 0.9f, 0.5f};
  driver->render(bands, 17, a);
  driver->render(bands, 17, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
}

// ---------------------------------------------------------------------------
// Retro (cliamp vis_retro.go renderRetro)
// ---------------------------------------------------------------------------

TEST_CASE("retro renders a braille scene with wave/sun/grid priority colors") {
  auto driver = vd::make_retro_driver();
  CellGrid grid(5, 40);
  driver->render(uniform_bands(10, 0.5f), 0, grid);
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      const char32_t ch = grid.at(r, c).rune;
      REQUIRE(ch >= U'⠀');   // U+2800..U+28FF braille block
      REQUIRE(ch <= U'⣿');
      REQUIRE((grid.at(r, c).color == kColorSpecLow ||
               grid.at(r, c).color == kColorSpecMid ||
               grid.at(r, c).color == kColorSpecHigh));
    }
  }
  // Sun (tier mid) above the horizon, wave (tier high) at the horizon on top
  // of it, grid floor (tier low) below.
  REQUIRE(grid.at(0, 9).color == kColorSpecMid);   // sun, braille col 9
  REQUIRE(grid.at(1, 9).color == kColorSpecHigh);  // wave overrides sun
  REQUIRE(grid.at(4, 10).color == kColorSpecLow);  // floor vertical line
}

TEST_CASE("retro renders the scene without band input") {
  auto driver = vd::make_retro_driver();
  // Go would panic on bands[-1] with zero bands; the port skips the wave and
  // still draws the sun + grid floor.
  CellGrid grid(5, 40);
  driver->render({}, 0, grid);
  REQUIRE(count_cells(grid, [](const Cell& c) { return c.rune != U'⠀'; }) > 0);
  REQUIRE(grid.at(0, 9).color == kColorSpecMid);   // sun still drawn
  REQUIRE(grid.at(4, 10).color == kColorSpecLow);  // floor still drawn
}

TEST_CASE("retro grid scroll advances with the frame") {
  auto driver = vd::make_retro_driver();
  CellGrid a(5, 40), b(5, 40);
  const std::vector<float> bands = {0.6f, 0.4f, 0.2f, 0.8f, 0.5f,
                                    0.3f, 0.7f, 0.9f, 0.1f, 0.4f};
  driver->render(bands, 10, a);
  driver->render(bands, 10, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
  CellGrid c(5, 40);
  driver->render(bands, 30, c);  // scroll = mod(frame*0.08, 1) = 0.4 vs 0.8
  REQUIRE(dump_grid(a) != dump_grid(c));
}

// ---------------------------------------------------------------------------
// Mosaic (cliamp vis_mosaic.go mosaicDriver)
// ---------------------------------------------------------------------------

TEST_CASE("mosaic tile count matches the panel width layout") {
  // (width + 1) / 3 tiles, each 2 chars + 1 gap, no trailing gap. With 74
  // columns: 25 tiles * 2 chars + 24 gaps = 74 written columns.
  auto driver = vd::make_mosaic_driver();
  CellGrid grid(5, 74);
  std::uint64_t frame = 0;
  VisTickContext ctx  = playing_ctx();
  driver->render(uniform_bands(10, 0.0f), 0, grid);  // generate the cells
  REQUIRE(count_cells(grid, is_ascii_space) == 5 * 74);  // unlit = all spaces
  driver->tick(ctx, frame, uniform_bands(10, 1.0f));     // ignite everything
  driver->render(uniform_bands(10, 1.0f), 0, grid);
  // Tile: two identical glyphs; gap: one default-colored space.
  for (int t = 0; t < 25; ++t) {
    const int col = t * 3;
    REQUIRE(grid.at(0, col).rune == U'█');
    REQUIRE(grid.at(0, col + 1).rune == U'█');
    REQUIRE(grid.at(0, col).color == kColorSpecHigh);  // overdrive tier
    if (t < 24) {
      REQUIRE(grid.at(0, col + 2).rune == U' ');
      REQUIRE(grid.at(0, col + 2).color == kColorDefault);
    }
  }
}

TEST_CASE("mosaic cell generation is deterministic and reshuffled per visit") {
  // Mixed bands: cells wired to loud bands ignite while quiet-wired cells
  // stay dark, so the dump carries the actual cell wiring (thresholds +
  // band assignments), making dump equality meaningful.
  const std::vector<float> bands = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f,
                                    0.4f, 0.3f, 0.2f, 0.1f, 0.05f};
  std::uint64_t frame = 0;
  VisTickContext ctx  = playing_ctx();

  auto driver_a = vd::make_mosaic_driver();
  auto driver_b = vd::make_mosaic_driver();
  CellGrid a(5, 40), b(5, 40), c(5, 40);
  driver_a->render(bands, 0, a);  // generate (unlit)
  driver_a->tick(ctx, frame, bands);
  driver_a->render(bands, 0, a);
  driver_b->render(bands, 0, b);
  driver_b->tick(ctx, frame, bands);
  driver_b->render(bands, 0, b);
  REQUIRE(dump_grid(a) == dump_grid(b));  // same seed -> same assignments
  // The scattered pattern is real: some tiles lit, some dark.
  const std::size_t lit = count_cells(a, [](const Cell& c) { return c.rune != U' '; });
  REQUIRE(lit > 0);
  REQUIRE(lit < static_cast<std::size_t>(5 * 40));

  // OnEnter forces a fresh generation; the seed is the same, so a fresh
  // driver produces the same grid again (Go reshuffles deterministically).
  driver_a->on_enter();
  driver_a->render(bands, 0, c);
  driver_a->tick(ctx, frame, bands);
  driver_a->render(bands, 0, c);
  REQUIRE(dump_grid(a) == dump_grid(c));
}

TEST_CASE("mosaic ignites on loud bands and decays to rest in silence") {
  auto driver = vd::make_mosaic_driver();

  CellGrid grid(5, 40);
  driver->render(uniform_bands(10, 0.0f), 0, grid);  // generate cells (unlit)
  REQUIRE(count_cells(grid, is_ascii_space) == 5 * 40);

  // Loud bands: every cell threshold is <= 0.78 < 1.0, so all ignite to
  // min(1.0, 1.05) and decay once -> 0.88 -> the '█' overdrive tier.
  std::uint64_t frame = 0;
  VisTickContext ctx  = playing_ctx();
  driver->tick(ctx, frame, uniform_bands(10, 1.0f));
  driver->render(uniform_bands(10, 1.0f), 0, grid);
  REQUIRE(count_cells(grid, [](const Cell& c) { return c.rune == U'█'; }) > 0);

  // Silence: cells decay by 0.88 per tick and floor at zero. 0.88^31 < 0.05
  // -> every tile drops below the '░' tier.
  const std::vector<float> silence(10, 0.0f);
  for (int i = 0; i < 31; ++i) {
    driver->tick(ctx, frame, silence);
  }
  driver->render(silence, 0, grid);
  REQUIRE(count_cells(grid, is_ascii_space) == 5 * 40);
}

TEST_CASE("mosaic bright cells track their assigned band") {
  // Cells wired to loud bands light up; cells wired to quiet bands do not —
  // the per-cell threshold/band wiring produces a scattered, density-driven
  // pattern.
  auto driver = vd::make_mosaic_driver();
  CellGrid lit(5, 40);
  std::uint64_t frame = 0;
  VisTickContext ctx  = playing_ctx();
  driver->render(uniform_bands(10, 0.0f), 0, lit);  // generate the cells
  driver->tick(ctx, frame, uniform_bands(10, 0.9f));  // ignite
  driver->render(uniform_bands(10, 0.9f), 0, lit);
  const std::size_t loud_lit = count_cells(lit, [](const Cell& c) { return c.rune != U' '; });

  // Quiet bands (0.1): only the ~8% of cells with a threshold < 0.1 keep
  // re-igniting (0.1*0.88 = 0.088 -> '░'); everything else decays 0.88^26
  // below the 0.05 floor -> dark. Strictly less lit than the loud pass.
  CellGrid dim(5, 40);
  for (int i = 0; i < 26; ++i) {
    driver->tick(ctx, frame, uniform_bands(10, 0.1f));
  }
  driver->render(uniform_bands(10, 0.1f), 0, dim);
  const std::size_t quiet_lit = count_cells(dim, [](const Cell& c) { return c.rune != U' '; });

  REQUIRE(loud_lit > 0);
  REQUIRE(quiet_lit < loud_lit);
}

TEST_CASE("mosaic overlay ticks skip the decay pass (Go OverlayActive)") {
  auto driver = vd::make_mosaic_driver();
  CellGrid grid(5, 40);
  std::uint64_t frame = 0;
  VisTickContext ctx  = playing_ctx();
  driver->render(uniform_bands(10, 0.0f), 0, grid);  // generate the cells
  driver->tick(ctx, frame, uniform_bands(10, 1.0f));  // ignite
  driver->render(uniform_bands(10, 1.0f), 0, grid);
  REQUIRE(count_cells(grid, [](const Cell& c) { return c.rune == U'█'; }) > 0);
  const std::string before = dump_grid(grid);

  VisTickContext overlay = playing_ctx();
  overlay.overlay_active = true;
  driver->tick(overlay, frame, uniform_bands(10, 1.0f));
  driver->render(uniform_bands(10, 1.0f), 0, grid);
  REQUIRE(dump_grid(grid) == before);  // no ignite, no decay
}
