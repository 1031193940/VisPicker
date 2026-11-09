#include "spikes/Spikes.h"

#include "capture/ScreenGrab.h"
#include "platform/Compat.h"
#include "spikes/VisualSupport.h"
#include "platform/Compat.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

namespace vispick {
namespace {

class OverlayWindow final : public Window {
public:
    explicit OverlayWindow(const RECT bounds)
        : client_{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top},
          frozen_(CaptureScreen(bounds)), masked_(client_.right, client_.bottom),
          backBuffer_(client_.right, client_.bottom) {
        const auto source = frozen_->Image().Pixels();
        auto destination = masked_.Pixels();
        for (std::size_t pixelIndex = 0; pixelIndex < source.size(); ++pixelIndex) {
            const auto pixel = source[pixelIndex];
            const auto blue = (pixel & 255U) * 55U / 100U;
            const auto green = ((pixel >> 8U) & 255U) * 55U / 100U;
            const auto red = ((pixel >> 16U) & 255U) * 55U / 100U;
            destination[pixelIndex] = 0xFF000000U | (red << 16U) | (green << 8U) | blue;
        }
        damage_.reserve(24);
        Create({L"VisPick.Spike.Overlay", CS_DBLCLKS, IDC_CROSS}, L"VisPick overlay rendering benchmark", WS_POPUP,
               WS_EX_TOOLWINDOW | WS_EX_TOPMOST, bounds, WindowActivation::Passive);
        DisableWindowAnimations(Handle());
        labelHeight_ = MulDiv(24, static_cast<int>(DpiForWindow(Handle())), 96);
        labelWidth_ = MulDiv(180, static_cast<int>(DpiForWindow(Handle())), 96);
        ShowWindow(Handle(), SW_SHOWNOACTIVATE);
        UpdateWindow(Handle());
        GdiFlush();
    }

    ~OverlayWindow() override { Destroy(); }

    void SetSelection(const RECT selection) {
        const RECT previous = selection_;
        selection_ = selection;
        damage_.clear();
        AddDifference(previous, selection_);
        AddDifference(selection_, previous);
        AddBorder(previous);
        AddBorder(selection_);
        AddDamage(LabelBounds(previous));
        AddDamage(LabelBounds(selection_));
        for (const RECT& dirty : damage_) { InvalidateRect(Handle(), &dirty, FALSE); }
        UpdateWindow(Handle());
    }

    [[nodiscard]] bool PaintSucceeded() const noexcept { return paintSucceeded_; }

protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message != WM_PAINT) { return Window::OnMessage(message, parameter, data); }
        spikes::PaintScope paint(Handle());
        try {
            if (damage_.empty()) { ComposeRegion(client_); }
            else { for (const RECT& dirty : damage_) { ComposeRegion(dirty); } }
            const RECT dirty = paint.Bounds();
            // BeginPaint already clips this destination DC to the exact update
            // region (not merely rcPaint's bounding box). The interior is not
            // copied for disjoint dirty strips; no duplicate GetUpdateRgn needed.
            if (!BitBlt(paint.Context(), dirty.left, dirty.top,
                        dirty.right - dirty.left, dirty.bottom - dirty.top,
                        backBuffer_.DeviceContext(), dirty.left, dirty.top, SRCCOPY)) {
                paintSucceeded_ = false;
            }
        } catch (...) {
            paintSucceeded_ = false;
        }
        return 0;
    }

private:
    void AddDamage(const RECT rectangle) {
        RECT clipped{};
        if (IntersectRect(&clipped, &rectangle, &client_)) { damage_.push_back(clipped); }
    }

    void AddDifference(const RECT source, const RECT removed) {
        RECT overlap{};
        if (!IntersectRect(&overlap, &source, &removed)) { AddDamage(source); return; }
        AddDamage({source.left, source.top, source.right, overlap.top});
        AddDamage({source.left, overlap.bottom, source.right, source.bottom});
        AddDamage({source.left, overlap.top, overlap.left, overlap.bottom});
        AddDamage({overlap.right, overlap.top, source.right, overlap.bottom});
    }

    void AddBorder(const RECT rectangle) {
        if (IsRectEmpty(&rectangle)) { return; }
        constexpr LONG thickness = 2;
        AddDamage({rectangle.left, rectangle.top, rectangle.right, rectangle.top + thickness});
        AddDamage({rectangle.left, rectangle.bottom - thickness, rectangle.right, rectangle.bottom});
        AddDamage({rectangle.left, rectangle.top, rectangle.left + thickness, rectangle.bottom});
        AddDamage({rectangle.right - thickness, rectangle.top, rectangle.right, rectangle.bottom});
    }

