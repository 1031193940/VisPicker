#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vispick {

struct Measurement {
    std::string name;
    double value = 0.0;
    std::string unit;
};

struct SpikeResult {
    std::string name;
    bool passed = false;
    std::vector<Measurement> measurements;
    std::vector<std::string> notes;
};

struct SpikeOptions {
    std::filesystem::path outputDirectory;
    int iterations = 1000;
    int seconds = 10;
};

SpikeResult RunCaptureSpike(const SpikeOptions& options);
SpikeResult RunOverlaySpike(const SpikeOptions& options);
SpikeResult RunPinSpike(const SpikeOptions& options);
SpikeResult RunGifSpike(const SpikeOptions& options);
SpikeResult RunGifPipelineSpike(const SpikeOptions& options);
SpikeResult RunQuantizerBenchmark(const SpikeOptions& options);

} // namespace vispick
