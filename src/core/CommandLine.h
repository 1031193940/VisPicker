#pragma once

#include "spikes/Spikes.h"
#include <span>
#include <string>

namespace vispick {

enum class SpikeCommand { Help, Capture, Overlay, Pin, Gif, GifPipeline, Quantizer, All, Desktop };
struct CommandLine {
    SpikeCommand command = SpikeCommand::Help;
    SpikeOptions options;
};

// Arguments exclude the executable name. Invalid input throws invalid_argument.
[[nodiscard]] CommandLine ParseCommandLine(std::span<const std::wstring> arguments);
[[nodiscard]] std::wstring CommandLineHelp();

} // namespace vispick

