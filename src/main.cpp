#include "App.h"
#include "VisPickVersion.h"
#include "core/AppCommand.h"
#include "core/CrashDump.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/SingleInstance.h"

#include <windows.h>
#include <stdexcept>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    std::vector<std::wstring> arguments;
    try {
        arguments = vispick::ProcessArguments();
        auto options = vispick::ParseLaunchOptions(arguments);
        if (options.help) {
            vispick::ConsoleOutput{}.Show(vispick::ApplicationHelp());
            return 0;
        }
        if (options.version) {
            vispick::ConsoleOutput{}.Show(L"VisPick " VISPICK_PRODUCT_VERSION_W L"\n");
            return 0;
        }
        // Resolve caller-relative paths before forwarding to another process,
        // whose working directory may be entirely different.
        if (!options.configPath.empty()) {
            options.configPath = std::filesystem::absolute(options.configPath).lexically_normal();
        }
        if (!options.pinFile.empty()) {
            options.pinFile = std::filesystem::absolute(options.pinFile).lexically_normal();
        }
        vispick::SingleInstance instance;
        if (!instance.IsPrimary()) {
            const auto forwarded = instance.Forward(options);
            if (forwarded == vispick::ForwardResult::Delivered) { return 0; }
            (void)vispick::ConsoleOutput{}.Write(
                forwarded == vispick::ForwardResult::ConfigConflict
                    ? L"VisPick 已使用另一个配置文件运行。请先退出已有实例，再指定新的 --config 路径。\n"
                    : L"无法把命令交给已有的 VisPick 实例；它可能正在启动或暂时没有响应。\n",
                true);
            return 4;
        }
        if (options.command == vispick::AppCommand::Exit) { return 0; }
        vispick::InitializeDpiAwareness();
        vispick::CrashDump crashDump;
        const auto crashStatus = crashDump.Install();
        if (!crashStatus.installed) { OutputDebugStringW(L"VisPick crash reporting could not be installed.\n"); }
        vispick::App application;
        return application.Run(options, instance.WindowTitle());
    } catch (const std::invalid_argument& error) {
        const auto detail = arguments.empty() ? std::wstring{} : L"\n参数：" + arguments.front();
        (void)vispick::ConsoleOutput{}.Write(
            L"VisPick: " + vispick::ExceptionMessage(error) + detail + L"\n使用 --help 查看用法。\n", true);
        return 2;
    } catch (const std::exception& error) {
        (void)vispick::ConsoleOutput{}.Write(L"VisPick: " + vispick::ExceptionMessage(error) + L"\n", true);
        return 3;
    } catch (...) {
        (void)vispick::ConsoleOutput{}.Write(L"VisPick: unexpected application error.\n", true);
        return 3;
    }
}
