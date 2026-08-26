// tests/ui/test_vis_bars_group.cpp — tests for the bars-family drivers
// (Bars, BarsDot, BarsOutline, Columns) and the ClassicPeak driver.
//
// Two halves:
//  1. Golden harness: fixed bands + fixed frame -> deterministic dump_grid,
//     compared against tests/golden/vis/<mode>.txt. The goldens are generated
//     later (from the C++ drivers, same inputs as below); until a golden file
//     exists the section is skipped — the exact frame content is documented
//     in each TEST_CASE so the golden generation is reproducible.
//  2. Direct tests: ports of cliamp/ui/vis_classic_peak_test.go (physics
//     state machine) plus geometry/color assertions for the render-only
//     drivers (Go has no *_test.go for bars/bars_dot/bars_outline/columns).
//
// Epsilon note: Go's tests compare against float64 bands; bootamp drivers
// receive float32 spans, so band-derived values carry ~1e-9 relative float32
// error. Seeded state (repeatedClassicPeakSlice equivalents) is compared
// exactly; band-derived positions use kTestEpsilon = 1e-6.
#include "ui/cell.hpp"
#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_driver.hpp"
#include "ui/vis_drivers/classic_peak.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace bootamp::ui;
using bootamp::ui::vis_drivers::ClassicPeakDriver;
using bootamp::ui::vis_drivers::kClassicPeakVisibleEpsilon;

namespace {

using Clock = std::chrono::steady_clock;

// Go classicPeakTestEpsilon was 1e-9 for float64 bands; float32 band input
// widens it (see header note).
constexpr double kTestEpsilon = 1e-6;

// One Go tickClassicPeak step (time.Second/60) as a duration for advance().
// Go's time.Duration is int64 nanoseconds, so time.Second/60 truncates to
// 16,666,666 ns; matching that keeps the C++ advance() dt bit-identical to Go.
constexpr auto kTickClassicPeak = std::chrono::nanoseconds(1000000000LL / 60);

// Golden file path: tests/golden/vis/<mode>.txt, resolved from this source
// file (CMake compiles tests with absolute source paths).
std::filesystem::path golden_path(std::string_view mode) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "golden" / "vis" / (std::string(mode) + ".txt");
}

std::optional<std::string> read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// check_golden compares the grid dump with tests/golden/vis/<mode>.txt.
// The goldens are generated later (fixed bands + fixed frame -> deterministic
// dump_grid); until the file exists the section is skipped with a comment
// explaining that goldens are produced in a follow-up step.
void check_golden(std::string_view mode, const CellGrid& grid) {
  const std::string dump = dump_grid(grid);
  const auto path        = golden_path(mode);
  if (!std::filesystem::exists(path)) {
    SKIP("golden " << path.string()
                   << " not generated yet (goldens are produced later from "
                      "the C++ drivers; skip-until-present)");
  }
  const auto golden = read_file(path);
  REQUIRE(golden.has_value());
  REQUIRE(dump == *golden);
}

// Render-only geometry: shared band-levels for the golden frames (10 bands,
// hitting all three spectrum tiers).
const std::vector<float> kGoldenBands = {0.05f, 0.18f, 0.32f, 0.45f,
                                         0.55f, 0.68f, 0.75f, 0.85f,
                                         0.95f, 0.62f};

// Factory list for the four render-only bars-family drivers.
std::vector<std::function<std::unique_ptr<VisDriver>()>> bars_factories() {
  return {vis_drivers::make_bars_driver, vis_drivers::make_bars_dot_driver,
          vis_drivers::make_bars_outline_driver, vis_drivers::make_columns_driver};
}

// uniform_bands: Go uniformBandsN(count, level).
std::vector<float> uniform_bands(std::size_t count, float level) {
  return std::vector<float>(count, level);
}

bool contains_glyph(const CellGrid& grid, char32_t glyph) {
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).rune == glyph) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared driver contract (spec + cadence)
// ---------------------------------------------------------------------------

