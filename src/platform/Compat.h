#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace vispick {

struct MonitorInfo {
    RECT bounds{};
    RECT workArea{};
    UINT dpiX = 96;
    UINT dpiY = 96;
    bool primary = false;
    std::wstring deviceName;
};

struct WindowsVersion {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
};

// Call before creating any HWND. The embedded manifest is the primary contract.
void InitializeDpiAwareness() noexcept;
[[nodiscard]] UINT DpiForWindow(HWND window) noexcept;
// Non-menu top-level window bounds for a physical client rectangle. The newer
// API is resolved at runtime; Win7/8 use the scaled system non-client metrics.
[[nodiscard]] RECT AdjustWindowBoundsForDpi(RECT clientBounds, DWORD style, DWORD extendedStyle, UINT dpi);
[[nodiscard]] RECT VirtualDesktopBounds() noexcept;
[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();
[[nodiscard]] WindowsVersion GetWindowsVersion() noexcept;
void DisableWindowAnimations(HWND window) noexcept;
[[nodiscard]] bool TryExcludeFromCapture(HWND window) noexcept;

} // namespace vispick
