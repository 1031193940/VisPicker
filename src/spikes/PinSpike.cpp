#include "spikes/Spikes.h"

#include "capture/ScreenGrab.h"
#include "platform/Compat.h"
#include "spikes/VisualSupport.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace vispick {
namespace {

constexpr int PinWidth = 320;
constexpr int PinHeight = 240;
constexpr int TransparentMargin = 4;
constexpr int TranslucentMargin = 8;

std::uint32_t PinPixel(const int column, const int row) {
    if (column < TransparentMargin || column >= PinWidth - TransparentMargin ||
        row < TransparentMargin || row >= PinHeight - TransparentMargin) {
        return 0;
    }
    if (column < TranslucentMargin || column >= PinWidth - TranslucentMargin ||
        row < TranslucentMargin || row >= PinHeight - TranslucentMargin) {
        constexpr std::uint32_t alpha = 128;
        return (alpha << 24U) | ((60U * alpha / 255U) << 16U) |
               ((120U * alpha / 255U) << 8U) | (240U * alpha / 255U);
    }
    return spikes::CoordinatePixel(column + 571, row + 811);
}

class LayeredPin final : public Window {
public:
    explicit LayeredPin(const POINT position) : surface_(PinWidth, PinHeight), position_(position) {
        auto pixels = surface_.Pixels();
        for (int row = 0; row < PinHeight; ++row) {
            for (int column = 0; column < PinWidth; ++column) {
                pixels[static_cast<std::size_t>(row) * PinWidth + static_cast<std::size_t>(column)] =
                    PinPixel(column, row);
            }
        }
        Create({L"VisPick.Spike.Pin", CS_DBLCLKS, IDC_ARROW}, L"VisPick layered pin benchmark", WS_POPUP,
               WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
               {position.x, position.y, position.x + PinWidth, position.y + PinHeight}, WindowActivation::Passive);
        DisableWindowAnimations(Handle());
    }

    ~LayeredPin() override { Destroy(); }

    void Present(const BYTE opacity) {
        POINT source{0, 0};
        SIZE size{PinWidth, PinHeight};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
        ScreenDc desktop;
        if (!UpdateLayeredWindow(Handle(), desktop.Get(), &position_, &size,
                                 surface_.DeviceContext(), &source, 0, &blend, ULW_ALPHA)) {
            throw std::runtime_error("UpdateLayeredWindow failed");
        }
        ShowWindow(Handle(), SW_SHOWNOACTIVATE);
    }

private:
    DibSection surface_;
    POINT position_;
};

std::uint64_t ComparePin(const BitmapView captured, const POINT origin,
                         const BYTE opacity, int& maximumChannelError) {
    std::uint64_t mismatchedPixels = 0;
    for (int row = 0; row < PinHeight; ++row) {
        for (int column = 0; column < PinWidth; ++column) {
            const std::uint32_t foreground = PinPixel(column, row);
            const std::uint32_t background = spikes::CoordinatePixel(origin.x + column, origin.y + row);
            const std::uint32_t actual = captured.Row(row)[column];
            const double constantAlpha = static_cast<double>(opacity) / 255.0;
            const double alpha = static_cast<double>(foreground >> 24U) / 255.0 * constantAlpha;
            bool differs = false;
            for (const unsigned shift : {0U, 8U, 16U}) {
                const double sourceChannel = static_cast<double>((foreground >> shift) & 255U);
                const double backgroundChannel = static_cast<double>((background >> shift) & 255U);
                const int expected = static_cast<int>(std::lround(sourceChannel * constantAlpha + backgroundChannel * (1.0 - alpha)));
                const int error = std::abs(static_cast<int>((actual >> shift) & 255U) - expected);
                maximumChannelError = std::max(maximumChannelError, error);
                // DWM alpha uses integer rounding. Opaque/transparent pixels must still be exact.
                const int tolerance = opacity == 255 && (foreground >> 24U) != 128U ? 0 : 1;
                differs = differs || error > tolerance;
            }
            if (differs) { ++mismatchedPixels; }
        }
    }
    return mismatchedPixels;
}

} // namespace

