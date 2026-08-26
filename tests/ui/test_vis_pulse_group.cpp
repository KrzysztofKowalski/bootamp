// tests/ui/test_vis_pulse_group.cpp — vis driver group tests: pulse,
// heartbeat, ascii, binary, braillegrid.
//
// Ports the cliamp driver-level behaviors (analysis specs, tick cadence,
// pause settling) onto the C++ VisDriver contract and adds golden-file checks:
// if golden/vis/<mode>.txt is present, dump_grid of a fixed deterministic
// frame must match it exactly; if absent, the golden comparison is skipped and
// analytic invariants are verified instead (golden README rule — never fail on
// a missing golden; goldens are host-generated).
//
// Golden generation conventions (fixed frame, fixed input — see each TEST_CASE):
//   golden/vis/pulse.txt        — dump_grid, 5x40 grid, frame 0, 10 bands x 0.5
//   golden/vis/heartbeat.txt    — dump_grid, 5x40 grid, frame 0, waveform =
//                                 2048-sample 440 Hz sine (amplitude 1.0)
//   golden/vis/ascii.txt        — dump_grid, 5x40 grid, frame 0, 10 bands x 0.5
//   golden/vis/binary.txt       — dump_grid, 5x40 grid, frame 42, 10 bands x 0.5
//   golden/vis/braillegrid.txt  — dump_grid, 5x40 grid, dots set by
//                                 tier = 1 + ((x*3 + y*5) % 3) for all dots
//                                 (x in [0,80), y in [0,20))
#include "ui/cell.hpp"
#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_drivers/braillegrid.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bootamp::ui;
using namespace bootamp::ui::vis_drivers;
using bootamp::dsp::kDefaultSpectrumBands;

