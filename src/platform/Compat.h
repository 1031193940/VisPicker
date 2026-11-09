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
    std::wstring adapterName;
    DWORD refreshHz = 0;
    DWORD adapterFlags = 0;
    bool physicalDisplayVerified = false;
};

struct TestEnvironment {
    bool remoteSession = false;
    bool remoteSessionMetric = false;
    bool sessionDetectionKnown = false;
    bool physicalAcceptanceEligible = false;
    std::vector<MonitorInfo> monitors;
    std::vector<std::string> invalidReasons;
};

[[nodiscard]] TestEnvironment DetectTestEnvironment();
// Pure policy used by tests as well as the OS detector. Unknown is ineligible.
void EvaluateTestEnvironment(TestEnvironment& environment);

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