SpikeResult RunPinSpike(const SpikeOptions&) {
    SpikeResult result{"pin", false, {}, {}};
    const auto monitors = EnumerateMonitors();
    if (monitors.empty()) { throw std::runtime_error("No monitors available for pin spike"); }
    const auto primary = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& monitor) {
        return monitor.primary;
    });
    const RECT workArea = (primary == monitors.end() ? monitors.front() : *primary).workArea;
    if (workArea.right - workArea.left < PinWidth || workArea.bottom - workArea.top < PinHeight) {
        throw std::runtime_error("Monitor is smaller than the 320x240 pin test");
    }
    const POINT origin{workArea.left + (workArea.right - workArea.left - PinWidth) / 2,
                       workArea.top + (workArea.bottom - workArea.top - PinHeight) / 2};
    const RECT bounds{origin.x, origin.y, origin.x + PinWidth, origin.y + PinHeight};
    spikes::PatternWindow background(bounds);
    DisableWindowAnimations(background.Handle());
    background.Show();
    PumpWindowMessages();
    DwmFlush();

    // Warm up window-class and compositor resources before sampling process handles.
    {
        LayeredPin pin(origin);
        pin.Present(255);
        GdiFlush();
        pin.Destroy();
    }
    PumpWindowMessages();
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    std::vector<double> presentTimes;
    presentTimes.reserve(100);
    std::uint64_t mismatchedPixels = 0;
    int maximumChannelError = 0;
    bool positionMatches = true;
    for (int iteration = 0; iteration < 100; ++iteration) {
        LayeredPin pin(origin);
        const auto begin = spikes::Clock::now();
        pin.Present(255);
        GdiFlush();
        presentTimes.push_back(spikes::ElapsedMilliseconds(begin));
        RECT actualBounds{};
        positionMatches = positionMatches && GetWindowRect(pin.Handle(), &actualBounds) &&
            EqualRect(&actualBounds, &bounds);
        if (iteration == 0 || iteration == 99) {
            PumpWindowMessages();
            DwmFlush();
            CaptureSession capture(bounds);
            capture.Capture(true);
            mismatchedPixels += ComparePin(capture.Image(), origin, 255, maximumChannelError);
            pin.Present(204);
            DwmFlush();
            capture.Capture(true);
            mismatchedPixels += ComparePin(capture.Image(), origin, 204, maximumChannelError);
        }
        pin.Destroy();
        PumpWindowMessages();
    }
    GdiFlush();
    const int gdiDelta = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - static_cast<int>(gdiBefore);
    const int userDelta = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - static_cast<int>(userBefore);
    result.passed = background.PaintSucceeded() && positionMatches && mismatchedPixels == 0 &&
                    std::abs(gdiDelta) <= 5 && std::abs(userDelta) <= 5;
    result.measurements = {
        {"pin_width", PinWidth, "px"},
        {"pin_height", PinHeight, "px"},
        {"creation_cycles", 100, "cycles"},
        {"position_matches", positionMatches ? 1.0 : 0.0, "boolean"},
        {"mismatched_pixels", static_cast<double>(mismatchedPixels), "pixels"},
        {"maximum_channel_error", static_cast<double>(maximumChannelError), "levels"},
        {"present_average", std::accumulate(presentTimes.begin(), presentTimes.end(), 0.0) /
                                static_cast<double>(presentTimes.size()), "ms"},
        {"present_max", *std::max_element(presentTimes.begin(), presentTimes.end()), "ms"},
        {"gdi_object_delta", static_cast<double>(gdiDelta), "objects"},
        {"user_object_delta", static_cast<double>(userDelta), "objects"}
    };
    result.notes.emplace_back("320x240 layered window with opaque center, premultiplied-alpha border and transparent margin. Capture compares opaque RGB exactly; blended RGB allows one channel level of compositor rounding.");
    result.notes.emplace_back("100 create/present/destroy cycles. Pixel checks run at 100% and 80% global opacity on the first and last cycles. This prototype does not implement P4 pin interactions.");
    background.Destroy();
    return result;
}

} // namespace vispick
