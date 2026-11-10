#include "platform/Compat.h"

#include "platform/Gdi.h"

#include <dwmapi.h>

#include <cstring>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <utility>

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
        MonitorDpi(monitor, result.dpiX, result.dpiY);
        enumeration.monitors.push_back(result);
        return TRUE;
    } catch (...) {
        // Never unwind a C++ exception through a Windows callback.
        enumeration.failed = true;
        return FALSE;
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