TEST_CASE("bars-family drivers declare the default 10-band spec and cadence") {
  for (auto& factory : bars_factories()) {
    auto driver = factory();
    REQUIRE(driver != nullptr);
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    REQUIRE(driver->analysis_spec() == VisAnalysisSpec{10, 2048});
    REQUIRE(driver->pause_settled());  // band-driven: framework decides settling

    VisTickContext ctx;
    ctx.playing = true;
    // cliamp newFastRenderOnlyDriver(TickAnim): fast while playing...
    REQUIRE(driver->tick_interval(ctx) == kTickFast);
    ctx.playing = false;
    REQUIRE(driver->tick_interval(ctx) == kTickSlow);
    ctx.playing        = true;
    ctx.overlay_active = true;
    REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  }
}

TEST_CASE("bars-family drivers render blank grids for empty bands") {
  for (auto& factory : bars_factories()) {
    auto driver = factory();
    CellGrid grid(5, 40);
    grid.fill();
    std::vector<float> none;
    driver->render(none, 0, grid);
    for (int r = 0; r < grid.rows(); ++r) {
      for (int c = 0; c < grid.cols(); ++c) {
        REQUIRE(grid.at(r, c).rune == U' ');
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Golden harness — one TEST_CASE per mode
// ---------------------------------------------------------------------------

TEST_CASE("vis golden: Bars") {
  // Frame: 10 bands {0.05,0.18,0.32,0.45,0.55,0.68,0.75,0.85,0.95,0.62},
  // 5 rows x 40 cols, frame 0. Band 0 spans 4 cols, bands 1-9 span 3, one
  // gap cell between bands (Go visBandWidth: 4+9*3+9 gaps = 40).
  auto driver = vis_drivers::make_bars_driver();
  CellGrid grid(5, 40);
  grid.fill();
  driver->render(kGoldenBands, 0, grid);
  check_golden("Bars", grid);

  // Determinism: same input -> same dump.
  CellGrid again(5, 40);
  driver->render(kGoldenBands, 0, again);
  REQUIRE(dump_grid(grid) == dump_grid(again));

  // Row colors follow the row-bottom tiers (Go specWrap).
  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);  // rowBottom 0.8
  REQUIRE(grid.at(1, 0).color == kColorSpecHigh);  // rowBottom 0.6
  REQUIRE(grid.at(2, 0).color == kColorSpecMid);   // rowBottom 0.4
  REQUIRE(grid.at(3, 0).color == kColorSpecLow);   // rowBottom 0.2
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);   // rowBottom 0

  // Geometry + fractional blocks (Go fracBlock/barBlocks), row 4 = (0, 0.2).
  REQUIRE(grid.at(4, 0).rune == U'▂');   // 0.05: frac 0.25 -> block 2
  REQUIRE(grid.at(4, 3).rune == U'▂');   // band 0 is 4 wide
  REQUIRE(grid.at(4, 4).rune == U' ');   // inter-band gap
  REQUIRE(grid.at(4, 5).rune == U'▇');   // 0.18: frac 0.9 -> block 7
  REQUIRE(grid.at(4, 36).rune == U' ');  // gap before the last band
  REQUIRE(grid.at(4, 37).rune == U'█');  // 0.62 saturates row 4
  REQUIRE(grid.at(4, 39).rune == U'█');  // band 9 spans 37..39
}

TEST_CASE("vis golden: BarsDot") {
  // Frame: same 10 bands, 5 rows x 40 cols, frame 0. Each cell maps a 4x2
  // Braille grid; dots fill bottom-up (Go renderBarsDot).
  auto driver = vis_drivers::make_bars_dot_driver();
  CellGrid grid(5, 40);
  grid.fill();
  driver->render(kGoldenBands, 0, grid);
  check_golden("BarsDot", grid);

  CellGrid again(5, 40);
  driver->render(kGoldenBands, 0, again);
  REQUIRE(dump_grid(grid) == dump_grid(again));

  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);

  // Uniform 0.8: rows 1-4 fully dotted ('⣿' = U+28FF), top row empty
  // (dotY >= 0.8 in row 0).
  CellGrid uni(5, 40);
  const auto loud = uniform_bands(10, 0.8f);
  driver->render(loud, 0, uni);
  for (int c = 0; c < uni.cols(); ++c) {
    REQUIRE(uni.at(0, c).rune == U'⠀');  // U+2800: no dots
    REQUIRE(uni.at(1, c).rune == U'⣿');
    REQUIRE(uni.at(2, c).rune == U'⣿');
    REQUIRE(uni.at(3, c).rune == U'⣿');
    REQUIRE(uni.at(4, c).rune == U'⣿');
  }

  // Uniform 0.05: only the bottom dot of the bottom row lights (0.05 < 0.05
  // is false for dotY == 0.05, so just the dot at dotY == 0: bit 0x40).
  CellGrid low_grid(5, 40);
  const auto low = uniform_bands(10, 0.05f);
  driver->render(low, 0, low_grid);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < low_grid.cols(); ++c) {
      REQUIRE(low_grid.at(r, c).rune == U'⠀');
    }
  }
  for (int c = 0; c < low_grid.cols(); ++c) {
    REQUIRE(low_grid.at(4, c).rune == U'⡀');  // U+2840: bottom dot only
  }
}