namespace {

// load_golden reads golden/vis/<mode>.txt, searching a few prefix depths so
// the tests run from a build dir at any nesting level. Returns nullopt when
// the golden is absent (the harness then checks invariants instead).
std::optional<std::string> load_golden(std::string_view mode) {
  constexpr std::array<const char*, 6> kPrefixes = {
      "golden/vis/",          "../golden/vis/",       "../../golden/vis/",
      "../../../golden/vis/", "../../../../golden/vis/", "../../../../../golden/vis/",
  };
  for (const char* prefix : kPrefixes) {
    std::ifstream in(std::string(prefix) + std::string(mode) + ".txt");
    if (!in) {
      continue;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    // dump_grid emits no trailing newline; tolerate editors that add one.
    if (!content.empty() && content.back() == '\n') {
      content.pop_back();
    }
    return content;
  }
  return std::nullopt;
}

// check_golden compares dump_grid against the golden when one exists.
void check_golden(std::string_view mode, const CellGrid& grid) {
  if (const std::optional<std::string> golden = load_golden(mode)) {
    INFO("golden/vis/" << mode << ".txt");
    REQUIRE(dump_grid(grid) == *golden);
  }
}

// All cells must be braille codepoints (Go renders braille runes everywhere).
bool all_braille(const CellGrid& g) {
  for (int r = 0; r < g.rows(); ++r) {
    for (int c = 0; c < g.cols(); ++c) {
      const char32_t cp = g.at(r, c).rune;
      if (cp < 0x2800 || cp > 0x28FF) {
        return false;
      }
    }
  }
  return true;
}

// Number of cells carrying at least one set braille dot.
std::size_t lit_cell_count(const CellGrid& g) {
  std::size_t n = 0;
  for (int r = 0; r < g.rows(); ++r) {
    for (int c = 0; c < g.cols(); ++c) {
      if (g.at(r, c).rune != 0x2800) {
        ++n;
      }
    }
  }
  return n;
}

// A deterministic 2048-sample 440 Hz sine waveform (amplitude 1.0).
std::vector<float> sine_wave(std::size_t n) {
  std::vector<float> w(n);
  for (std::size_t i = 0; i < n; ++i) {
    w[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 *
                                       static_cast<double>(i) / 44100.0));
  }
  return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------

TEST_CASE("pulse driver: 10-band spec and default cadence") {
  auto driver = make_pulse_driver();
  REQUIRE(driver != nullptr);
  REQUIRE(driver->analysis_spec().band_count == 10);  // cliamp DefaultSpectrumBands
  REQUIRE(driver->analysis_spec().fft_size == 2048);

  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == kTickFast);
  ctx.playing = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  REQUIRE(driver->pause_settled());  // band-driven: framework decides settling
}

TEST_CASE("pulse driver: renders braille, deterministic, louder fills more") {
  auto driver = make_pulse_driver();
  std::vector<float> loud(kDefaultSpectrumBands, 0.9f);
  std::vector<float> quiet(kDefaultSpectrumBands, 0.0f);

  CellGrid a(5, 40), b(5, 40), c(5, 40);
  driver->render(loud, 0, a);
  driver->render(loud, 0, b);  // same frame -> identical
  driver->render(quiet, 0, c);

  REQUIRE(dump_grid(a) == dump_grid(b));
  REQUIRE(all_braille(a));
  REQUIRE(all_braille(c));
  // Loud audio fills the ellipse; silence collapses it to the breathing core.
  REQUIRE(lit_cell_count(a) > lit_cell_count(c));
  // Colors come from the palette slots (green->yellow->red gradient).
  for (int r = 0; r < a.rows(); ++r) {
    for (int cc = 0; cc < a.cols(); ++cc) {
      const Color col = a.at(r, cc).color;
      const bool valid = (col == kColorSpecLow) || (col == kColorSpecMid) ||
                         (col == kColorSpecHigh);
      REQUIRE(valid);
    }
  }
}

TEST_CASE("pulse driver: golden output when present") {
  auto driver = make_pulse_driver();
  std::vector<float> bands(kDefaultSpectrumBands, 0.5f);
  CellGrid grid(5, 40);
  driver->render(bands, 0, grid);
  check_golden("pulse", grid);
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

TEST_CASE("heartbeat driver: raw-sample spec, TickWave cadence, pause settling") {
  auto driver = make_heartbeat_driver();
  REQUIRE(driver != nullptr);
  REQUIRE(driver->analysis_spec().band_count == 0);  // raw-sample mode
  REQUIRE(driver->analysis_spec().fft_size == 2048);

  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == kTickWave);  // cliamp TickWave cadence
  ctx.playing = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);

  REQUIRE(driver->pause_settled());  // no waveform held yet
}

TEST_CASE("heartbeat driver: traces the waveform and clears on pause") {
  auto driver = make_heartbeat_driver();
  const auto wave = sine_wave(2048);
  VisTickContext ctx;
  ctx.playing = true;
  ctx.waveform_samples_into = [&wave](std::span<float> dst) {
    const std::size_t n = std::min(dst.size(), wave.size());
    std::copy_n(wave.begin(), n, dst.begin());
    return n;
  };
  std::uint64_t frame = 0;

  driver->tick(ctx, frame, {});
  REQUIRE_FALSE(driver->pause_settled());  // waveform held

  CellGrid with_wave(5, 40);
  driver->render({}, 0, with_wave);
  REQUIRE(all_braille(with_wave));
  // A loud sine (shaped = sample^2 >= 0) lifts the trace above the baseline:
  // some cell must be red (trace tier), not all green.
  bool saw_trace = false;
  for (int r = 0; r < with_wave.rows(); ++r) {
    for (int c = 0; c < with_wave.cols(); ++c) {
      if (with_wave.at(r, c).color == kColorSpecHigh) {
        saw_trace = true;
      }
    }
  }
  REQUIRE(saw_trace);

  // Deterministic for a fixed waveform.
  CellGrid again(5, 40);
  driver->render({}, 0, again);
  REQUIRE(dump_grid(with_wave) == dump_grid(again));

  // Pause: the driver clears its waveform (cliamp clears the waveBuf on the
  // first paused tick) and reports settled.
  VisTickContext paused;
  paused.paused = true;
  driver->tick(paused, frame, {});
  REQUIRE(driver->pause_settled());

  // The cleared trace renders the flat baseline: exactly `width` lit cells,
  // all in the middle row, all baseline-green.
  CellGrid flat(5, 40);
  driver->render({}, 0, flat);
  REQUIRE(lit_cell_count(flat) == 40);
  for (int c = 0; c < flat.cols(); ++c) {
    REQUIRE(flat.at(2, c).rune != 0x2800);
    REQUIRE(flat.at(2, c).color == kColorSpecLow);
    for (int r = 0; r < flat.rows(); ++r) {
      if (r != 2) {
        REQUIRE(flat.at(r, c).rune == 0x2800);
      }
    }
  }
}

