#include "core/AppCommand.h"
#include "VisPickVersion.h"

#include <array>
#include <stdexcept>
#include <string_view>

namespace vispick {

LaunchOptions ParseLaunchOptions(std::span<const std::wstring> arguments) {
    LaunchOptions options;
    bool hasCommand = false;
    bool hasConfig = false;
    constexpr std::array commands{std::pair{L"--screenshot", AppCommand::Screenshot},
                                  std::pair{L"--gif", AppCommand::Gif},
                                  std::pair{L"--pin", AppCommand::Pin},
                                  std::pair{L"--pin-clipboard", AppCommand::PinClipboard},
                                  std::pair{L"--pin-file", AppCommand::PinFile},
                                  std::pair{L"--toggle-pins", AppCommand::TogglePins},
                                  std::pair{L"--reload", AppCommand::Reload},
                                  std::pair{L"--exit", AppCommand::Exit}};
    for (std::size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
        const auto& argument = arguments[argumentIndex];
        if (argument == L"--help" || argument == L"-h") {
            if (arguments.size() != 1) { throw std::invalid_argument("--help must be used alone"); }
            options.help = true;
        } else if (argument == L"--version") {
            if (arguments.size() != 1) { throw std::invalid_argument("--version must be used alone"); }
            options.version = true;
        } else if (argument == L"--config") {
            if (hasConfig || ++argumentIndex == arguments.size() || arguments[argumentIndex].empty() ||
                arguments[argumentIndex].starts_with(L"--")) {
                throw std::invalid_argument("--config requires exactly one nonempty file path");
            }
            options.configPath = arguments[argumentIndex];
            hasConfig = true;
        } else if (argument == L"--perf") {
            if (options.performanceLogging) { throw std::invalid_argument("Duplicate --perf option"); }
            options.performanceLogging = true;
        } else {
            bool recognized = false;
            for (const auto& [name, command] : commands) {
                if (argument != name) { continue; }
                if (hasCommand) { throw std::invalid_argument("Choose only one action per invocation"); }
                options.command = command;
                hasCommand = true;
                recognized = true;
                if (command == AppCommand::PinFile) {
                    if (++argumentIndex == arguments.size() || arguments[argumentIndex].empty() ||
                        arguments[argumentIndex].starts_with(L"--")) {
                        throw std::invalid_argument("--pin-file requires a nonempty image path");
                    }
                    options.pinFile = arguments[argumentIndex];
                }
                break;
            }
            if (!recognized) { throw std::invalid_argument("Unknown command-line option; use --help"); }
        }
    }
    return options;
}

std::wstring ApplicationHelp() {
    return L"VisPick " VISPICK_PRODUCT_VERSION_W L" — 截图、参考图与 GIF 录制\n\n"
           L"无参数启动后常驻托盘。Ctrl+1 截图，选区后可贴图、复制或保存。Ctrl+2 录制 GIF。\n"
           L"拖出选区后 Enter 复制，R/E/A/P/T/M 标注，Ctrl+S 保存。\n\n"
           L"用法：VisPick.exe [动作] [--config <文件>] [--perf]\n"
           L"动作：--screenshot | --gif | --pin | --pin-clipboard\n"
           L"      --pin-file <图片> | --toggle-pins | --reload | --exit\n"
           L"      --help | --version（单独使用）\n\n"
           L"重复启动会把动作转发给当前实例。配置保存后自动生效。\n"
           L"开发实验使用 VisPickSpikes.exe；详细说明见 README.md。\n";
}

} // namespace vispick