TEST_CASE("vis golden: BarsOutline") {
  // Frame: same 10 bands, 5 rows x 40 cols, frame 0. '─' only on the row
  // containing each band's peak (Go renderBarsOutline).
  auto driver = vis_drivers::make_bars_outline_driver();
  CellGrid grid(5, 40);
  grid.fill();
  driver->render(kGoldenBands, 0, grid);
  check_golden("BarsOutline", grid);

  CellGrid again(5, 40);
  driver->render(kGoldenBands, 0, again);
  REQUIRE(dump_grid(grid) == dump_grid(again));

  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);

  // Uniform 0.5: the level crosses only row 2 (rowBottom 0.4, rowTop 0.6).
  CellGrid uni(5, 40);
  const auto mid = uniform_bands(10, 0.5f);
  driver->render(mid, 0, uni);
  for (int r : {0, 1, 3, 4}) {
    for (int c = 0; c < uni.cols(); ++c) {
      REQUIRE(uni.at(r, c).rune == U' ');
    }
  }
  REQUIRE(uni.at(2, 0).rune == U'─');  // band 0 outline
  REQUIRE(uni.at(2, 4).rune == U' ');  // gap between bands stays empty
  REQUIRE(uni.at(2, 5).rune == U'─');  // band 1 outline
}

TEST_CASE("vis golden: Columns") {
  // Frame: same 10 bands, 5 rows x 40 cols, frame 0. Levels are linearly
  // interpolated across each band's columns (Go interpolateBandColumns).
  auto driver = vis_drivers::make_columns_driver();
  CellGrid grid(5, 40);
  grid.fill();
  driver->render(kGoldenBands, 0, grid);
  check_golden("Columns", grid);

  CellGrid again(5, 40);
  driver->render(kGoldenBands, 0, again);
  REQUIRE(dump_grid(grid) == dump_grid(again));

  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);

  // Alternating 0/1 bands: band 0 spans cols 0-3 with interpolated levels
  // 0, 0.25, 0.5, 0.75 — distinct from Bars, which would render four blanks.
  CellGrid alt_grid(5, 40);
  const std::vector<float> alt = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f,
                                  1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
  driver->render(alt, 0, alt_grid);
  REQUIRE(alt_grid.at(3, 0).rune == U' ');  // level 0 in row 3 (0.2, 0.4)
  REQUIRE(alt_grid.at(3, 1).rune == U'▂');  // level 0.25 -> frac 0.25
  REQUIRE(alt_grid.at(3, 2).rune == U'█');  // level 0.5 >= rowTop 0.4
  REQUIRE(alt_grid.at(3, 3).rune == U'█');  // level 0.75
}

