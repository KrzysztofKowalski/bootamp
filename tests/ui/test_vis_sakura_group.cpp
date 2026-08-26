// tests/ui/test_vis_sakura_group.cpp — golden/behavioral tests for the
// sakura, sand, terrain, firefly, and flame drivers.
//
// Ports of the cliamp/ui Go test cases that exercise these five drivers:
//   * vis_sakura.go      — renderSakura (render-only, pure in bands+frame)
//   * vis_sand.go        — TestPausedSandWaitsForExplosion
//   * vis_terrain.go     — TestTerrainPreservesStateAcrossModeSwitch,
//                          TestTerrainRenderDoesNotAdvanceWithoutTick,
//                          TestTerrainTickSkipsAnalyzeUnderOverlay
//   * vis_firefly.go     — renderFirefly (render-only)
//   * vis_flame.go       — flameDriver tick/render + OnEnter reset
//   * new_vis_smoke_test.go — per-mode render sweep (every mode must render a
//                             non-empty grid at 5 rows without panicking)
//
// The drivers are created through the real factories and driven directly
// (tick + render), matching how the framework drives them. The stateful
// drivers (sand, terrain, flame) learn the panel size from the last render()
// call — the VisDriver contract gives tick() no grid — so every ticked
// sequence starts with one render() that establishes the dot-grid dimensions
// (cliamp reads v.Rows / PanelWidth live; the framework's first tick before
// any render is a no-op).
#include "ui/visualizer.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace bootamp::ui;
using bootamp::dsp::kDefaultSpectrumBands;

namespace {

VisTickContext playing_ctx() {
  VisTickContext ctx;
  ctx.playing = true;
  return ctx;
}

std::vector<float> const_bands(float v) {
  return std::vector<float>(kDefaultSpectrumBands, v);
}

std::span<const float> span_of(const std::vector<float>& v) {
  return std::span<const float>(v);
}

std::size_t dot_cell_count(const CellGrid& grid) {
  std::size_t n = 0;
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).rune != U'⠀') {  // any braille dot set
        ++n;
      }
    }
  }
  return n;
}

std::size_t color_cell_count(const CellGrid& grid, Color color) {
  std::size_t n = 0;
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).color == color) {
        ++n;
      }
    }
  }
  return n;
}

// Ticks `driver` with the given per-frame band levels, advancing the frame
// counter exactly like the framework (one step per non-overlay tick).
void run_ticked(std::unique_ptr<VisDriver>& driver,
                const std::vector<float>& levels,
                std::uint64_t& frame) {
  VisTickContext ctx = playing_ctx();
  for (const float lv : levels) {
    ++frame;
    driver->tick(ctx, frame, span_of(const_bands(lv)));
  }
}

// Driver-level assertions shared by all five drivers (cliamp visModes table:
// every band-driven mode uses spectrumAnalysisSpec(DefaultSpectrumBands) and
// defaultDriverTickInterval).
void check_band_spec_interval(VisDriver& driver) {
  const auto spec = driver.analysis_spec();
  REQUIRE(spec.band_count == 10);
  REQUIRE(spec.fft_size == 2048);

  VisTickContext ctx = playing_ctx();
  REQUIRE(driver.tick_interval(ctx) == kTickFast);
  ctx.playing = false;
  REQUIRE(driver.tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver.tick_interval(ctx) == kTickSlow);
}

}  // namespace

// ---------------------------------------------------------------------------
// Sakura
// ---------------------------------------------------------------------------

TEST_CASE("sakura driver declares the default 10-band spec and interval") {
  auto driver = vis_drivers::make_sakura_driver();
  REQUIRE(driver != nullptr);
  check_band_spec_interval(*driver);
  REQUIRE(driver->pause_settled());  // render-only: framework decides settling
}

