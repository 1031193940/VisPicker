#include "platform/Compat.h"

#include "platform/Gdi.h"

#include <dwmapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <stdexcept>

namespace vispick {
namespace {

// Load optional DLLs only from the Windows system directory.
class SystemLibrary {
public:
    explicit SystemLibrary(const wchar_t* name) noexcept {
        wchar_t path[MAX_PATH + 1]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) { return; }
        if (wcscat_s(path, L"\\") != 0 || wcscat_s(path, name) != 0) { return; }
        module_ = LoadLibraryW(path);
    }
    ~SystemLibrary() { if (module_ != nullptr) { FreeLibrary(module_); } }
    SystemLibrary(const SystemLibrary&) = delete;
    SystemLibrary& operator=(const SystemLibrary&) = delete;
    [[nodiscard]] HMODULE Get() const noexcept { return module_; }
private:
    HMODULE module_ = nullptr;
};

template <typename Function>
Function FindFunction(HMODULE module, const char* name) noexcept {
    if (module == nullptr) { return nullptr; }
    const FARPROC address = GetProcAddress(module, name);
    static_assert(sizeof(Function) == sizeof(address));
    Function function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

HMODULE User32() noexcept { return GetModuleHandleW(L"user32.dll"); }

HMODULE Shcore() noexcept {
    static const SystemLibrary library(L"shcore.dll");
    return library.Get();
}

UINT SystemDpi() noexcept {
    try {
        const ScreenDc context;
        const int dpi = GetDeviceCaps(context.Get(), LOGPIXELSX);
        return dpi > 0 ? static_cast<UINT>(dpi) : 96U;
    } catch (...) {
        return 96U;
    }
}

void MonitorDpi(HMONITOR monitor, UINT& dpiX, UINT& dpiY) noexcept {
    using GetDpiForMonitorFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpi = FindFunction<GetDpiForMonitorFunction>(Shcore(), "GetDpiForMonitor");
    if (getDpi != nullptr && SUCCEEDED(getDpi(monitor, 0, &dpiX, &dpiY))) { return; }
    dpiX = SystemDpi();
    dpiY = dpiX;
}

struct MonitorEnumeration {
    std::vector<MonitorInfo> monitors;
    bool failed = false;
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) noexcept {
    auto& enumeration = *reinterpret_cast<MonitorEnumeration*>(parameter);
    try {
        MONITORINFOEXW information{};
        information.cbSize = sizeof(information);
        if (!GetMonitorInfoW(monitor, &information)) {
            enumeration.failed = true;
            return FALSE;
        }
        MonitorInfo result;
        result.bounds = information.rcMonitor;
        result.workArea = information.rcWork;
        result.primary = (information.dwFlags & MONITORINFOF_PRIMARY) != 0;
        result.deviceName = information.szDevice;
        DEVMODEW displayMode{};
        displayMode.dmSize = sizeof(displayMode);
        if (EnumDisplaySettingsW(information.szDevice, ENUM_CURRENT_SETTINGS, &displayMode)) {
            result.refreshHz = displayMode.dmDisplayFrequency;
        }
        MonitorDpi(monitor, result.dpiX, result.dpiY);
        enumeration.monitors.push_back(result);
        return TRUE;
    } catch (...) {
        // Never unwind a C++ exception through a Windows callback.
        enumeration.failed = true;
        return FALSE;
    }
}

bool ContainsWord(const std::wstring& text, const wchar_t* word) {
    std::wstring lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return lowered.find(word) != std::wstring::npos;
}

bool RemoteAdapter(const MonitorInfo& monitor) {
    constexpr DWORD remoteFlag = 0x04000000; // DISPLAY_DEVICE_REMOTE, including newer SDKs.
    return (monitor.adapterFlags & (remoteFlag | DISPLAY_DEVICE_MIRRORING_DRIVER)) != 0 ||
           ContainsWord(monitor.adapterName, L"remote") || ContainsWord(monitor.adapterName, L"rdp");
}

bool NonPhysicalAdapter(const MonitorInfo& monitor) {
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

void AddDisplayDetails(std::vector<MonitorInfo>& monitors) {
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
        const LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                                &modeCount, modes.data(), nullptr);
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
                    allOutputsPhysical[monitorIndex] = allOutputsPhysical[monitorIndex] &&
                        path.targetInfo.targetAvailable && PhysicalConnector(path.targetInfo.outputTechnology) &&
                        (monitor.adapterFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
                        !monitor.adapterName.empty() && !NonPhysicalAdapter(monitor);
                }
            }
        }
        for (std::size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
            monitors[monitorIndex].physicalDisplayVerified = sourceFound[monitorIndex] && allOutputsPhysical[monitorIndex];
        }
        return;
    }
}

} // namespace

