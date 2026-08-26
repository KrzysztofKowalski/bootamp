// ui/vis_drivers/registry.hpp — per-driver factory declarations.
//
// One factory per Go driver file (cliamp/ui/vis_*.go). Each driver lives in
// its own .cpp (vis_drivers/<name>.cpp) and defines exactly its factory.
// visualizer.cpp includes this header to build all_vis_modes(). Drivers are
// implemented by parallel agents, so this header is FIXED: driver agents must
// NOT edit it (or any shared ui header). Factories return nullptr on failure.
#pragma once

#include "ui/vis_driver.hpp"

#include <memory>

namespace bootamp::ui::vis_drivers {

#define BOOTAMP_VIS_FACTORY(NAME)                       \
  std::unique_ptr<bootamp::ui::VisDriver> make_##NAME##_driver()

BOOTAMP_VIS_FACTORY(ascii);
BOOTAMP_VIS_FACTORY(bars);
BOOTAMP_VIS_FACTORY(bars_dot);
BOOTAMP_VIS_FACTORY(bars_outline);
BOOTAMP_VIS_FACTORY(binary);
BOOTAMP_VIS_FACTORY(braillegrid);
BOOTAMP_VIS_FACTORY(bricks);
BOOTAMP_VIS_FACTORY(bubbles);
BOOTAMP_VIS_FACTORY(butterfly);
BOOTAMP_VIS_FACTORY(classic_led);
BOOTAMP_VIS_FACTORY(classic_peak);
BOOTAMP_VIS_FACTORY(columns);
BOOTAMP_VIS_FACTORY(firefly);
BOOTAMP_VIS_FACTORY(firework);
BOOTAMP_VIS_FACTORY(flame);
BOOTAMP_VIS_FACTORY(geyser);
BOOTAMP_VIS_FACTORY(heartbeat);
BOOTAMP_VIS_FACTORY(logo);
BOOTAMP_VIS_FACTORY(matrix);
BOOTAMP_VIS_FACTORY(mirror);
BOOTAMP_VIS_FACTORY(mosaic);
BOOTAMP_VIS_FACTORY(pulse);
BOOTAMP_VIS_FACTORY(rain);
BOOTAMP_VIS_FACTORY(retro);
BOOTAMP_VIS_FACTORY(sakura);
BOOTAMP_VIS_FACTORY(sand);
BOOTAMP_VIS_FACTORY(scatter);
BOOTAMP_VIS_FACTORY(scope);
BOOTAMP_VIS_FACTORY(stereo);
BOOTAMP_VIS_FACTORY(terrain);
BOOTAMP_VIS_FACTORY(wave);

#undef BOOTAMP_VIS_FACTORY

}  // namespace bootamp::ui::vis_drivers