TEST_CASE("vis golden: ClassicPeak") {
  // Frame: 5 rows x 40 cols. Sequence (deterministic, no wall clock):
  //  tick 1 at t0 with 64 bands of 0.8: bars + caps snap to 0.8 (fresh state,
  //    no animation — lastTick stays zero).
  //  tick 2 at t0+16ms with 64 bands of 0.2: bars ease down by the default
  //    1/60 s step (lastTick zero -> dt 1/60): 0.8 -> 0.708; caps stay at 0.8
  //    with gravity applied (vel -0.158), so the render shows detached '⎺'
  //    caps on row 1 above the easing bars ('▄' on row 1, '█' on rows 2-4).
  //  render with the 0.2 bands: 20 bars (classicPeakColsForWidth(40)),
  //    rowPad 1, one gap cell between bars.
  auto driver_base = vis_drivers::make_classic_peak_driver();
  auto* driver =
      dynamic_cast<vis_drivers::ClassicPeakDriver*>(driver_base.get());
  REQUIRE(driver != nullptr);
  driver->on_enter();
  CellGrid grid(5, 40);
  grid.fill();
  const auto loud  = uniform_bands(64, 0.8f);
  const auto quiet = uniform_bands(64, 0.2f);
  const auto t0    = Clock::time_point(std::chrono::seconds(1));
  VisTickContext ctx;
  ctx.now     = t0;
  ctx.playing = true;
  std::uint64_t frame = 0;

  driver->tick(ctx, frame, loud);
  ctx.now = t0 + std::chrono::milliseconds(16);
  driver->tick(ctx, frame, quiet);
  driver->render(quiet, frame, grid);
  check_golden("ClassicPeak", grid);

  // Determinism: the same sequence on a fresh driver produces the same dump.
  auto d2 = vis_drivers::make_classic_peak_driver();
  d2->on_enter();
  CellGrid grid2(5, 40);
  grid2.fill();
  VisTickContext ctx2;
  ctx2.now     = t0;
  ctx2.playing = true;
  d2->tick(ctx2, frame, loud);
  ctx2.now = t0 + std::chrono::milliseconds(16);
  d2->tick(ctx2, frame, quiet);
  d2->render(quiet, frame, grid2);
  REQUIRE(dump_grid(grid) == dump_grid(grid2));

  // Sanity for the frame: bars at 0.8 - 0.6*(1 - e^(-10/60)) ~ 0.708,
  // caps frozen at 0.8, peak velocities negative (falling after apex).
  const double expected_bar = 0.8 - 0.6 * (1.0 - std::exp(-10.0 / 60.0));
  REQUIRE(std::abs(driver->bar_pos_[0] - expected_bar) < 1e-12);
  REQUIRE(std::abs(driver->peak_pos_[0] - 0.8) < 1e-12);
  REQUIRE(driver->peak_vel_[0] < 0.0);

  // Row colors follow the row-bottom tiers (Go specWrap).
  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);
}

// ---------------------------------------------------------------------------
// ClassicPeak physics — port of cliamp/ui/vis_classic_peak_test.go
// ---------------------------------------------------------------------------

TEST_CASE("classic peak declares the 64-band high-res spec") {
  auto driver = vis_drivers::make_classic_peak_driver();
  // Go TestClassicPeakRequestsHighResBands.
  REQUIRE(driver->analysis_spec() == VisAnalysisSpec{64, 4096});
  REQUIRE(driver->pause_settled());  // Go: classicPeakDriver has no visPauseSettler

  // Cadence (Go classicPeakDriver.TickInterval): playing -> frame interval
  // for 5 rows (fps = 1.7*5*4 = 34 -> 1000/34 = 29 ms); overlay/rest -> slow.
  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == std::chrono::milliseconds(29));
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.overlay_active = false;
  ctx.playing        = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
}

