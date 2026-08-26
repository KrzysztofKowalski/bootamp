// ui/screens/eq_overlay.hpp — 10-band equalizer overlay: model + FTXUI component.
//
// Port of cliamp's EQ: the preset table from ui/model/eq_presets.go, the EQ
// state machine from ui/model/audio.go (SetEQPreset/EQPresetName/applyEQBands/
// setCustomEQBand/cycleEQPreset), and the cursor keys from ui/model/keys.go
// (up/down ±1 dB, h/l + left/right band cursor). The model is plain C++ (no
// FTXUI): band values mutate live through an injected engine hook
// (AudioEngine::set_eq_band), so audio changes immediately while the host
// debounces the config save (Go scheduleEQSave). The FTXUI Component glue
// (10 vertical sliders) is compiled only when BOOTAMP_HAS_FTXUI.
//
// Bands: 70, 180, 320, 600, 1k, 3k, 6k, 12k, 14k, 16k Hz (Go eqPresets comment).
// Keys (EQ focus): j/k or up/down = ±1 dB on the cursor band, h/l or left/right
// = move the band cursor, 0 = reset the cursor band to 0 dB flat, e = cycle
// preset (Flat → … → Small Speakers → Custom, Go cycleEQPreset wrap).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#if BOOTAMP_HAS_FTXUI
// Forward declaration of the FTXUI component base class at global scope.
// Kept out of namespace bootamp::ui::screens so it refers to the real
// ::ftxui::ComponentBase rather than a nested bootamp::ui::screens::ftxui.
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// kEqBandCount — number of EQ bands (Go eqBandCount).
inline constexpr int kEqBandCount = 10;

// kEqBandLabels — band display labels in Hz order (Go renderControls eqLabels).
inline constexpr std::array<std::string_view, kEqBandCount> kEqBandLabels = {
    "70", "180", "320", "600", "1k", "3k", "6k", "12k", "14k", "16k"};

// kEqBandFreqs — band center frequencies in Hz (Go eqPresets.go comment).
inline constexpr std::array<int, kEqBandCount> kEqBandFreqs = {
    70, 180, 320, 600, 1000, 3000, 6000, 12000, 14000, 16000};

// EqPreset — a named 10-band EQ curve (Go model.EQPreset).
struct EqPreset {
  std::string                      name;
  std::array<double, kEqBandCount> bands;
};

// eq_presets returns the built-in preset table in Go order (eq_presets.go):
// Flat, Rock, Pop, Jazz, Classical, Bass Boost, Treble Boost, Vocal,
// Electronic, Acoustic, Hip-Hop, R&B, Loudness, Late Night, Podcast,
// Small Speakers.
const std::array<EqPreset, 16>& eq_presets();

// eq_preset_by_name looks up a built-in preset by case-insensitive name
// (Go EQPresetByName). Returns {name, found}.
std::pair<EqPreset, bool> eq_preset_by_name(std::string_view name);

// EqModel holds the equalizer screen state. Band mutations call the injected
// on_band_changed hook live (the host wires AudioEngine::set_eq_band). Band
// values are read from the hook source via read_bands at construction so the
// UI shows the engine's actual state.
class EqModel {
public:
  // read_bands snapshots the current engine bands (engine.eq_bands).
  using ReadBandsFn = std::function<std::array<double, kEqBandCount>()>;
  // on_band_changed applies one band to the engine (engine.set_eq_band).
  using BandChangedFn = std::function<void(int band, double db)>;

  EqModel(ReadBandsFn read_bands = {}, BandChangedFn on_band_changed = {});
  ~EqModel() = default;
  EqModel(const EqModel&)            = delete;
  EqModel& operator=(const EqModel&) = delete;

  void set_read_bands(ReadBandsFn f) { read_bands_ = std::move(f); }
  void set_on_band_changed(BandChangedFn f) { on_band_changed_ = std::move(f); }

  // --- Band values (live) -------------------------------------------------
  double band_db(int band) const;
  const std::array<double, kEqBandCount>& bands() const { return bands_; }
  // set_band applies `db` to `band` immediately (Go setCustomEQBand: engine
  // call + drop any preset selection + record the custom curve).
  void set_band(int band, double db);

  // --- Cursor -------------------------------------------------------------
  int  cursor() const { return cursor_; }
  void cursor_left();   // Go h / left: band-1 (clamped at 0)
  void cursor_right();  // Go l / right: band+1 (clamped at count-1)

  // --- Adjust -------------------------------------------------------------
  void band_up();     // Go up/k: +1 dB on the cursor band
  void band_down();   // Go down/j: -1 dB on the cursor band
  void reset_cursor_band();  // bootamp "0": cursor band back to 0 dB flat

  // --- Presets ------------------------------------------------------------
  // preset_name returns the current preset name or "Custom" (Go EQPresetName;
  // a custom label set via apply_preset_by_name("label") is returned instead).
  std::string preset_name() const;
  // apply_preset_by_name ports Go SetEQPreset: built-in preset names apply the
  // table; "Custom" (or "") restores the saved custom curve; any other name
  // keeps the current bands under that label.
  void apply_preset_by_name(std::string_view name);
  // apply_preset applies a raw curve and labels it (Go SetEQPreset bands
  // branch: preset index cleared, custom label = name unless "Custom").
  void apply_preset(std::string name, const std::array<double, kEqBandCount>& bands);
  // cycle_preset advances to the next built-in preset, wrapping to the saved
  // custom curve after the last one (Go cycleEQPreset).
  void cycle_preset();

  // --- State for persistence (host debounced save, Go saveEQ) -------------
  int  preset_index() const { return preset_idx_; }       // -1 = custom
  const std::array<double, kEqBandCount>& custom_bands() const { return custom_bands_; }
  const std::string& custom_label() const { return custom_label_; }

  // --- Rendering data (Go renderControls) ---------------------------------
  // value_text renders the band readout: the band label when flat, else the
  // signed dB value ("+5", "-3", ...) — Go: label = fmt.Sprintf("%+.0f", v)
  // when non-zero.
  std::string value_text(int band) const;

  // handle_key dispatches EQ-focus keys; returns true if consumed.
  bool handle_key(std::string_view key);

private:
  void apply_bands(const std::array<double, kEqBandCount>& bands);

  ReadBandsFn    read_bands_;
  BandChangedFn  on_band_changed_;

  std::array<double, kEqBandCount> bands_        {};
  std::array<double, kEqBandCount> custom_bands_ {};
  std::string                      custom_label_;
  int                              preset_idx_   = -1;  // Go: -1 = custom
  int                              cursor_       = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see eq_overlay.cpp).
// Renders the 10 vertical sliders; band cursor and gain keys drive the model.
std::shared_ptr<ftxui::ComponentBase> make_eq_component(EqModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
