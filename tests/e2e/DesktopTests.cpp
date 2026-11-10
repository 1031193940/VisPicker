#include "DesktopTests.h"

#include "capture/ScreenGrab.h"
#include "platform/Compat.h"
#include "spikes/TestEnvironment.h"
#include "spikes/VisualSupport.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vispick {
namespace {

std::uint64_t CountMismatches(BitmapView actual, const RECT capturedBounds,
                              const RECT checkedBounds, std::uint64_t& comparedPixels) {
    std::uint64_t mismatches = 0;
    for (LONG screenY = checkedBounds.top; screenY < checkedBounds.bottom; ++screenY) {
        const auto* actualRow = actual.Row(screenY - capturedBounds.top);
        for (LONG screenX = checkedBounds.left; screenX < checkedBounds.right; ++screenX) {
            const auto expected = spikes::CoordinatePixel(screenX, screenY) & 0xFFFFFFU;
            if ((actualRow[screenX - capturedBounds.left] & 0xFFFFFFU) != expected) { ++mismatches; }
            ++comparedPixels;
        }
    }
    return mismatches;
}

} // namespace

SpikeResult RunDesktopTests(const SpikeOptions&) {
    SpikeResult result{"desktop-pixels", false, {}, {}};
    const RECT desktop = VirtualDesktopBounds();
    const auto environment = DetectTestEnvironment();
    const auto& monitors = environment.monitors;
    if (monitors.empty()) { throw std::runtime_error("No monitors available for desktop pixel test"); }
    std::uint64_t mismatches = 0;
    std::uint64_t comparedPixels = 0;
    bool windowBoundsMatch = true;
    bool paintingSucceeded = true;
    double captureMilliseconds = 0.0;
    {
        // Separate monitor windows keep physical pixel coordinates explicit under mixed DPI.
        std::vector<std::unique_ptr<spikes::PatternWindow>> patterns;
        for (const auto& monitor : monitors) {
            auto pattern = std::make_unique<spikes::PatternWindow>(monitor.bounds);
            DisableWindowAnimations(pattern->Handle());
            pattern->Show();
            RECT actualBounds{};
            windowBoundsMatch = windowBoundsMatch && GetWindowRect(pattern->Handle(), &actualBounds) &&
                                EqualRect(&actualBounds, &monitor.bounds);
            paintingSucceeded = paintingSucceeded && pattern->PaintSucceeded();
            patterns.push_back(std::move(pattern));
        }
        PumpWindowMessages();
        GdiFlush();
        DwmFlush();
        const auto beginCapture = spikes::Clock::now();
        const auto frozen = CaptureScreen(desktop);
        captureMilliseconds = spikes::ElapsedMilliseconds(beginCapture);
        for (const auto& monitor : monitors) {
            mismatches += CountMismatches(frozen->Image(), frozen->Bounds(), monitor.bounds, comparedPixels);
        }
        for (const auto& pattern : patterns) { pattern->Destroy(); }
    }
    PumpWindowMessages();
    GdiFlush();

    const RECT monitor = monitors.front().workArea;
    const RECT smallBounds{monitor.left, monitor.top,
        std::min(monitor.right, monitor.left + 320), std::min(monitor.bottom, monitor.top + 240)};
    // Warm up the repeated path before comparing handle counts.
    {
        spikes::PatternWindow pattern(smallBounds);
        pattern.Show();
        CaptureSession capture(smallBounds);
        capture.Capture(true);
        pattern.Destroy();
    }
    PumpWindowMessages();
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int iteration = 0; iteration < 100; ++iteration) {
        spikes::PatternWindow pattern(smallBounds);
        DisableWindowAnimations(pattern.Handle());
        pattern.Show();
        PumpWindowMessages();
        GdiFlush();
        DwmFlush();
        CaptureSession capture(smallBounds);
        capture.Capture(true);
        mismatches += CountMismatches(capture.Image(), smallBounds, smallBounds, comparedPixels);
        paintingSucceeded = paintingSucceeded && pattern.PaintSucceeded();
        pattern.Destroy();
    }
    PumpWindowMessages();
    GdiFlush();
    const int gdiDelta = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - static_cast<int>(gdiBefore);
    const int userDelta = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - static_cast<int>(userBefore);
    result.passed = mismatches == 0 && windowBoundsMatch && paintingSucceeded &&
                    std::abs(gdiDelta) <= 5 && std::abs(userDelta) <= 5 && captureMilliseconds <= 150.0;
    result.measurements = {
        {"desktop_left", static_cast<double>(desktop.left), "px"},
        {"desktop_top", static_cast<double>(desktop.top), "px"},
        {"desktop_width", static_cast<double>(desktop.right - desktop.left), "px"},
        {"desktop_height", static_cast<double>(desktop.bottom - desktop.top), "px"},
        {"monitor_count", static_cast<double>(monitors.size()), "monitors"},
        {"full_desktop_allocate_and_capture", captureMilliseconds, "ms"},
        {"compared_pixels", static_cast<double>(comparedPixels), "pixels"},
        {"mismatched_pixels", static_cast<double>(mismatches), "pixels"},
        {"window_bounds_match", windowBoundsMatch ? 1.0 : 0.0, "boolean"},
        {"capture_window_cycles", 100, "cycles"},
        {"gdi_object_delta", static_cast<double>(gdiDelta), "objects"},
        {"user_object_delta", static_cast<double>(userDelta), "objects"},
        {"pixel_correctness", mismatches == 0 && windowBoundsMatch && paintingSucceeded ? 1.0 : 0.0, "boolean"},
        {"physical_acceptance_eligible", environment.physicalAcceptanceEligible ? 1.0 : 0.0, "boolean"}
    };
    for (std::size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
        const auto& monitorInfo = monitors[monitorIndex];
        result.notes.push_back("Monitor " + std::to_string(monitorIndex + 1) + ": (" +
            std::to_string(monitorInfo.bounds.left) + "," + std::to_string(monitorInfo.bounds.top) + ") " +
            std::to_string(monitorInfo.bounds.right - monitorInfo.bounds.left) + "x" +
            std::to_string(monitorInfo.bounds.bottom - monitorInfo.bounds.top) + ", DPI " +
            std::to_string(monitorInfo.dpiX) + "x" + std::to_string(monitorInfo.dpiY));
    }
    result.notes.emplace_back("One opaque coordinate-encoded pattern per monitor; compares every monitor pixel after full virtual-desktop capture. Unoccupied gaps between monitors are excluded. No mouse/keyboard injection, and no desktop-content screenshots are saved.");
    result.notes.emplace_back("100 repeated 320x240-or-smaller window/capture/destruction cycles check RGB and GDI/USER resource counts. Other DPI/layout combinations require rerunning after changing display settings.");
    result.notes.emplace_back("Full desktop timing includes allocation and capture of the final shared FrozenScreen; no intermediate whole-frame copy is performed. Pixel correctness is reported separately even when the environment is not acceptance eligible.");
    result.notes.insert(result.notes.end(), environment.invalidReasons.begin(), environment.invalidReasons.end());
    return result;
}

} // namespace vispick
