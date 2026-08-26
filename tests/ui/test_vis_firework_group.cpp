// tests/ui/test_vis_firework_group.cpp — golden tests for the firework driver
// group: firework, bubbles, butterfly, scatter (stateless renderers) and
// geyser (stateful particle fountain). Port of the cliamp Go drivers
// vis_firework.go / vis_bubbles.go / vis_butterfly.go / vis_scatter.go /
// vis_geyser.go (which have no dedicated *_test.go; the assertions here follow
// the visualizer_driver_test.go conventions: spec, cadence, determinism, and
// energy/frame sensitivity, plus exact golden dumps where the Go math is
// hand-computable — silence draws only the blank braille glyph).
//
// The tests drive the drivers directly (like the Logo tests in
// test_visualizer.cpp): the grid is pre-filled with spaces first, matching the
// framework's render() contract (Visualizer::render clears the grid, then the
// driver writes into it).
#include "ui/cell.hpp"
#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_driver.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace bootamp::ui;
using bootamp::dsp::kDefaultSpectrumBands;

namespace {

// Count character cells that carry at least one Braille dot (Go
// visualizer_driver_test.go's "any braille dot set" probe).
std::size_t lit_dot_count(const CellGrid& g) {
  std::size_t n = 0;
  for (int r = 0; r < g.rows(); ++r) {
    for (int c = 0; c < g.cols(); ++c) {
      if (g.at(r, c).rune != U'⠀') {
        ++n;
      }
    }
  }
  return n;
}

// n copies of the blank Braille glyph U+2800 as UTF-8 (as dump_grid emits).
std::string braille_blanks(int n) {
  std::string out;
  out.reserve(static_cast<std::size_t>(n) * 3);
  for (int i = 0; i < n; ++i) {
    out += "\xE2\xA0\x80";
  }
  return out;
}

std::string spaces(int n) { return std::string(static_cast<std::size_t>(n), ' '); }

// A grid pre-filled like the framework does before driver->render().
CellGrid make_grid(int rows, int cols) {
  CellGrid g(rows, cols);
  g.fill(Cell{});
  return g;
}

VisTickContext playing_ctx() {
  VisTickContext ctx;
  ctx.playing = true;
  ctx.now     = std::chrono::steady_clock::now();
  return ctx;
}

std::vector<float> bands_fill(float v) {
  return std::vector<float>(kDefaultSpectrumBands, v);
}

}  // namespace

// ---------------------------------------------------------------------------
// Spec + cadence (shared by all five drivers)
// ---------------------------------------------------------------------------

TEST_CASE("the firework group drivers declare the default 10-band spec and cadence") {
  const auto check = [](std::unique_ptr<VisDriver> d, const char* name) {
    INFO(name);
    REQUIRE(d != nullptr);
    const auto spec = d->analysis_spec();
    REQUIRE(spec.band_count == 10);  // DefaultSpectrumBands
    REQUIRE(spec.fft_size == 2048);  // defaultFFTSize
    VisTickContext ctx;
    ctx.playing = true;
    REQUIRE(d->tick_interval(ctx) == kTickFast);  // defaultDriverTickInterval
    ctx.playing = false;
    REQUIRE(d->tick_interval(ctx) == kTickSlow);
    ctx.playing = true;
    ctx.overlay_active = true;
    REQUIRE(d->tick_interval(ctx) == kTickSlow);
  };
  check(vis_drivers::make_firework_driver(), "firework");
  check(vis_drivers::make_bubbles_driver(), "bubbles");
  check(vis_drivers::make_butterfly_driver(), "butterfly");
  check(vis_drivers::make_scatter_driver(), "scatter");
  check(vis_drivers::make_geyser_driver(), "geyser");

  // Band-driven renderers: the framework decides settling from band levels.
  REQUIRE(vis_drivers::make_firework_driver()->pause_settled());
  REQUIRE(vis_drivers::make_bubbles_driver()->pause_settled());
  REQUIRE(vis_drivers::make_butterfly_driver()->pause_settled());
  REQUIRE(vis_drivers::make_scatter_driver()->pause_settled());
  REQUIRE(vis_drivers::make_geyser_driver()->pause_settled());  // no particles yet
}