TEST_CASE("classic peak launches caps and settles to rest") {
  // Go TestClassicPeakLaunchAndSettle (withPanelWidth 8 -> 4 columns).
  ClassicPeakDriver driver;
  driver.cols_ = 8;
  const std::size_t cols = 4;  // classicPeakColsForWidth(8)

  const auto bands02 = uniform_bands(10, 0.2f);
  driver.sync(bands02);

  REQUIRE(driver.bar_pos_.size() == cols);
  REQUIRE(driver.peak_pos_.size() == cols);
  for (std::size_t i = 0; i < cols; ++i) {
    REQUIRE(std::abs(driver.bar_pos_[i] - 0.2) < kTestEpsilon);
    REQUIRE(std::abs(driver.peak_pos_[i] - 0.2) < kTestEpsilon);
    REQUIRE(driver.peak_vel_[i] == 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }

  const auto bands08 = uniform_bands(10, 0.8f);
  driver.sync(bands08);
  for (std::size_t i = 0; i < cols; ++i) {
    // Bars never snap on sync; caps launch to the new level.
    REQUIRE(std::abs(driver.bar_pos_[i] - 0.2) < kTestEpsilon);
    REQUIRE(std::abs(driver.peak_pos_[i] - 0.8) < kTestEpsilon);
    REQUIRE(driver.peak_vel_[i] > 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }

  // Advance one default step (lastTick zero -> dt 1/60).
  const auto now = Clock::now();
  driver.advance(now, bands08);
  for (std::size_t i = 0; i < cols; ++i) {
    REQUIRE(driver.bar_pos_[i] > 0.2);
    REQUIRE(driver.bar_pos_[i] < 0.8);
    REQUIRE(driver.peak_pos_[i] > 0.8);
    REQUIRE(driver.peak_pos_[i] > driver.bar_pos_[i]);
    REQUIRE(driver.peak_vel_[i] > 0.0);
  }

  // Target drops to 0.3: bars keep easing down, caps stay airborne.
  const auto bands03 = uniform_bands(10, 0.3f);
  driver.sync(bands03);
  for (std::size_t i = 0; i < cols; ++i) {
    REQUIRE(driver.bar_pos_[i] > 0.3);
    REQUIRE(driver.peak_pos_[i] > driver.bar_pos_[i]);
  }

  for (int step = 1; step <= 128 && driver.animating(bands03); ++step) {
    driver.advance(now + step * kTickClassicPeak, bands03);
  }
  REQUIRE_FALSE(driver.animating(bands03));
  for (std::size_t i = 0; i < cols; ++i) {
    REQUIRE(std::abs(driver.bar_pos_[i] - 0.3) < kClassicPeakVisibleEpsilon);
    REQUIRE(std::abs(driver.peak_pos_[i] - 0.3) < kClassicPeakVisibleEpsilon);
    REQUIRE(driver.peak_vel_[i] == 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }
}

TEST_CASE("classic peak hangs briefly at the apex") {
  // Go TestClassicPeakHangsBrieflyAtApex.
  ClassicPeakDriver driver;
  driver.cols_ = 8;

  const auto bands02 = uniform_bands(10, 0.2f);
  const auto bands08 = uniform_bands(10, 0.8f);
  driver.sync(bands02);
  driver.sync(bands08);  // launch

  const auto now = Clock::now();
  bool found_hold = false;
  for (int step = 1; step <= 32; ++step) {
    driver.advance(now + step * kTickClassicPeak, bands08);
    if (driver.peak_hold_[0] <= 0.0) {
      continue;
    }
    found_hold = true;
    const double held_pos  = driver.peak_pos_[0];
    const double held_vel  = driver.peak_vel_[0];
    const double held_for  = driver.peak_hold_[0];
    driver.advance(now + (step + 1) * kTickClassicPeak, bands08);
    REQUIRE(driver.peak_pos_[0] == held_pos);   // frozen at the apex
    REQUIRE(driver.peak_vel_[0] == held_vel);
    REQUIRE(driver.peak_hold_[0] < held_for);   // hold timer counts down
    break;
  }
  REQUIRE(found_hold);
}

TEST_CASE("classic peak does not relaunch while airborne") {
  // Go TestClassicPeakDoesNotRelaunchWhileAirborne.
  ClassicPeakDriver driver;
  driver.cols_ = 8;

  const auto bands02 = uniform_bands(10, 0.2f);
  const auto bands08 = uniform_bands(10, 0.8f);
  const auto bands095 = uniform_bands(10, 0.95f);
  driver.sync(bands02);
  driver.sync(bands08);
  const auto launch_pos = driver.peak_pos_;
  const auto launch_vel = driver.peak_vel_;

  driver.sync(bands095);  // louder bands while the cap is airborne
  REQUIRE(driver.peak_pos_ == launch_pos);
  REQUIRE(driver.peak_vel_ == launch_vel);
}

TEST_CASE("classic peak resets on enter and on width change") {
  // Go TestClassicPeakResetsOnModeSwitchAndWidthChange.
  ClassicPeakDriver driver;
  driver.cols_ = 6;
  const auto bands04 = uniform_bands(10, 0.4f);
  driver.sync(bands04);  // seed state
  driver.bar_pos_[1]  = 0.7;
  driver.peak_pos_[1] = 0.9;
  driver.peak_vel_[1] = 1.2;

  // Go activateMode(VisBars) + activateMode(VisClassicPeak) -> OnEnter
  // zeroes the whole driver.
  driver.on_enter();
  driver.cols_ = 6;
  driver.sync(bands04);
  REQUIRE(driver.bar_pos_.size() == 3);   // classicPeakColsForWidth(6)
  REQUIRE(driver.peak_pos_.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    REQUIRE(std::abs(driver.bar_pos_[i] - 0.4) < kTestEpsilon);
    REQUIRE(std::abs(driver.peak_pos_[i] - 0.4) < kTestEpsilon);
    REQUIRE(driver.peak_vel_[i] == 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }

  // Width change (Go PanelWidth 6 -> 8) re-resamples and resets.
  driver.cols_ = 8;
  driver.sync(bands04);
  REQUIRE(driver.bar_pos_.size() == 4);
  REQUIRE(driver.peak_pos_.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    REQUIRE(std::abs(driver.bar_pos_[i] - 0.4) < kTestEpsilon);
    REQUIRE(std::abs(driver.peak_pos_[i] - 0.4) < kTestEpsilon);
    REQUIRE(driver.peak_vel_[i] == 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }
}

TEST_CASE("classic peak animating flags caps above bars and settling bars") {
  // Go TestClassicPeakAnimatingWhenCapIsAboveBar +
  // TestClassicPeakAnimatingWhenBarsAreSettling.
  ClassicPeakDriver driver;
  driver.cols_ = 8;
  const auto bands03 = uniform_bands(10, 0.3f);
  const auto bands07 = uniform_bands(10, 0.7f);

  driver.bar_pos_   = {0.3, 0.3, 0.3, 0.3};
  driver.peak_pos_  = {0.5, 0.5, 0.5, 0.5};
  driver.peak_vel_  = {0.0, 0.0, 0.0, 0.0};
  driver.peak_hold_ = {0.0, 0.0, 0.0, 0.0};
  REQUIRE(driver.animating(bands03));  // caps still above the bar

  driver.bar_pos_  = {0.5, 0.5, 0.5, 0.5};
  driver.peak_pos_ = {0.5, 0.5, 0.5, 0.5};
  REQUIRE(driver.animating(bands07));  // bars still easing to target
}

TEST_CASE("classic peak render hides landed caps") {
  // Go TestClassicPeakRenderHidesLandedCaps: fresh driver, caps seeded at
  // the bar height -> no cap glyphs anywhere.
  ClassicPeakDriver driver;
  driver.on_enter();
  CellGrid grid(5, 8);
  grid.fill();
  const auto bands06 = uniform_bands(10, 0.6f);
  driver.render(bands06, 0, grid);
  for (const char32_t glyph : {U'⎺', U'⎻', U'⎼', U'⎽'}) {
    REQUIRE_FALSE(contains_glyph(grid, glyph));
  }
  // But bars render (row 2+ full blocks).
  REQUIRE(contains_glyph(grid, U'█'));
}

TEST_CASE("classic peak render shows attached cap while settling") {
  // Go TestClassicPeakRenderShowsAttachedCapWhileSettling: cap just above
  // the bar renders a glyph.
  ClassicPeakDriver driver;
  driver.on_enter();
  driver.rows_ = 5;
  driver.cols_ = 8;
  driver.bar_pos_   = {0.61, 0.61, 0.61, 0.61};
  driver.peak_pos_  = {0.68, 0.68, 0.68, 0.68};
  driver.peak_vel_  = {0.0, 0.0, 0.0, 0.0};
  driver.peak_hold_ = {0.0, 0.0, 0.0, 0.0};
  CellGrid grid(5, 8);
  grid.fill();
  const auto bands061 = uniform_bands(10, 0.61f);
  driver.render(bands061, 0, grid);
  bool glyph_seen = false;
  for (const char32_t glyph : {U'⎺', U'⎻', U'⎼', U'⎽'}) {
    glyph_seen = glyph_seen || contains_glyph(grid, glyph);
  }
  REQUIRE(glyph_seen);
}

TEST_CASE("classic peak paused ticks decay bars and caps to rest") {
  // Go TestClassicPeakPausedDecaysBarsAndCapsToRest: while paused the Go
  // driver re-analyzes silence (bands -> 0); bootamp's framework keeps
  // passing silent bands, so the direct-driver port ticks with zeros and
  // ctx.playing=false. The clamped dt makes each TickSlow (200 ms) wake
  // advance exactly one 1/60 s frame (Go clamps to 10*tickClassicPeak).
  ClassicPeakDriver driver;
  driver.cols_ = 8;
  const auto bands06 = uniform_bands(10, 0.6f);
  const auto zeros   = uniform_bands(10, 0.0f);
  driver.bar_pos_   = {0.6, 0.6, 0.6, 0.6};
  driver.peak_pos_  = {0.82, 0.82, 0.82, 0.82};
  driver.peak_vel_  = {1.1, 1.1, 1.1, 1.1};
  driver.peak_hold_ = {0.08, 0.08, 0.08, 0.08};
  const auto snapshot_bar = driver.bar_pos_;

  const auto t0 = Clock::time_point(std::chrono::seconds(1));
  driver.last_tick_ = t0;
  VisTickContext ctx;
  ctx.playing = false;  // paused: Go's Tick takes the !Playing branch
  std::uint64_t frame = 0;

  // Two paused ticks must decay the bars, not freeze them at launch height.
  for (int i = 1; i <= 2; ++i) {
    ctx.now = t0 + i * kTickSlow;
    driver.tick(ctx, frame, zeros);
  }
  for (std::size_t i = 0; i < driver.bar_pos_.size(); ++i) {
    REQUIRE(driver.bar_pos_[i] < snapshot_bar[i]);
  }

  // Keep ticking until the airborne caps have fallen and the driver settles
  // (Go caps the settle window at 240 ticks).
  int steps = 2;
  for (; steps < 240 && driver.animating(zeros); ++steps) {
    ctx.now = t0 + (steps + 1) * kTickSlow;
    driver.tick(ctx, frame, zeros);
  }
  REQUIRE_FALSE(driver.animating(zeros));
  for (std::size_t i = 0; i < driver.bar_pos_.size(); ++i) {
    // Go pausedDecayEpsilon == 0.01 == kClassicPeakVisibleEpsilon.
    REQUIRE(driver.bar_pos_[i] < kClassicPeakVisibleEpsilon);
    REQUIRE(driver.peak_pos_[i] < kClassicPeakVisibleEpsilon);
    REQUIRE(driver.peak_vel_[i] == 0.0);
    REQUIRE(driver.peak_hold_[i] == 0.0);
  }
  REQUIRE(driver.tick_interval(ctx) == kTickSlow);
}

TEST_CASE("classic peak overlay freezes state and clears the animation clock") {
  // Go TestClassicPeakOverlayFreezesStateAndClearsAnimationClock.
  ClassicPeakDriver driver;
  driver.cols_ = 8;
  const auto bands06 = uniform_bands(10, 0.6f);
  driver.bar_pos_   = {0.6, 0.6, 0.6, 0.6};
  driver.peak_pos_  = {0.82, 0.82, 0.82, 0.82};
  driver.peak_vel_  = {1.1, 1.1, 1.1, 1.1};
  driver.peak_hold_ = {0.08, 0.08, 0.08, 0.08};
  const auto snapshot_peak = driver.peak_pos_;
  const auto snapshot_vel  = driver.peak_vel_;
  const auto snapshot_hold = driver.peak_hold_;

  driver.last_tick_ = Clock::now();
  VisTickContext ctx;
  ctx.now            = Clock::now();
  ctx.overlay_active = true;
  std::uint64_t frame = 0;
  driver.tick(ctx, frame, bands06);

  REQUIRE(driver.last_tick_ == Clock::time_point{});
  REQUIRE(driver.bands_at_ == Clock::time_point{});
  REQUIRE(driver.peak_pos_ == snapshot_peak);   // frozen
  REQUIRE(driver.peak_vel_ == snapshot_vel);
  REQUIRE(driver.peak_hold_ == snapshot_hold);
  REQUIRE(driver.animating(bands06));  // airborne caps still animate-worthy
  ctx.playing = true;
  REQUIRE(driver.tick_interval(ctx) == kTickSlow);
}

TEST_CASE("classic peak renders full-width on even panel widths") {
  // Go TestClassicPeakRenderFillsEvenWidthPanels: 8-col panel -> 4 bars +
  // 3 gaps + 1 rowPad == every column written.
  ClassicPeakDriver driver;
  driver.on_enter();
  CellGrid grid(5, 8);
  grid.fill(Cell{U'x', kColorDefault});
  const auto bands06 = uniform_bands(10, 0.6f);
  driver.render(bands06, 0, grid);
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      REQUIRE(grid.at(r, c).rune != U'x');  // no fill left behind
    }
  }
}

TEST_CASE("classic peak even widths keep bars separated") {
  // Go TestClassicPeakEvenWidthKeepsBarsSeparated: 1-wide bars with 1-wide
  // gaps must never merge into runs of three (no "███").
  ClassicPeakDriver driver;
  driver.on_enter();
  CellGrid grid(5, 8);
  grid.fill();
  const auto bands06 = uniform_bands(10, 0.6f);
  driver.render(bands06, 0, grid);

  bool found_bar = false;
  for (int r = 0; r < grid.rows(); ++r) {
    int run = 0;
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).rune == U' ') {
        run = 0;
        continue;
      }
      found_bar = true;
      ++run;
      REQUIRE(run < 3);  // adjacent bars must stay separated
    }
  }
  REQUIRE(found_bar);
}

TEST_CASE("classic peak retains detail at the right edge") {
  // Go TestClassicPeakRetainsDetailAtRightEdge: a descending tail at the top
  // of the 64-band spectrum must survive the linear resample (default panel
  // width 74 -> 37 columns).
  ClassicPeakDriver driver;  // cols_ stays 0 -> levels() uses panel_width()
  std::vector<float> bands(64, 0.0f);
  const float tail[] = {0.55f, 0.46f, 0.37f, 0.28f, 0.19f, 0.1f};
  for (int i = 0; i < 6; ++i) {
    bands[static_cast<std::size_t>(58 + i)] = tail[i];
  }

  const std::vector<double> levels = driver.levels(bands);
  REQUIRE(levels.size() >= 4);
  bool found_step = false;
  for (std::size_t i = levels.size() - 4; i + 1 < levels.size(); ++i) {
    if (std::abs(levels[i] - levels[i + 1]) > 0.01) {
      found_step = true;
      break;
    }
  }
  REQUIRE(found_step);
}