void InitializeDpiAwareness() noexcept {
    using SetContextFunction = BOOL(WINAPI*)(HANDLE);
    const auto setContext = FindFunction<SetContextFunction>(User32(), "SetProcessDpiAwarenessContext");
    if (setContext != nullptr) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is an opaque pseudo-handle.
        if (setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) || GetLastError() == ERROR_ACCESS_DENIED) {
            return;
        }
    }
    using SetAwarenessFunction = HRESULT(WINAPI*)(int);
    const auto setAwareness = FindFunction<SetAwarenessFunction>(Shcore(), "SetProcessDpiAwareness");
    if (setAwareness != nullptr) {
        const HRESULT result = setAwareness(2); // PROCESS_PER_MONITOR_DPI_AWARE
        if (SUCCEEDED(result) || result == E_ACCESSDENIED) { return; }
    }
    SetProcessDPIAware(); // Vista/Win7 fallback; manifest normally selected it already.
}

UINT DpiForWindow(HWND window) noexcept {
    using GetDpiFunction = UINT(WINAPI*)(HWND);
    static const auto getDpi = FindFunction<GetDpiFunction>(User32(), "GetDpiForWindow");
    if (getDpi != nullptr) {
        const UINT dpi = getDpi(window);
        if (dpi != 0) { return dpi; }
    }
    UINT dpiX = 96;
    UINT dpiY = 96;
    MonitorDpi(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), dpiX, dpiY);
    return dpiX;
}

RECT AdjustWindowBoundsForDpi(RECT clientBounds, DWORD style, DWORD extendedStyle, UINT dpi) {
    if (dpi == 0 || dpi > static_cast<UINT>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Invalid DPI for window bounds");
    }
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjust = FindFunction<AdjustForDpi>(User32(), "AdjustWindowRectExForDpi");
    if (adjust) {
        if (!adjust(&clientBounds, style, FALSE, extendedStyle, dpi)) { throw std::runtime_error("Cannot calculate DPI-aware window bounds"); }
        return clientBounds;
    }
    RECT frame{};
    if (!AdjustWindowRectEx(&frame, style, FALSE, extendedStyle)) { throw std::runtime_error("Cannot calculate window frame bounds"); }
    const int systemDpi = static_cast<int>(SystemDpi());
    const int targetDpi = static_cast<int>(dpi);
    clientBounds.left += MulDiv(frame.left, targetDpi, systemDpi);
    clientBounds.top += MulDiv(frame.top, targetDpi, systemDpi);
    clientBounds.right += MulDiv(frame.right, targetDpi, systemDpi);
    clientBounds.bottom += MulDiv(frame.bottom, targetDpi, systemDpi);
    return clientBounds;
}

RECT VirtualDesktopBounds() noexcept {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

std::vector<MonitorInfo> EnumerateMonitors() {
    MonitorEnumeration enumeration;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&enumeration)) || enumeration.failed) {
        throw std::runtime_error("Unable to enumerate display monitors");
    }
    return std::move(enumeration.monitors);
}

void EvaluateTestEnvironment(TestEnvironment& environment) {
    environment.invalidReasons.clear();
    environment.remoteSession = environment.remoteSession || environment.remoteSessionMetric;
    for (const auto& monitor : environment.monitors) {
        environment.remoteSession = environment.remoteSession || RemoteAdapter(monitor);
    }
    if (environment.remoteSession) {
        environment.invalidReasons.emplace_back("Remote session/display evidence: physical-display performance acceptance is invalid.");
    }
    if (!environment.sessionDetectionKnown) {
        environment.invalidReasons.emplace_back("Local session identity could not be independently verified.");
    }
    if (environment.monitors.empty()) { environment.invalidReasons.emplace_back("No active display monitors detected."); }
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
    environment.monitors = EnumerateMonitors();
    AddDisplayDetails(environment.monitors);
    DWORD currentSession = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        DWORD glassSession = 0;
        DWORD size = sizeof(glassSession);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
                        L"GlassSessionId", RRF_RT_REG_DWORD, nullptr, &glassSession, &size) == ERROR_SUCCESS) {
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

WindowsVersion GetWindowsVersion() noexcept {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtlGetVersion = FindFunction<RtlGetVersionFunction>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW information{};
    information.dwOSVersionInfoSize = sizeof(information);
    if (rtlGetVersion != nullptr && rtlGetVersion(&information) == 0) {
        return {information.dwMajorVersion, information.dwMinorVersion, information.dwBuildNumber};
    }
    return {};
}

void DisableWindowAnimations(HWND window) noexcept {
    const BOOL disabled = TRUE;
    // DwmSetWindowAttribute and this attribute both predate the Win7 target.
    // With composition disabled the call may fail, which is harmless here.
    (void)DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
}

bool TryExcludeFromCapture(HWND window) noexcept {
    const auto version = GetWindowsVersion();
    // Earlier Windows treats 0x11 as WDA_MONITOR, which produces a black rectangle.
    if (version.major < 10 || version.build < 19041) { return false; }
    using SetAffinityFunction = BOOL(WINAPI*)(HWND, DWORD);
    const auto setAffinity = FindFunction<SetAffinityFunction>(User32(), "SetWindowDisplayAffinity");
    return setAffinity != nullptr && setAffinity(window, 0x11) != FALSE;
}

} // namespace vispick