TEST_CASE("sakura renders deterministically and louder energy adds petals") {
  auto driver = vis_drivers::make_sakura_driver();

  CellGrid loud_a(5, 16), loud_b(5, 16), quiet(5, 16);
  const auto hi = const_bands(0.9f);
  const auto lo = const_bands(0.05f);
  driver->render(span_of(hi), 37, loud_a);
  driver->render(span_of(hi), 37, loud_b);
  driver->render(span_of(lo), 37, quiet);

  REQUIRE(dump_grid(loud_a) == dump_grid(loud_b));  // deterministic per frame

  // 12 petals at silence, up to 28 when loud — more petals, more dots.
  const std::size_t loud_dots  = dot_cell_count(loud_a);
  const std::size_t quiet_dots = dot_cell_count(quiet);
  REQUIRE(loud_dots > 0);
  REQUIRE(quiet_dots > 0);
  REQUIRE(loud_dots > quiet_dots);

  // Per-line spectrum tiers (cliamp specWrap): top row high, bottom row low.
  REQUIRE(loud_a.at(0, 0).color == kColorSpecHigh);
  REQUIRE(loud_a.at(4, 0).color == kColorSpecLow);
  REQUIRE(quiet.at(0, 0).color == kColorSpecHigh);
  REQUIRE(quiet.at(4, 0).color == kColorSpecLow);
}

TEST_CASE("sakura renders a blank grid for degenerate sizes") {
  auto driver = vis_drivers::make_sakura_driver();
  CellGrid tiny(1, 1);  // dotRows 4, dotCols 2 -> too narrow
  const auto hi = const_bands(0.9f);
  driver->render(span_of(hi), 0, tiny);
  REQUIRE(dot_cell_count(tiny) == 0);
}

// ---------------------------------------------------------------------------
// Sand
// ---------------------------------------------------------------------------

TEST_CASE("sand driver declares the default 10-band spec and interval") {
  auto driver = vis_drivers::make_sand_driver();
  REQUIRE(driver != nullptr);
  check_band_spec_interval(*driver);
  REQUIRE(driver->pause_settled());  // no explosion yet
}

TEST_CASE("sand simulates deterministically for an identical tick stream") {
  auto a = vis_drivers::make_sand_driver();
  auto b = vis_drivers::make_sand_driver();
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);

  // Script: mixed levels that spawn grains, fire transient bumps (0.2 -> 0.5,
  // 0.4 -> 0.7) and sustained rumble (bass > 0.3) without ever reaching the
  // 30% fill that triggers the explosion.
  const std::vector<float> levels = {0.5f, 0.2f, 0.5f, 0.2f, 0.7f,
                                     0.4f, 0.7f, 0.4f, 0.5f, 0.3f};

  auto run = [&levels](std::unique_ptr<VisDriver>& driver) {
    CellGrid grid(16, 5);
    const auto zeros = const_bands(0.0f);
    driver->render(span_of(zeros), 0, grid);  // establish dot-grid dims
    std::uint64_t frame = 0;
    run_ticked(driver, levels, frame);
    driver->render(span_of(zeros), frame, grid);
    return grid;
  };

  const CellGrid ga = run(a);
  const CellGrid gb = run(b);
  REQUIRE(dot_cell_count(ga) > 0);  // grains actually poured and fell
  REQUIRE(dump_grid(ga) == dump_grid(gb));  // same LCG stream -> same sand
}

TEST_CASE("sand bass kick blows an overfull bed into particles, then settles") {
  // Port of cliamp TestPausedSandWaitsForExplosion: a live explosion keeps
  // the driver un-settled until every particle has left the panel.
  auto driver = vis_drivers::make_sand_driver();

  CellGrid grid(16, 5);
  const auto zeros = const_bands(0.0f);
  driver->render(span_of(zeros), 0, grid);  // establish 20x32 dot grid
  std::uint64_t frame = 0;

  // Pour at full volume: ~7 grains/frame, well past the 30% fill threshold.
  for (int i = 0; i < 60; ++i) {
    ++frame;
    driver->tick(playing_ctx(), frame, span_of(const_bands(0.9f)));
  }
  // One quiet tick (bass drops), then a bass kick: delta > 0.06 over a bed
  // past 30% fill -> every grain becomes a ballistic particle.
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(zeros));
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.9f)));

  REQUIRE_FALSE(driver->pause_settled());  // grains are in flight

  // Keep ticking (cliamp paused decay keeps ticking until settled); the
  // particles rise, peak, and fall off the panel within a few dozen frames.
  int n = 0;
  for (; n < 40 && !driver->pause_settled(); ++n) {
    driver->tick(playing_ctx(), frame, span_of(zeros));
  }
  REQUIRE(driver->pause_settled());
  REQUIRE(n < 30);  // settled well inside the safety TTL

  // The bed restarted empty: nothing left to render.
  CellGrid out(16, 5);
  driver->render(span_of(zeros), frame, out);
  REQUIRE(dot_cell_count(out) == 0);
}

