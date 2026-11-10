#include "spikes/TestEnvironment.h"

#include <wtsapi32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <utility>

namespace vispick {
namespace {

bool ContainsWord(const std::wstring& text, const wchar_t* word) {
    std::wstring lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return lowered.find(word) != std::wstring::npos;
}

bool RemoteAdapter(const TestMonitorInfo& monitor) {
    constexpr DWORD remoteFlag = 0x04000000; // DISPLAY_DEVICE_REMOTE, including newer SDKs.
    return (monitor.adapterFlags & (remoteFlag | DISPLAY_DEVICE_MIRRORING_DRIVER)) != 0 ||
           ContainsWord(monitor.adapterName, L"remote") || ContainsWord(monitor.adapterName, L"rdp");
}

bool NonPhysicalAdapter(const TestMonitorInfo& monitor) {
    return RemoteAdapter(monitor) || ContainsWord(monitor.adapterName, L"virtual") ||
           ContainsWord(monitor.adapterName, L"indirect") || ContainsWord(monitor.adapterName, L"dummy");
}

bool PhysicalConnector(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) noexcept {
    switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        return true;
    default:
        return false; // Unknown, Miracast and indirect/virtual outputs are not acceptance evidence.
    }
}

void AddDisplayDetails(std::vector<TestMonitorInfo>& monitors) {
    for (auto& monitor : monitors) {
        DEVMODEW displayMode{};
        displayMode.dmSize = sizeof(displayMode);
        if (EnumDisplaySettingsW(monitor.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &displayMode)) {
            monitor.refreshHz = displayMode.dmDisplayFrequency;
        }
    }
    for (DWORD adapterIndex = 0;; ++adapterIndex) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0)) { break; }
        for (auto& monitor : monitors) {
            if (_wcsicmp(monitor.deviceName.c_str(), adapter.DeviceName) == 0) {
                monitor.adapterName = adapter.DeviceString;
                monitor.adapterFlags = adapter.StateFlags;
            }
        }
    }

    // Win7 display configuration identifies the actual active connector. A name
    // containing "Console" or a false SM_REMOTESESSION alone proves nothing.
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) { return; }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG status =
            QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) { continue; }
        if (status != ERROR_SUCCESS) { return; }
        std::vector<bool> sourceFound(monitors.size(), false);
        std::vector<bool> allOutputsPhysical(monitors.size(), true);
        for (UINT32 pathIndex = 0; pathIndex < pathCount; ++pathIndex) {
            const auto& path = paths[pathIndex];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) { continue; }
            for (std::size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
                const auto& monitor = monitors[monitorIndex];
                if (_wcsicmp(monitor.deviceName.c_str(), source.viewGdiDeviceName) == 0) {
                    sourceFound[monitorIndex] = true;
                    allOutputsPhysical[monitorIndex] =
                        allOutputsPhysical[monitorIndex] && path.targetInfo.targetAvailable &&
                        PhysicalConnector(path.targetInfo.outputTechnology) &&
                        (monitor.adapterFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
                        !monitor.adapterName.empty() && !NonPhysicalAdapter(monitor);
                }
            }
        }
        for (std::size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
            monitors[monitorIndex].physicalDisplayVerified =
                sourceFound[monitorIndex] && allOutputsPhysical[monitorIndex];
        }
        return;
    }
}

} // namespace

void EvaluateTestEnvironment(TestEnvironment& environment) {
    environment.invalidReasons.clear();
    environment.remoteSession = environment.remoteSession || environment.remoteSessionMetric;
    for (const auto& monitor : environment.monitors) {
        environment.remoteSession = environment.remoteSession || RemoteAdapter(monitor);
    }
    if (environment.remoteSession) {
        environment.invalidReasons.emplace_back(
            "Remote session/display evidence: physical-display performance acceptance is invalid.");
    }
    if (!environment.sessionDetectionKnown) {
        environment.invalidReasons.emplace_back("Local session identity could not be independently verified.");
    }
    if (environment.monitors.empty()) {
        environment.invalidReasons.emplace_back("No active display monitors detected.");
    }
    for (std::size_t index = 0; index < environment.monitors.size(); ++index) {
        const auto& monitor = environment.monitors[index];
        const std::string prefix = "Monitor " + std::to_string(index + 1) + ": ";
        if (monitor.refreshHz < 50) {
            environment.invalidReasons.push_back(prefix + "refresh rate is unknown or below 50 Hz.");
        }
        if (!monitor.physicalDisplayVerified || NonPhysicalAdapter(monitor) || monitor.adapterName.empty()) {
            environment.invalidReasons.push_back(prefix + "a local physical output path was not verified.");
        }
    }
    environment.physicalAcceptanceEligible = environment.invalidReasons.empty();
}

TestEnvironment DetectTestEnvironment() {
    TestEnvironment environment;
    environment.remoteSessionMetric = GetSystemMetrics(SM_REMOTESESSION) != 0;
    environment.remoteSession = environment.remoteSessionMetric;
    for (auto& monitor : EnumerateMonitors()) { environment.monitors.push_back({std::move(monitor)}); }
    AddDisplayDetails(environment.monitors);
    DWORD currentSession = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        DWORD glassSession = 0;
        DWORD size = sizeof(glassSession);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server", L"GlassSessionId",
                         RRF_RT_REG_DWORD, nullptr, &glassSession, &size) == ERROR_SUCCESS) {
            environment.sessionDetectionKnown = true;
            environment.remoteSession = environment.remoteSession || currentSession != glassSession;
        }
        wchar_t* information = nullptr;
        DWORD returnedBytes = 0;
        const BOOL queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, currentSession,
                                                         WTSClientProtocolType, &information, &returnedBytes);
        const std::unique_ptr<void, decltype(&WTSFreeMemory)> owner(information, WTSFreeMemory);
        if (queried && owner && returnedBytes >= sizeof(USHORT)) {
            USHORT protocol = 0;
            std::memcpy(&protocol, information, sizeof(protocol));
            environment.sessionDetectionKnown = true;
            environment.remoteSession = environment.remoteSession || protocol != 0;
        }
    }
    EvaluateTestEnvironment(environment);
    return environment;
}

} // namespace vispick