// ---------------------------------------------------------------------------
// Firework (cliamp vis_firework.go)
// ---------------------------------------------------------------------------

TEST_CASE("firework renders deterministic trails and bursts, denser when loud") {
  auto driver = vis_drivers::make_firework_driver();

  CellGrid quiet = make_grid(5, 40);
  CellGrid loud  = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, quiet);
  driver->render(bands_fill(1.0f), 0, loud);

  // 5 bursts always run; burst 0 at frame 0 is a rising trail, so there is
  // always at least one lit dot even in silence.
  REQUIRE(lit_dot_count(quiet) > 0);
  REQUIRE(lit_dot_count(loud) > lit_dot_count(quiet));

  // Deterministic for a given frame + bands.
  CellGrid q2 = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, q2);
  REQUIRE(dump_grid(quiet) == dump_grid(q2));

  // Next cycle re-seeds burst positions (frame 0 trail at dot col 0, frame 48
  // trail at dot col 79 for a 40-col panel).
  CellGrid f48 = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 48, f48);
  REQUIRE(dump_grid(loud) != dump_grid(f48));

  // Per-row spectrum colors (Go specWrap: top rows bright, bottom dim).
  REQUIRE(quiet.at(0, 0).color == kColorSpecHigh);
  REQUIRE(quiet.at(1, 0).color == kColorSpecHigh);
  REQUIRE(quiet.at(2, 0).color == kColorSpecMid);
  REQUIRE(quiet.at(3, 0).color == kColorSpecLow);
  REQUIRE(quiet.at(4, 0).color == kColorSpecLow);

  // Every cell carries a Braille glyph in the 0x2800..0x28FF range.
  for (int r = 0; r < quiet.rows(); ++r) {
    for (int c = 0; c < quiet.cols(); ++c) {
      const char32_t rune = quiet.at(r, c).rune;
      REQUIRE(rune >= U'⠀');
      REQUIRE(rune <= U'⣿');
    }
  }
}

// ---------------------------------------------------------------------------
// Bubbles (cliamp vis_bubbles.go)
// ---------------------------------------------------------------------------

TEST_CASE("bubbles renders deterministic hollow rings that rise and sway") {
  auto driver = vis_drivers::make_bubbles_driver();

  CellGrid quiet = make_grid(5, 40);
  CellGrid loud  = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, quiet);
  driver->render(bands_fill(1.0f), 0, loud);

  // Fixed 18-bubble count; most are on screen at frame 0 regardless of audio.
  REQUIRE(lit_dot_count(quiet) > 0);
  REQUIRE(lit_dot_count(loud) > 0);
  // Loud passages sway harder (avgEnergy drives swayAmp), shifting the rings.
  REQUIRE(dump_grid(quiet) != dump_grid(loud));

  // Deterministic for a given frame + bands.
  CellGrid q2 = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, q2);
  REQUIRE(dump_grid(quiet) == dump_grid(q2));

  // Upward scroll (y depends on frame/speedDiv) + sway move every ring.
  CellGrid q24 = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 24, q24);
  REQUIRE(dump_grid(quiet) != dump_grid(q24));

  // Per-row spectrum colors (Go specWrap: top warm, bottom cool).
  REQUIRE(quiet.at(0, 0).color == kColorSpecHigh);
  REQUIRE(quiet.at(4, 0).color == kColorSpecLow);
}

// ---------------------------------------------------------------------------
// Butterfly (cliamp vis_butterfly.go)
// ---------------------------------------------------------------------------

