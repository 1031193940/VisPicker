#pragma once

#include "platform/Gdi.h"
#include "platform/Window.h"

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 1
#endif
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace vispick::spikes {

using Clock = std::chrono::steady_clock;

inline double ElapsedMilliseconds(const Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

struct ProcessMemory {
    std::uint64_t privateCommitBytes = 0;
    std::uint64_t workingSetBytes = 0;
};

inline ProcessMemory ReadProcessMemory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return {static_cast<std::uint64_t>(counters.PrivateUsage), static_cast<std::uint64_t>(counters.WorkingSetSize)};
}

class PaintScope {
public:
    explicit PaintScope(const HWND window) : window_(window), context_(BeginPaint(window, &paint_)) {}
    ~PaintScope() { EndPaint(window_, &paint_); }
    PaintScope(const PaintScope&) = delete;
    PaintScope& operator=(const PaintScope&) = delete;
    [[nodiscard]] HDC Context() const noexcept { return context_; }
    [[nodiscard]] const RECT& Bounds() const noexcept { return paint_.rcPaint; }
private:
    HWND window_;
    PAINTSTRUCT paint_{};
    HDC context_;
};

class SavedDc {
public:
    explicit SavedDc(const HDC context) : context_(context), saved_(SaveDC(context)) {
        if (!saved_) { throw std::runtime_error("SaveDC failed"); }
    }
    ~SavedDc() { RestoreDC(context_, saved_); }
    SavedDc(const SavedDc&) = delete;
    SavedDc& operator=(const SavedDc&) = delete;
private:
    HDC context_;
    int saved_;
};

// Every channel varies with physical position, including high coordinate bits.
// A shifted or DPI-scaled capture therefore fails exact RGB comparison.
inline std::uint32_t CoordinatePixel(const int physicalX, const int physicalY) noexcept {
    const auto horizontal = static_cast<std::uint32_t>(physicalX);
    const auto vertical = static_cast<std::uint32_t>(physicalY);
    const auto red = (horizontal * 17U + vertical * 3U + (horizontal >> 8U)) & 255U;
    const auto green = (vertical * 29U + horizontal * 5U + (vertical >> 8U)) & 255U;
    const auto blue = ((horizontal ^ vertical) * 11U + (horizontal >> 4U)) & 255U;
    return 0xFF000000U | (red << 16U) | (green << 8U) | blue;
}

class PatternWindow final : public Window {
public:
    explicit PatternWindow(const RECT bounds)
        : surface_(bounds.right - bounds.left, bounds.bottom - bounds.top) {
        auto pixels = surface_.Pixels();
        for (int row = 0; row < surface_.Height(); ++row) {
            for (int column = 0; column < surface_.Width(); ++column) {
                pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(surface_.Width()) +
                       static_cast<std::size_t>(column)] = CoordinatePixel(bounds.left + column, bounds.top + row);
            }
        }
        Create({L"VisPick.Spike.Pattern", CS_DBLCLKS, IDC_ARROW}, L"VisPick automated pixel pattern", WS_POPUP,
               WS_EX_TOOLWINDOW | WS_EX_TOPMOST, bounds, WindowActivation::Passive);
    }

    ~PatternWindow() override { Destroy(); }

    void Show() {
        ShowWindow(Handle(), SW_SHOWNOACTIVATE);
        UpdateWindow(Handle());
    }
    [[nodiscard]] bool PaintSucceeded() const noexcept { return paintSucceeded_; }

protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_PAINT) {
            PaintScope paint(Handle());
            const RECT dirty = paint.Bounds();
            paintSucceeded_ = BitBlt(paint.Context(), dirty.left, dirty.top,
                dirty.right - dirty.left, dirty.bottom - dirty.top,
                surface_.DeviceContext(), dirty.left, dirty.top, SRCCOPY) != FALSE;
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }

private:
    DibSection surface_;
    bool paintSucceeded_ = false;
};

inline double Percentile(std::vector<double> samples, const double fraction) {
    if (samples.empty()) { return 0.0; }
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

} // namespace vispick::spikes
