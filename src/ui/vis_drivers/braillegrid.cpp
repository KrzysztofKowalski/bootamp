// ui/vis_drivers/braillegrid.cpp — BrailleGrid rasteriser + factory stub
// (port of cliamp/ui/vis_braillegrid.go).
//
// The Go file defines only shared helpers — the brailleGrid rasteriser and
// rng64 — plus the BrailleGrid dot-grid class in braillegrid.hpp for the
// drivers that draw to a fine subgrid (mirror, sand, geyser, ...). cliamp has
// no BrailleGrid *mode*, so make_braillegrid_driver() returns nullptr: the
// registry declares the factory but the visualizer mode table never calls it.
#include "ui/vis_drivers/braillegrid.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <memory>

namespace bootamp::ui::vis_drivers {

std::unique_ptr<VisDriver> make_braillegrid_driver() {
  return nullptr;  // no BrailleGrid mode exists in cliamp — helper only
}

}  // namespace bootamp::ui::vis_drivers
