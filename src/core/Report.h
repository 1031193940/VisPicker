#pragma once

#include "spikes/Spikes.h"
#include "platform/Compat.h"
#include <span>
#include <string_view>

namespace vispick {

[[nodiscard]] std::string JsonEscape(std::string_view text);
[[nodiscard]] std::string FormatMeasurement(double value);
[[nodiscard]] std::filesystem::path DefaultReportDirectory();
void WriteReports(const std::filesystem::path& directory, std::span<const SpikeResult> results);
void WriteReports(const std::filesystem::path& directory, std::span<const SpikeResult> results,
                  const TestEnvironment& environment);
[[nodiscard]] bool AcceptancePassed(std::span<const SpikeResult> results, const TestEnvironment& environment);

} // namespace vispick