    [[nodiscard]] RECT LabelBounds(const RECT selection) const {
        if (IsRectEmpty(&selection)) { return {}; }
        const LONG labelTop = selection.top >= labelHeight_ ? selection.top - labelHeight_ : selection.bottom;
        return {selection.left, labelTop, selection.left + labelWidth_, labelTop + labelHeight_};
    }

    void ComposeRegion(const RECT dirty) {
        const HDC context = backBuffer_.DeviceContext();
        spikes::SavedDc saved(context);
        IntersectClipRect(context, dirty.left, dirty.top, dirty.right, dirty.bottom);
        if (!BitBlt(context, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                    masked_.DeviceContext(), dirty.left, dirty.top, SRCCOPY)) {
            throw std::runtime_error("Overlay mask blit failed");
        }
        RECT selectedDirty{};
        if (IntersectRect(&selectedDirty, &dirty, &selection_)) {
            frozen_->BlitTo(context, selectedDirty, {selectedDirty.left, selectedDirty.top});
        }
        if (IsRectEmpty(&selection_)) { return; }
        SetDCBrushColor(context, RGB(58, 122, 254));
        auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        FrameRect(context, &selection_, brush);
        RECT inner = selection_;
        InflateRect(&inner, -1, -1);
        FrameRect(context, &inner, brush);
        RECT label = LabelBounds(selection_);
        SetDCBrushColor(context, RGB(32, 32, 32));
        FillRect(context, &label, brush);
        SetTextColor(context, RGB(255, 255, 255));
        SetBkMode(context, TRANSPARENT);
        const std::wstring dimensions = std::to_wstring(selection_.right - selection_.left) +
            L" x " + std::to_wstring(selection_.bottom - selection_.top);
        DrawTextW(context, dimensions.c_str(), static_cast<int>(dimensions.size()), &label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    RECT client_;
    std::shared_ptr<const FrozenScreen> frozen_;
    DibSection masked_;
    DibSection backBuffer_;
    RECT selection_{};
    std::vector<RECT> damage_;
    int labelHeight_ = 24;
    int labelWidth_ = 180;
    bool paintSucceeded_ = true;
};

} // namespace

SpikeResult RunOverlaySpike(const SpikeOptions& options) {
    SpikeResult result{"overlay", false, {}, {}};
    const RECT desktop = VirtualDesktopBounds();
    const int width = desktop.right - desktop.left;
    const int height = desktop.bottom - desktop.top;
    if (options.iterations < 1) { throw std::invalid_argument("Overlay iterations must be positive"); }
    std::vector<double> paintTimes;
    std::vector<double> flushTimes;
    paintTimes.reserve(static_cast<std::size_t>(options.iterations));
    flushTimes.reserve(static_cast<std::size_t>(options.iterations));
    const auto environment = DetectTestEnvironment();
    const auto memoryBefore = spikes::ReadProcessMemory();
    std::uint64_t peakPrivateCommit = memoryBefore.privateCommitBytes;
    const auto initialization = spikes::Clock::now();
    auto overlay = std::make_unique<OverlayWindow>(desktop);
    const double initializationMilliseconds = spikes::ElapsedMilliseconds(initialization);
    const auto memoryInitialized = spikes::ReadProcessMemory();
    peakPrivateCommit = std::max(peakPrivateCommit, memoryInitialized.privateCommitBytes);
    PumpWindowMessages();
    DwmFlush();
    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        const bool largeSelection = iteration >= options.iterations / 2;
        const int selectionWidth = largeSelection ? width * 3 / 4 : std::min(640, width / 2);
        const int selectionHeight = largeSelection ? height * 3 / 4 : std::min(360, height / 2);
        const int horizontalRange = std::max(1, width - selectionWidth);
        const int verticalRange = std::max(1, height - selectionHeight);
        const int left = (iteration * 7) % horizontalRange;
        const int top = (iteration * 5) % verticalRange;
        const auto beginPaint = spikes::Clock::now();
        overlay->SetSelection({left, top, left + selectionWidth, top + selectionHeight});
        const double paintingMilliseconds = spikes::ElapsedMilliseconds(beginPaint);
        const auto beginFlush = spikes::Clock::now();
        GdiFlush();
        const double flushingMilliseconds = spikes::ElapsedMilliseconds(beginFlush);
        paintTimes.push_back(paintingMilliseconds + flushingMilliseconds);
        flushTimes.push_back(flushingMilliseconds);
        PumpWindowMessages();
        if (iteration % 100 == 0) {
            peakPrivateCommit = std::max(peakPrivateCommit, spikes::ReadProcessMemory().privateCommitBytes);
        }
    }
    const double average = std::accumulate(paintTimes.begin(), paintTimes.end(), 0.0) /
                           static_cast<double>(paintTimes.size());
    const double maximum = *std::max_element(paintTimes.begin(), paintTimes.end());
    const bool paintSucceeded = overlay->PaintSucceeded();
    overlay.reset(); // Release the FrozenScreen and both other full-size DIBs before sampling.
    PumpWindowMessages();
    GdiFlush();
    const auto memoryAfter = spikes::ReadProcessMemory();
    const auto afterDelta = static_cast<std::int64_t>(memoryAfter.privateCommitBytes) -
                            static_cast<std::int64_t>(memoryBefore.privateCommitBytes);
    const auto bufferBytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4U;
    constexpr std::uint64_t memoryToleranceBytes = 5U * 1024U * 1024U;
    const bool memoryStable = std::abs(afterDelta) <= static_cast<std::int64_t>(memoryToleranceBytes) &&
        peakPrivateCommit - memoryBefore.privateCommitBytes <= 3U * bufferBytes + memoryToleranceBytes;
    result.passed = paintSucceeded && average <= 5.0 && maximum <= 16.0 && memoryStable;
    result.measurements = {
        {"canvas_width", static_cast<double>(width), "px"},
        {"canvas_height", static_cast<double>(height), "px"},
        {"iterations", static_cast<double>(options.iterations), "frames"},
        {"initialization", initializationMilliseconds, "ms"},
        {"paint_and_gdi_flush_average", average, "ms"},
        {"paint_and_gdi_flush_p95", spikes::Percentile(paintTimes, 0.95), "ms"},
        {"paint_and_gdi_flush_max", maximum, "ms"},
        {"gdi_flush_average", std::accumulate(flushTimes.begin(), flushTimes.end(), 0.0) /
            static_cast<double>(flushTimes.size()), "ms"},
        {"full_screen_buffer_count", 3, "buffers"},
        {"full_screen_buffer_bytes", static_cast<double>(bufferBytes), "bytes"},
        {"owned_image_bytes", static_cast<double>(3U * bufferBytes), "bytes"},
        {"private_commit_before", static_cast<double>(memoryBefore.privateCommitBytes), "bytes"},
        {"private_commit_observed_peak", static_cast<double>(peakPrivateCommit), "bytes"},
        {"private_commit_after_close", static_cast<double>(memoryAfter.privateCommitBytes), "bytes"},
        {"private_commit_after_close_delta", static_cast<double>(afterDelta), "bytes"},
        {"working_set_before", static_cast<double>(memoryBefore.workingSetBytes), "bytes"},
        {"working_set_after_close", static_cast<double>(memoryAfter.workingSetBytes), "bytes"},
        {"memory_stable", memoryStable ? 1.0 : 0.0, "boolean"},
        {"physical_acceptance_eligible", environment.physicalAcceptanceEligible ? 1.0 : 0.0, "boolean"}
    };
    result.notes.emplace_back("Actual virtual-desktop window; first half moves a 640x360 selection, second half moves a selection spanning 75% of each canvas dimension. Symmetric-difference strips, borders and labels are redrawn.");
    result.notes.emplace_back("Timers include synchronous WM_PAINT and GdiFlush; exclude DWM presentation/vsync. Frozen desktop pixels are never written to disk.");
    result.notes.emplace_back("Exactly three owned full-screen images coexist: immutable shared FrozenScreen, precomputed mask, and back buffer. Capture writes directly into the retained frozen DIB. Private commit is sampled after initialization and every 100 frames; observed peak is not the OS lifetime peak. Close releases all three before the final memory sample; a 5 MiB allowance covers window/allocator bookkeeping.");
    result.notes.emplace_back("BeginPaint already clips the destination to the precise update region; rcPaint is only its bounding rectangle. Back-buffer composition separately clips each dirty strip.");
    result.notes.insert(result.notes.end(), environment.invalidReasons.begin(), environment.invalidReasons.end());
    if (width != 7680 || height != 2160) {
        result.notes.emplace_back("This machine's canvas differs from AC0-5's 7680x2160 target; this run is not evidence for that exact configuration.");
    }
    return result;
}

} // namespace vispick