TEST_CASE("heartbeat driver: golden output when present") {
  auto driver = make_heartbeat_driver();
  const auto wave = sine_wave(2048);
  VisTickContext ctx;
  ctx.playing = true;
  ctx.waveform_samples_into = [&wave](std::span<float> dst) {
    const std::size_t n = std::min(dst.size(), wave.size());
    std::copy_n(wave.begin(), n, dst.begin());
    return n;
  };
  std::uint64_t frame = 0;
  driver->tick(ctx, frame, {});
  CellGrid grid(5, 40);
  driver->render({}, 0, grid);
  check_golden("heartbeat", grid);
}

// ---------------------------------------------------------------------------
// Ascii
// ---------------------------------------------------------------------------

TEST_CASE("ascii driver: 10-band spec and TickAnim cadence") {
  auto driver = make_ascii_driver();
  REQUIRE(driver != nullptr);
  REQUIRE(driver->analysis_spec().band_count == 10);
  REQUIRE(driver->analysis_spec().fft_size == 2048);

  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == kTickAnim);  // cliamp TickAnim cadence
  ctx.playing = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
}

TEST_CASE("ascii driver: shade blocks follow the row spans") {
  auto driver = make_ascii_driver();

  // Full-scale bands: every row is a solid block line (Go shadeBlock >= rowTop).
  CellGrid full(5, 40);
  driver->render(std::vector<float>(kDefaultSpectrumBands, 1.0f), 0, full);
  for (int r = 0; r < full.rows(); ++r) {
    for (int c = 0; c < full.cols(); ++c) {
      if (c % 2 == 0) {
        REQUIRE(full.at(r, c).rune == U'█');
      } else {
        REQUIRE(full.at(r, c).rune == U' ');  // 1-wide/1-gap layout
      }
    }
  }
  // Row-bottom tiers: top two rows high, middle mid, bottom two low
  // (Go specWrap: 0.8/0.6 >= 0.6 high; 0.4 mid; 0.2/0.0 low).
  REQUIRE(full.at(0, 0).color == kColorSpecHigh);
  REQUIRE(full.at(1, 0).color == kColorSpecHigh);
  REQUIRE(full.at(2, 0).color == kColorSpecMid);
  REQUIRE(full.at(3, 0).color == kColorSpecLow);
  REQUIRE(full.at(4, 0).color == kColorSpecLow);

  // Silence: all spaces.
  CellGrid silent(5, 40);
  driver->render(std::vector<float>(kDefaultSpectrumBands, 0.0f), 0, silent);
  for (int r = 0; r < silent.rows(); ++r) {
    for (int c = 0; c < silent.cols(); ++c) {
      REQUIRE(silent.at(r, c).rune == U' ');
    }
  }

  // Uniform 0.5: row spans give ' ', ' ', '▒', '█', '█' (Go shadeBlock
  // frac 0.5 -> '▒' at row 2; rows 3-4 are fully covered).
  CellGrid mid(5, 40);
  driver->render(std::vector<float>(kDefaultSpectrumBands, 0.5f), 0, mid);
  const char32_t expect[5] = {U' ', U' ', U'▒', U'█', U'█'};
  for (int r = 0; r < mid.rows(); ++r) {
    for (int c = 0; c < mid.cols(); c += 2) {
      REQUIRE(mid.at(r, c).rune == expect[r]);
    }
  }

  // Deterministic for a given frame.
  CellGrid again(5, 40);
  driver->render(std::vector<float>(kDefaultSpectrumBands, 0.5f), 0, again);
  REQUIRE(dump_grid(mid) == dump_grid(again));
}