// ---------------------------------------------------------------------------
// Terrain
// ---------------------------------------------------------------------------

TEST_CASE("terrain driver declares the default 10-band spec and interval") {
  auto driver = vis_drivers::make_terrain_driver();
  REQUIRE(driver != nullptr);
  check_band_spec_interval(*driver);
  REQUIRE(driver->pause_settled());
}

TEST_CASE("terrain render does not advance the buffer without a tick") {
  // Port of cliamp TestTerrainRenderDoesNotAdvanceWithoutTick: redraws are
  // pure reads of the buffer.
  auto driver = vis_drivers::make_terrain_driver();
  CellGrid grid(16, 5);
  const auto zeros = const_bands(0.0f);
  driver->render(span_of(zeros), 0, grid);  // establish width
  std::uint64_t frame = 0;
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.6f)));

  CellGrid a(16, 5), b(16, 5);
  driver->render(span_of(zeros), frame, a);
  driver->render(span_of(zeros), frame, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
}

TEST_CASE("terrain tick scrolls the ridge left and writes new columns") {
  auto driver = vis_drivers::make_terrain_driver();
  CellGrid grid(16, 5);
  const auto zeros = const_bands(0.0f);
  driver->render(span_of(zeros), 0, grid);  // establish width
  std::uint64_t frame = 0;

  // Before any tick the buffer is empty: terrain height 0 keeps only the
  // bottom dot of each column lit.
  CellGrid before(16, 5);
  driver->render(span_of(zeros), frame, before);
  const char32_t bottom_dot_rune = before.at(4, 15).rune;

  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.6f)));
  CellGrid after1(16, 5);
  driver->render(span_of(zeros), frame, after1);
  // 0.6-height terrain fills the rightmost columns far above the ground dot
  // (cliamp: height 0.6 -> topDot ~7 of 20). The new rune is a strict
  // superset of the bottom-dot pattern.
  REQUIRE(after1.at(4, 15).rune > bottom_dot_rune);

  // A second tick re-seeds the rightmost columns with a fresh frame's noise
  // and scrolls the first pair left: the frame content changes.
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.6f)));
  CellGrid after2(16, 5);
  driver->render(span_of(zeros), frame, after2);
  REQUIRE(dump_grid(after1) != dump_grid(after2));

  // Rendering again without a tick must not advance (buffer preserved).
  CellGrid after2b(16, 5);
  driver->render(span_of(zeros), frame, after2b);
  REQUIRE(dump_grid(after2) == dump_grid(after2b));
}

TEST_CASE("terrain tick skips the buffer update under an overlay") {
  // Port of cliamp TestTerrainTickSkipsAnalyzeUnderOverlay.
  auto driver = vis_drivers::make_terrain_driver();
  CellGrid grid(16, 5);
  const auto zeros = const_bands(0.0f);
  driver->render(span_of(zeros), 0, grid);  // establish width
  std::uint64_t frame = 0;
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.6f)));
  ++frame;
  driver->tick(playing_ctx(), frame, span_of(const_bands(0.6f)));

  CellGrid snap(16, 5);
  driver->render(span_of(zeros), frame, snap);

  VisTickContext overlay = playing_ctx();
  overlay.overlay_active = true;
  ++frame;
  driver->tick(overlay, frame, span_of(const_bands(0.9f)));

  CellGrid after(16, 5);
  driver->render(span_of(zeros), frame, after);
  REQUIRE(dump_grid(after) == dump_grid(snap));  // buffer unchanged
}

// ---------------------------------------------------------------------------
// Firefly
// ---------------------------------------------------------------------------

TEST_CASE("firefly driver declares the default 10-band spec and interval") {
  auto driver = vis_drivers::make_firefly_driver();
  REQUIRE(driver != nullptr);
  check_band_spec_interval(*driver);
  REQUIRE(driver->pause_settled());
}

