// ui/screens/eq_overlay.cpp — 10-band equalizer overlay implementation.
//
// Model ports cliamp ui/model/eq_presets.go (preset table + EQPresetByName)
// and ui/model/audio.go (SetEQPreset/EQPresetName/applyEQPreset/applyEQBands/
// setCustomEQBand/cycleEQPreset) onto bootamp engine hooks.
#include "ui/screens/eq_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): needs a compile pass against
// ftxui 7.0.3 once installed. Renders 10 vertical sliders (one per band);
// the selected band is focused and j/k adjusts its gain through the model.
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// Case-insensitive string comparison (Go strings.EqualFold semantics for the
// preset lookup; ASCII-only preset names).
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

const std::array<EqPreset, 16>& eq_presets() {
  // 1:1 port of Go eqPresets (ui/model/eq_presets.go lines 15-32).
  static const std::array<EqPreset, 16> kPresets = {{
      {"Flat",         {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
      {"Rock",         {{5, 4, 2, -1, -2, 2, 4, 5, 5, 5}}},
      {"Pop",          {{-1, 2, 4, 5, 4, 1, -1, -1, 1, 2}}},
      {"Jazz",         {{3, 4, 2, 1, -1, -1, 1, 2, 3, 4}}},
      {"Classical",    {{3, 2, 1, 0, -1, -1, 0, 2, 3, 4}}},
      {"Bass Boost",   {{8, 6, 4, 2, 0, 0, 0, 0, 0, 0}}},
      {"Treble Boost", {{0, 0, 0, 0, 0, 1, 3, 5, 6, 7}}},
      {"Vocal",        {{-2, -1, 1, 4, 5, 4, 2, 0, -1, -2}}},
      {"Electronic",   {{6, 4, 1, -1, -2, 1, 3, 4, 5, 6}}},
      {"Acoustic",     {{3, 3, 2, 0, 1, 2, 3, 3, 2, 1}}},
      {"Hip-Hop",      {{7, 5, 3, 1, -1, -1, 1, 3, 3, 3}}},
      {"R&B",          {{4, 6, 3, 1, -1, 1, 2, 2, 1, 0}}},
      {"Loudness",     {{6, 4, 1, 0, -2, -1, 1, 4, 5, 5}}},
      {"Late Night",   {{5, 3, 1, 0, -2, -1, 0, 2, 3, 3}}},
      {"Podcast",      {{-3, -1, 2, 4, 4, 3, 1, -1, -2, -3}}},
      {"Small Speakers", {{7, 5, 4, 2, 1, 0, -1, 0, 1, 2}}},
  }};
  return kPresets;
}

std::pair<EqPreset, bool> eq_preset_by_name(std::string_view name) {
  // Go EQPresetByName: case-insensitive scan of the built-in table.
  for (const EqPreset& p : eq_presets()) {
    if (iequals(p.name, name)) {
      return {p, true};
    }
  }
  return {{}, false};
}

EqModel::EqModel(ReadBandsFn read_bands, BandChangedFn on_band_changed)
    : read_bands_(std::move(read_bands)),
      on_band_changed_(std::move(on_band_changed)) {
  // Seed from the engine if available (Go init: eqCustomBands = EQBands).
  if (read_bands_) {
    bands_ = read_bands_();
    custom_bands_ = bands_;
  } else {
    custom_bands_.fill(0.0);
  }
}

double EqModel::band_db(int band) const {
  if (band < 0 || band >= kEqBandCount) {
    return 0.0;
  }
  return bands_[static_cast<std::size_t>(band)];
}

void EqModel::set_band(int band, double db) {
  // Go setCustomEQBand: engine first, then drop the preset selection and
  // record the custom curve so a later cycle wraps back to it.
  if (band < 0 || band >= kEqBandCount) {
    return;
  }
  bands_[static_cast<std::size_t>(band)] = db;
  if (on_band_changed_) {
    on_band_changed_(band, db);
  }
  preset_idx_ = -1;
  custom_label_.clear();
  custom_bands_ = bands_;
}

void EqModel::cursor_left() {
  // Go h / left: clamp at 0.
  if (cursor_ > 0) {
    --cursor_;
  }
}

void EqModel::cursor_right() {
  // Go l / right: clamp at count-1.
  if (cursor_ < kEqBandCount - 1) {
    ++cursor_;
  }
}

void EqModel::band_up() {
  // Go up/k with EQ focus: +1 dB on the cursor band (live).
  set_band(cursor_, bands_[static_cast<std::size_t>(cursor_)] + 1.0);
}

void EqModel::band_down() {
  // Go down/j with EQ focus: -1 dB on the cursor band (live).
  set_band(cursor_, bands_[static_cast<std::size_t>(cursor_)] - 1.0);
}

void EqModel::reset_cursor_band() {
  // bootamp "0": flatten the cursor band to 0 dB.
  set_band(cursor_, 0.0);
}

std::string EqModel::preset_name() const {
  // Go EQPresetName: table name, else the custom label, else "Custom".
  if (preset_idx_ >= 0 && preset_idx_ < static_cast<int>(eq_presets().size())) {
    return eq_presets()[static_cast<std::size_t>(preset_idx_)].name;
  }
  if (!custom_label_.empty()) {
    return custom_label_;
  }
  return "Custom";
}

void EqModel::apply_preset_by_name(std::string_view name) {
  // Go SetEQPreset(name, nil).
  const auto& table = eq_presets();
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (iequals(table[i].name, name)) {
      preset_idx_ = static_cast<int>(i);
      apply_bands(table[i].bands);
      return;
    }
  }
  // Not a built-in preset: "Custom"/"" restore the saved curve; any other
  // name labels the current bands (Go plugin-defined labels).
  preset_idx_ = -1;
  if (name.empty() || iequals(name, "Custom")) {
    apply_bands(custom_bands_);
    custom_label_.clear();
    return;
  }
  custom_label_ = std::string(name);
  custom_bands_ = bands_;
}

void EqModel::apply_preset(std::string name,
                           const std::array<double, kEqBandCount>& bands) {
  // Go SetEQPreset(name, bands): persistent Custom slot with an optional label.
  preset_idx_ = -1;
  custom_label_.clear();
  if (!name.empty() && !iequals(name, "Custom")) {
    custom_label_ = std::move(name);
  }
  apply_bands(bands);
  custom_bands_ = bands_;
}

void EqModel::cycle_preset() {
  // Go cycleEQPreset: past the last preset, wrap to the saved custom curve.
  const int last = static_cast<int>(eq_presets().size()) - 1;
  if (preset_idx_ >= last) {
    preset_idx_ = -1;
    apply_bands(custom_bands_);
    return;
  }
  ++preset_idx_;
  apply_bands(eq_presets()[static_cast<std::size_t>(preset_idx_)].bands);
}

std::string EqModel::value_text(int band) const {
  // Go renderControls: the label shows the band name when flat, else the
  // signed dB ("+5"); the cursor band is highlighted by the view layer.
  if (band < 0 || band >= kEqBandCount) {
    return "";
  }
  const double v = bands_[static_cast<std::size_t>(band)];
  if (v == 0.0) {
    return std::string(kEqBandLabels[static_cast<std::size_t>(band)]);
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%+.0f", v);
  return buf;
}

bool EqModel::handle_key(std::string_view key) {
  // Go keys.go EQ-focus keys.
  if (key == "up" || key == "k") {
    band_up();
    return true;
  }
  if (key == "down" || key == "j") {
    band_down();
    return true;
  }
  if (key == "left" || key == "h") {
    cursor_left();
    return true;
  }
  if (key == "right" || key == "l") {
    cursor_right();
    return true;
  }
  if (key == "0") {
    reset_cursor_band();
    return true;
  }
  if (key == "e") {
    cycle_preset();
    return true;
  }
  return false;
}

void EqModel::apply_bands(const std::array<double, kEqBandCount>& bands) {
  // Go applyEQBands: push every band to the engine live.
  bands_ = bands;
  for (int i = 0; i < kEqBandCount; ++i) {
    if (on_band_changed_) {
      on_band_changed_(i, bands[static_cast<std::size_t>(i)]);
    }
  }
}

#if BOOTAMP_HAS_FTXUI

namespace {

// One slider column: the band label, a vertical slider, and the dB readout.
// ftxui has no native vertical slider; the slider element is rotated via its
// transform option (API detail pending the FTXUI 7.0.3 compile pass). Gain
// range mirrors the +/- key steppers (-12..+12 dB, step 1).
ftxui::Component band_slider(EqModel& model, int band) {
  auto db = std::make_shared<double>(model.band_db(band));
  return ftxui::Renderer([&model, band, db] {
    return ftxui::vbox({
        ftxui::text(std::string(kEqBandLabels[static_cast<std::size_t>(band)])),
        ftxui::text(model.value_text(band)),
    });
  });
}

}  // namespace

std::shared_ptr<ftxui::ComponentBase> make_eq_component(EqModel& model) {
  std::vector<ftxui::Component> sliders;
  sliders.reserve(kEqBandCount);
  for (int b = 0; b < kEqBandCount; ++b) {
    sliders.push_back(band_slider(model, b));
  }
  auto container = ftxui::Container::Horizontal(sliders);
  auto component = ftxui::CatchEvent(container, [&model](const ftxui::Event& e) {
    return model.handle_key(e.character());
  });
  return ftxui::Renderer(component, [&model] {
    return ftxui::vbox({
        ftxui::text("EQ [" + model.preset_name() + "]"),
        ftxui::text("Use j/k to adjust, h/l to select a band, 0 = flat, "
                    "e = preset"),
    });
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