TEST_CASE("ascii driver: golden output when present") {
  auto driver = make_ascii_driver();
  std::vector<float> bands(kDefaultSpectrumBands, 0.5f);
  CellGrid grid(5, 40);
  driver->render(bands, 0, grid);
  check_golden("ascii", grid);
}

// ---------------------------------------------------------------------------
// Binary
// ---------------------------------------------------------------------------

TEST_CASE("binary driver: 10-band spec and default cadence") {
  auto driver = make_binary_driver();
  REQUIRE(driver != nullptr);
  REQUIRE(driver->analysis_spec().band_count == 10);
  REQUIRE(driver->analysis_spec().fft_size == 2048);

  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == kTickFast);
  ctx.playing = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
}

TEST_CASE("binary driver: bit stream output, determinism, energy bias") {
  auto driver = make_binary_driver();

  CellGrid a(5, 40), b(5, 40), loud(5, 40), quiet(5, 40);
  std::vector<float> bands(kDefaultSpectrumBands, 0.5f);
  driver->render(bands, 42, a);
  driver->render(bands, 42, b);  // same frame -> identical stream
  REQUIRE(dump_grid(a) == dump_grid(b));

  // Only '0', '1', and gap ' ' ever appear.
  for (int r = 0; r < a.rows(); ++r) {
    for (int c = 0; c < a.cols(); ++c) {
      const char32_t cp = a.at(r, c).rune;
      REQUIRE((cp == U'0' || cp == U'1' || cp == U' '));
    }
  }

  // Higher energy produces more 1s (active data).
  driver->render(std::vector<float>(kDefaultSpectrumBands, 0.9f), 42, loud);
  driver->render(std::vector<float>(kDefaultSpectrumBands, 0.1f), 42, quiet);
  const auto count_ones = [](const CellGrid& g) {
    std::size_t n = 0;
    for (int r = 0; r < g.rows(); ++r) {
      for (int c = 0; c < g.cols(); ++c) {
        if (g.at(r, c).rune == U'1') {
          ++n;
        }
      }
    }
    return n;
  };
  REQUIRE(count_ones(loud) > count_ones(quiet));

  // Bright colors on 1s of high-energy bands (tag 2/1), dim otherwise.
  bool saw_bright = false, saw_dim = false;
  for (int r = 0; r < loud.rows(); ++r) {
    for (int c = 0; c < loud.cols(); ++c) {
      if (loud.at(r, c).color == kColorSpecHigh) {
        saw_bright = true;
      }
      if (loud.at(r, c).color == kColorSpecLow) {
        saw_dim = true;
      }
    }
  }
  REQUIRE(saw_bright);
  REQUIRE(saw_dim);
}

TEST_CASE("binary driver: golden output when present") {
  auto driver = make_binary_driver();
  std::vector<float> bands(kDefaultSpectrumBands, 0.5f);
  CellGrid grid(5, 40);
  driver->render(bands, 42, grid);
  check_golden("binary", grid);
}

// ---------------------------------------------------------------------------
// BrailleGrid rasteriser (cliamp/ui/vis_braillegrid.go brailleGrid + rng64)
// ---------------------------------------------------------------------------