TEST_CASE("firefly renders deterministically with grass and fireflies") {
  auto driver = vis_drivers::make_firefly_driver();

  CellGrid a(5, 16), b(5, 16);
  const auto quiet = const_bands(0.05f);
  driver->render(span_of(quiet), 11, a);
  driver->render(span_of(quiet), 11, b);
  REQUIRE(dump_grid(a) == dump_grid(b));  // deterministic per frame

  // Grass silhouette: the bottom row has dots at x=0 (cliamp: height 4 there).
  REQUIRE(a.at(4, 0).rune != U'⠀');
  REQUIRE(dot_cell_count(a) > 0);
}

TEST_CASE("firefly high-frequency energy brightens the population") {
  auto driver = vis_drivers::make_firefly_driver();

  CellGrid quiet(5, 16), loud(5, 16);
  driver->render(span_of(const_bands(0.05f)), 11, quiet);
  driver->render(span_of(const_bands(0.9f)), 11, loud);

  // Bright (lit) flies paint the high tier; high band energy raises both the
  // lit probability and the population brightness (cliamp renderFirefly).
  const std::size_t quiet_bright = color_cell_count(quiet, kColorSpecHigh);
  const std::size_t loud_bright  = color_cell_count(loud, kColorSpecHigh);
  REQUIRE(loud_bright > 0);
  REQUIRE(loud_bright > quiet_bright);
}

// ---------------------------------------------------------------------------
// Flame
// ---------------------------------------------------------------------------

TEST_CASE("flame driver declares the default 10-band spec and interval") {
  auto driver = vis_drivers::make_flame_driver();
  REQUIRE(driver != nullptr);
  check_band_spec_interval(*driver);
  REQUIRE(driver->pause_settled());  // no heat decay state (framework settles)
}

TEST_CASE("flame heat evolution is deterministic for an identical tick stream") {
  auto a = vis_drivers::make_flame_driver();
  auto b = vis_drivers::make_flame_driver();

  auto run = [](std::unique_ptr<VisDriver>& driver) {
    CellGrid grid(16, 5);
    const auto zeros = const_bands(0.0f);
    driver->render(span_of(zeros), 0, grid);  // establish 20x32 heat field
    std::uint64_t frame = 0;
    for (int i = 0; i < 8; ++i) {
      ++frame;
      driver->tick(playing_ctx(), frame,
                   span_of((i % 2 == 0) ? const_bands(0.7f) : const_bands(0.2f)));
    }
    driver->render(span_of(zeros), frame, grid);
    return grid;
  };

  const CellGrid ga = run(a);
  const CellGrid gb = run(b);
  REQUIRE(dot_cell_count(ga) > 0);
  REQUIRE(dump_grid(ga) == dump_grid(gb));  // same LCG stream -> same flame
}

TEST_CASE("flame sustained bass feeds a taller flame, on_enter resets the bed") {
  auto tall = vis_drivers::make_flame_driver();
  auto fresh = vis_drivers::make_flame_driver();

  const auto zeros = const_bands(0.0f);

  // One tick after a reset only the source row plus one decayed row are lit.
  CellGrid g1(16, 5);
  fresh->render(span_of(zeros), 0, g1);
  std::uint64_t f1 = 0;
  run_ticked(fresh, {0.9f}, f1);
  fresh->render(span_of(zeros), f1, g1);
  const std::size_t dots_after_1 = dot_cell_count(g1);
  REQUIRE(dots_after_1 > 0);

  // Twelve ticks of sustained bass let the flame climb most of the panel.
  CellGrid g12(16, 5);
  tall->render(span_of(zeros), 0, g12);
  std::uint64_t f12 = 0;
  const std::vector<float> twelve(12, 0.9f);
  run_ticked(tall, twelve, f12);
  tall->render(span_of(zeros), f12, g12);
  const std::size_t dots_after_12 = dot_cell_count(g12);
  REQUIRE(dots_after_12 > dots_after_1);  // heat climbed, not just the base

  // on_enter resets the heat bed: the next tick re-seeds only the base.
  tall->on_enter();
  CellGrid g_reset(16, 5);
  std::uint64_t fr = f12;
  run_ticked(tall, {0.9f}, fr);
  tall->render(span_of(zeros), fr, g_reset);
  REQUIRE(dot_cell_count(g_reset) < dots_after_12);
}
