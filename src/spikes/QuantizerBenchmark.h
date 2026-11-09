#pragma once

#include "spikes/Spikes.h"

namespace vispick {

// CPU-only synthetic and optional WIC image quality benchmarks; no desktop capture.
// VISPICK_QUALITY_INPUT overrides docs/reports/data/quality-input (nonrecursive).
SpikeResult RunQuantizerBenchmark(const SpikeOptions& options);

} // namespace vispick