TEST_CASE("butterfly renders a blank golden grid for silence and a spine for loud") {
  auto driver = vis_drivers::make_butterfly_driver();

  // Golden: silence draws nothing — every cell is the blank Braille glyph and
  // butterfly emits no inter-band separators.
  CellGrid quiet = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, quiet);
  const std::string line = braille_blanks(40);
  REQUIRE(dump_grid(quiet) == line + "\n" + line + "\n" + line + "\n" + line + "\n" + line);
  REQUIRE(lit_dot_count(quiet) == 0);

  // Butterfly's row gradient is inverted vs the other renderers (Go colors by
  // row/(rows-1)): top dim, bottom bright.
  REQUIRE(quiet.at(0, 0).color == kColorSpecLow);
  REQUIRE(quiet.at(4, 0).color == kColorSpecHigh);

  // Loud: the central spine (energy > 0.05) plus wing dots appear.
  CellGrid loud = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 0, loud);
  REQUIRE(lit_dot_count(loud) > 0);
  REQUIRE(lit_dot_count(loud) > lit_dot_count(quiet));

  // Determinism + wobble/flicker phase sensitivity.
  CellGrid l2 = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 0, l2);
  REQUIRE(dump_grid(loud) == dump_grid(l2));
  CellGrid l48 = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 48, l48);
  REQUIRE(dump_grid(loud) != dump_grid(l48));
}

// ---------------------------------------------------------------------------
// Scatter (cliamp vis_scatter.go)
// ---------------------------------------------------------------------------

TEST_CASE("scatter renders a golden blank layout for silence with band gaps") {
  auto driver = vis_drivers::make_scatter_driver();

  // Golden layout: 10 bands across 40 cols -> visBandWidth widths
  // [4,3,3,3,3,3,3,3,3,3] with 9 inter-band spaces; silence draws no dots.
  CellGrid quiet = make_grid(5, 40);
  driver->render(bands_fill(0.0f), 0, quiet);
  std::string line;
  for (int b = 0; b < 10; ++b) {
    line += braille_blanks(b == 0 ? 4 : 3);
    if (b < 9) {
      line += ' ';
    }
  }
  REQUIRE(line.size() == 40);
  REQUIRE(dump_grid(quiet) == line + "\n" + line + "\n" + line + "\n" + line + "\n" + line);
  REQUIRE(lit_dot_count(quiet) == 0);
  REQUIRE(quiet.at(0, 0).color == kColorSpecHigh);
  REQUIRE(quiet.at(4, 0).color == kColorSpecLow);

  // Loud: density is the squared band level times a gravity bias.
  CellGrid loud = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 0, loud);
  REQUIRE(lit_dot_count(loud) > 0);
  REQUIRE(lit_dot_count(loud) > lit_dot_count(quiet));

  // Determinism + twinkle: the scatterHash gate moves with the frame.
  CellGrid l2 = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 0, l2);
  REQUIRE(dump_grid(loud) == dump_grid(l2));
  CellGrid l100 = make_grid(5, 40);
  driver->render(bands_fill(1.0f), 100, l100);
  REQUIRE(dump_grid(loud) != dump_grid(l100));

  // Narrow panel: only the leading visible bands get columns; the trailing
  // inter-band gaps are clipped by the fit (grid.set drops out-of-range
  // writes): 5 cols -> "blank, space, blank, space, blank".
  CellGrid narrow = make_grid(5, 5);
  driver->render(bands_fill(0.0f), 0, narrow);
  const std::string narrow_line = braille_blanks(1) + " " + braille_blanks(1) + " " +
                                  braille_blanks(1);
  REQUIRE(dump_grid(narrow) == narrow_line + "\n" + narrow_line + "\n" + narrow_line + "\n" +
                                   narrow_line + "\n" + narrow_line);
}

// ---------------------------------------------------------------------------
// Geyser (cliamp vis_geyser.go) — stateful particle fountain
// ---------------------------------------------------------------------------

