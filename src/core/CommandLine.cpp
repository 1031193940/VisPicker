#include "core/CommandLine.h"

#include <windows.h>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace vispick {
namespace {

int ParsePositiveInteger(std::wstring_view value, int upperBound) {
    int result = 0;
    if (value.empty()) { throw std::invalid_argument("Expected an integer."); }
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument("Expected an unsigned decimal integer.");
        }
        const int digit = character - L'0';
        if (result > (upperBound - digit) / 10) { throw std::invalid_argument("Integer argument is out of range."); }
        result = result * 10 + digit;
        if (result > upperBound) { throw std::invalid_argument("Integer argument is out of range."); }
    }
    if (result < 1) { throw std::invalid_argument("Integer argument must be positive."); }
    return result;
}

} // namespace

CommandLine ParseCommandLine(std::span<const std::wstring> arguments) {
    CommandLine result;
    bool hasCommand = false;
    bool hasOutput = false;
    bool hasIterations = false;
    bool hasSeconds = false;
    for (std::size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
        const auto& argument = arguments[argumentIndex];
        if (argument == L"--output" || argument == L"--iterations" || argument == L"--seconds") {
            if (++argumentIndex == arguments.size()) { throw std::invalid_argument("Missing option value."); }
            const auto& value = arguments[argumentIndex];
            if (argument == L"--output") {
                if (hasOutput || value.empty() || value.starts_with(L"--")) {
                    throw std::invalid_argument("Invalid or duplicate output directory.");
                }
                result.options.outputDirectory = value;
                hasOutput = true;
            } else if (argument == L"--iterations") {
                if (hasIterations) { throw std::invalid_argument("Duplicate iterations option."); }
                result.options.iterations = ParsePositiveInteger(value, 10000);
                hasIterations = true;
            } else {
                if (hasSeconds) { throw std::invalid_argument("Duplicate seconds option."); }
                result.options.seconds = ParsePositiveInteger(value, 60);
                hasSeconds = true;
            }
            continue;
        }
        if (hasCommand) { throw std::invalid_argument("Specify exactly one spike command."); }
        if (argument == L"--help" || argument == L"-h") {
            result.command = SpikeCommand::Help;
        } else if (argument == L"--spike-capture") {
            result.command = SpikeCommand::Capture;
        } else if (argument == L"--spike-overlay") {
            result.command = SpikeCommand::Overlay;
        } else if (argument == L"--spike-pin") {
            result.command = SpikeCommand::Pin;
        } else if (argument == L"--spike-gif") {
            result.command = SpikeCommand::Gif;
        } else if (argument == L"--spike-gif-pipeline") {
            result.command = SpikeCommand::GifPipeline;
        } else if (argument == L"--spike-quantizer") {
            result.command = SpikeCommand::Quantizer;
        } else if (argument == L"--all" || argument == L"--spike-all") {
            result.command = SpikeCommand::All;
        } else if (argument == L"--desktop-tests") {
            result.command = SpikeCommand::Desktop;
        } else {
            throw std::invalid_argument("Unknown command-line option. Use --help.");
        }
        hasCommand = true;
    }
    if (!hasCommand && (hasOutput || hasIterations || hasSeconds)) {
        throw std::invalid_argument("Options require a spike command.");
    }
    return result;
}

std::wstring CommandLineHelp() {
    return L"VisPickSpikes — 开发专用技术验证工具\n\n"
           L"--spike-capture   虚拟桌面和 1080p 截屏计时\n"
           L"--spike-overlay   浮层脏矩形重绘计时\n"
           L"--spike-pin       分层窗口贴图验证\n"
           L"--spike-gif       顺序 GIF 正确性检查，不判断零丢帧\n"
           L"--spike-gif-pipeline  独立图样进程、采集与编码流水线\n"
           L"--spike-quantizer 精确调色板 / Wu 微基准\n"
           L"--spike-all       依次运行全部开发检查\n"
           L"--output <目录>   报告和测试 GIF 输出位置\n"
           L"--iterations N   浮层重绘次数，默认 1000\n"
           L"--seconds N      GIF 录制秒数，默认 10，范围 1–60\n\n"
           L"未指定输出位置时使用临时目录。测试窗口会自动关闭。\n"
           L"远程会话、未知显示设备或刷新率低于 50 Hz 时不计为验收通过。\n"
           L"详细开发进度见 docs/DEVELOPMENT.md。";
}

} // namespace vispick
