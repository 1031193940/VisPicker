#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace vispick {

enum class AppCommand {
    None, Screenshot, Gif, Pin, PinClipboard, PinFile, TogglePins,
    ShowPins, HidePins, ClosePins, ResetPinPassthrough, RestorePin,
    OpenConfig, Reload, ToggleAutostart, About, Exit
};

struct LaunchOptions {
    AppCommand command = AppCommand::None;
    std::filesystem::path pinFile;
    std::filesystem::path configPath;
    bool help = false;
    bool version = false;
    bool performanceLogging = false;
};

LaunchOptions ParseLaunchOptions(std::span<const std::wstring> arguments);
std::wstring ApplicationHelp();

} // namespace vispick