TEST_CASE("braillegrid: ensure, clear, and max-update set semantics") {
  BrailleGrid g;
  g.ensure(20, 80);
  REQUIRE(g.dot_rows() == 20);
  REQUIRE(g.dot_cols() == 80);

  // set is a max-update (Go: if tier > cells[...]).
  g.set(3, 5, 1);
  g.set(3, 5, 3);
  g.set(3, 5, 2);
  REQUIRE(g.tier_at(3, 5) == 3);

  // Out-of-bounds writes are dropped.
  g.set(-1, 0, 1);
  g.set(0, -1, 1);
  g.set(80, 0, 1);
  g.set(0, 20, 1);
  REQUIRE(g.tier_at(0, 0) == 0);

  // clear zeroes everything but keeps the allocation.
  g.clear();
  REQUIRE(g.tier_at(3, 5) == 0);

  // ensure with the same dims clears; with new dims reallocates.
  g.ensure(20, 80);
  REQUIRE(g.dot_rows() == 20);
  g.set(0, 0, 2);
  g.ensure(10, 40);
  REQUIRE(g.dot_rows() == 10);
  REQUIRE(g.dot_cols() == 40);
  REQUIRE(g.tier_at(0, 0) == 0);  // fresh allocation
}

TEST_CASE("braillegrid: renders dots as braille glyphs with tier colors") {
  BrailleGrid g;
  g.ensure(20, 80);
  // Cell (0,0): dots dr0dc0, dr0dc1, dr1dc0, dr1dc1, dr2dc0, dr3dc1
  //             -> 0x01|0x08|0x02|0x10|0x04|0x80 = 0x9F, max tier 3 -> high.
  g.set(0, 0, 1);
  g.set(1, 0, 1);
  g.set(0, 1, 2);
  g.set(1, 1, 1);
  g.set(0, 2, 3);
  g.set(1, 3, 1);
  // Cell (0,1): dots dr0dc0, dr0dc1, dr3dc0
  //             -> 0x01|0x08|0x40 = 0x49, max tier 3 -> high.
  g.set(2, 0, 1);
  g.set(3, 0, 2);
  g.set(2, 3, 3);

  CellGrid out(5, 40);
  g.render(5, 40, out);
  REQUIRE(out.at(0, 0).rune == (0x2800 | 0x9F));
  REQUIRE(out.at(0, 0).color == kColorSpecHigh);
  REQUIRE(out.at(0, 1).rune == (0x2800 | 0x49));
  REQUIRE(out.at(0, 1).color == kColorSpecHigh);
  // Every other cell: empty glyph, low-tier color (Go cellTag defaults to 0).
  for (int r = 0; r < out.rows(); ++r) {
    for (int c = 0; c < out.cols(); ++c) {
      if (r == 0 && (c == 0 || c == 1)) {
        continue;
      }
      REQUIRE(out.at(r, c).rune == 0x2800);
      REQUIRE(out.at(r, c).color == kColorSpecLow);
    }
  }
}

TEST_CASE("braillegrid: undersized dot grid renders blank") {
  BrailleGrid g;
  g.ensure(10, 10);
  g.set(0, 0, 1);
  CellGrid out(5, 40);
  g.render(5, 40, out);  // dotRows 10 < 5*4 -> blank lines (Go)
  for (int r = 0; r < out.rows(); ++r) {
    for (int c = 0; c < out.cols(); ++c) {
      REQUIRE(out.at(r, c).rune == U' ');
    }
  }
}

TEST_CASE("braillegrid: rng64 matches the Go LCG sequence") {
  // Reference values computed from cliamp rng64 (64-bit LCG, (state>>33)%1000).
  std::uint64_t s = 1;
  REQUIRE(rng64(s) == 0.774);
  REQUIRE(rng64(s) == 0.153);
  REQUIRE(rng64(s) == 0.196);
  REQUIRE(rng64(s) == 0.87);

  s = 0;
  REQUIRE(rng64(s) == 0.807);
  REQUIRE(rng64(s) == 0.424);
  REQUIRE(rng64(s) == 0.937);
}

TEST_CASE("braillegrid: golden output when present") {
  // Documented convention: tier = 1 + ((x*3 + y*5) % 3) for every dot.
  BrailleGrid g;
  g.ensure(20, 80);
  for (int y = 0; y < 20; ++y) {
    for (int x = 0; x < 80; ++x) {
      g.set(x, y, 1 + ((x * 3 + y * 5) % 3));
    }
  }
  CellGrid grid(5, 40);
  g.render(5, 40, grid);
  check_golden("braillegrid", grid);
}