TEST_CASE("geyser spawns from bass, freezes under overlay, drains to rest") {
  auto driver = vis_drivers::make_geyser_driver();
  REQUIRE(driver->pause_settled());

  CellGrid grid = make_grid(5, 40);
  std::uint64_t frame = 0;

  // First render caches the render target; no particles yet -> untouched
  // (framework pre-fill: spaces).
  driver->render(bands_fill(1.0f), frame, grid);
  REQUIRE(lit_dot_count(grid) == 0);
  REQUIRE(dump_grid(grid) == spaces(40) + "\n" + spaces(40) + "\n" + spaces(40) + "\n" +
                                 spaces(40) + "\n" + spaces(40));

  // Loud bass tick: steady drizzle + transient kick spawn particles. Uniform
  // 1.0 bands mean bass == 1.0, so every particle inherits tier 3 (red/high).
  driver->tick(playing_ctx(), frame, bands_fill(1.0f));
  REQUIRE_FALSE(driver->pause_settled());
  grid.fill(Cell{});
  driver->render(bands_fill(1.0f), frame, grid);
  REQUIRE(lit_dot_count(grid) > 0);
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).rune != U'⠀') {
        REQUIRE(grid.at(r, c).color == kColorSpecHigh);
      }
    }
  }
  const std::string after_loud = dump_grid(grid);

  // Overlay tick: cliamp returns early — the fountain is frozen (no spawns,
  // no physics), so the frame is unchanged.
  VisTickContext overlay = playing_ctx();
  overlay.overlay_active = true;
  driver->tick(overlay, frame, bands_fill(1.0f));
  grid.fill(Cell{});
  driver->render(bands_fill(1.0f), frame, grid);
  REQUIRE(dump_grid(grid) == after_loud);

  // A normal tick advances the physics even with silence (no new spawns —
  // steady == 0 and the bass delta is negative — but existing particles move).
  driver->tick(playing_ctx(), frame, bands_fill(0.0f));
  grid.fill(Cell{});
  driver->render(bands_fill(1.0f), frame, grid);
  REQUIRE(dump_grid(grid) != after_loud);

  // Silence drains the fountain: gravity pulls every particle out of the
  // panel (or past the 200-tick life cap), then the driver reports settled.
  bool settled = false;
  for (int i = 0; i < 400 && !settled; ++i) {
    driver->tick(playing_ctx(), frame, bands_fill(0.0f));
    settled = driver->pause_settled();
  }
  REQUIRE(settled);
  grid.fill(Cell{});
  driver->render(bands_fill(0.0f), frame, grid);
  REQUIRE(lit_dot_count(grid) == 0);

  // Determinism: two drivers driven through the same sequence render alike.
  auto a = vis_drivers::make_geyser_driver();
  auto b = vis_drivers::make_geyser_driver();
  CellGrid ga = make_grid(5, 40), gb = make_grid(5, 40);
  std::uint64_t f2 = 0;
  a->render(bands_fill(0.5f), f2, ga);
  b->render(bands_fill(0.5f), f2, gb);
  for (int i = 0; i < 8; ++i) {
    a->tick(playing_ctx(), f2, bands_fill(0.5f));
    b->tick(playing_ctx(), f2, bands_fill(0.5f));
  }
  ga.fill(Cell{});
  gb.fill(Cell{});
  a->render(bands_fill(0.5f), f2, ga);
  b->render(bands_fill(0.5f), f2, gb);
  REQUIRE(dump_grid(ga) == dump_grid(gb));

  // on_enter resets grid, particles, and the bass integrator — but the
  // render-size cache survives, so the fountain respawns on the next tick.
  driver->on_enter();
  REQUIRE(driver->pause_settled());
  driver->tick(playing_ctx(), frame, bands_fill(1.0f));
  REQUIRE_FALSE(driver->pause_settled());
  grid.fill(Cell{});
  driver->render(bands_fill(1.0f), frame, grid);
  REQUIRE(lit_dot_count(grid) > 0);
}
