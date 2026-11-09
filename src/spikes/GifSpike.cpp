#include "spikes/Spikes.h"
#include "capture/ScreenGrab.h"
#include "gif/GifWriter.h"
#include "gif/Timing.h"
#include "spikes/PatternProcess.h"
#include "imaging/Bitmap.h"
#include "platform/Compat.h"
#include "platform/Window.h"

#include <Windows.h>
#include <dwmapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace vispick {
namespace {

using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

class PaintScope {
public:
    explicit PaintScope(HWND window) : window_(window), context_(BeginPaint(window, &paint_)) {}
    ~PaintScope() { EndPaint(window_, &paint_); }
    PaintScope(const PaintScope&) = delete;
    PaintScope& operator=(const PaintScope&) = delete;
    [[nodiscard]] HDC Context() const noexcept { return context_; }
private:
    HWND window_;
    PAINTSTRUCT paint_{};
    HDC context_;
};

class TemporaryArtifact {
public:
    explicit TemporaryArtifact(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryArtifact() {
        if (!keep_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    TemporaryArtifact(const TemporaryArtifact&) = delete;
    TemporaryArtifact& operator=(const TemporaryArtifact&) = delete;
    void Keep() noexcept { keep_ = true; }
private:
    std::filesystem::path path_;
    bool keep_ = false;
};

void FillPattern(Bitmap& image, int frameNumber) {
    constexpr std::array<std::uint32_t, 8> colors{
        0xFF172333U, 0xFFEEF2F6U, 0xFF3689E5U, 0xFF34B788U,
        0xFFF0BB45U, 0xFFD85C62U, 0xFF946EE0U, 0xFF54BBC8U};
    const int squareLeft = (frameNumber * 7) % (image.Width() - 100);
    const int squareTop = 180 + (frameNumber * 3) % 400;
    for (int row = 0; row < image.Height(); ++row) {
        for (int column = 0; column < image.Width(); ++column) {
            auto paletteIndex = static_cast<std::size_t>(((column / 160) + (row / 90)) % 8);
            if (row < 100) {
                const int bit = column / 80;
                paletteIndex = ((frameNumber >> bit) & 1) != 0 ? 1 : 0;
            }
            if (column >= squareLeft && column < squareLeft + 100 &&
                row >= squareTop && row < squareTop + 100) {
                paletteIndex = 1;
            }
            image.Pixels()[static_cast<std::size_t>(row) *
                static_cast<std::size_t>(image.Width()) + static_cast<std::size_t>(column)] =
                colors[paletteIndex];
        }
    }
}

bool SameRgb(std::uint32_t captured, std::uint32_t expected) noexcept {
    return (captured & 0x00FFFFFFU) == (expected & 0x00FFFFFFU);
}

class AnimationWindow final : public Window {
public:
    AnimationWindow() : image_(1280, 720) { UpdatePattern(0); }
    ~AnimationWindow() override { Destroy(); }

    void UpdatePattern(int frameNumber) {
        FillPattern(image_, frameNumber);
        if (Handle() != nullptr) {
            InvalidateRect(Handle(), nullptr, FALSE);
            UpdateWindow(Handle());
        }
    }

    [[nodiscard]] const Bitmap& Image() const noexcept { return image_; }
    [[nodiscard]] bool PaintedSuccessfully() const noexcept { return paintedSuccessfully_; }

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_PAINT) {
            PaintScope paint(Handle());
            BITMAPINFO information{};
            information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            information.bmiHeader.biWidth = image_.Width();
            information.bmiHeader.biHeight = -image_.Height();
            information.bmiHeader.biPlanes = 1;
            information.bmiHeader.biBitCount = 32;
            information.bmiHeader.biCompression = BI_RGB;
            const int scanlines = StretchDIBits(paint.Context(), 0, 0,
                image_.Width(), image_.Height(), 0, 0, image_.Width(), image_.Height(),
                image_.Pixels().data(), &information, DIB_RGB_COLORS, SRCCOPY);
            paintedSuccessfully_ = scanlines == image_.Height();
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }

private:
    Bitmap image_;
    bool paintedSuccessfully_ = false;
};

void AddTiming(SpikeResult& result, const char* name, std::vector<double> samples) {
    if (samples.empty()) {
        return;
    }
    std::sort(samples.begin(), samples.end());
    const std::string prefix(name);
    result.measurements.push_back({prefix + "_mean", std::accumulate(samples.begin(),
        samples.end(), 0.0) / static_cast<double>(samples.size()), "ms"});
    result.measurements.push_back({prefix + "_p95", samples[static_cast<std::size_t>(
        0.95 * static_cast<double>(samples.size() - 1))], "ms"});
    result.measurements.push_back({prefix + "_max", samples.back(), "ms"});
}

void DescribeHitWindow(SpikeResult& result, HWND hitWindow) {
    // Class/geometry are enough to identify overlay types; never read titles,
    // process command lines, or the unexpected pixels themselves.
    std::array<wchar_t, 256> className{};
    const int characterCount = hitWindow != nullptr
        ? GetClassNameW(hitWindow, className.data(), static_cast<int>(className.size())) : 0;
    if (characterCount > 0) {
        std::array<char, 768> utf8Name{};
        const int byteCount = WideCharToMultiByte(CP_UTF8, 0, className.data(), characterCount,
            utf8Name.data(), static_cast<int>(utf8Name.size()), nullptr, nullptr);
        if (byteCount > 0) {
            result.notes.push_back("Hit window class at first mismatch: " +
                std::string(utf8Name.data(), static_cast<std::size_t>(byteCount)));
        } else {
            result.notes.push_back("Hit window class at first mismatch: unavailable (UTF-8 conversion failed).");
        }
    } else {
        result.notes.push_back("Hit window class at first mismatch: unavailable.");
    }
    RECT hitBounds{};
    const bool boundsAvailable = hitWindow != nullptr && GetWindowRect(hitWindow, &hitBounds) != FALSE;
    result.measurements.push_back({"hit_window_bounds_available", boundsAvailable ? 1.0 : 0.0, "boolean"});
    if (boundsAvailable) {
        result.measurements.push_back({"hit_window_left", static_cast<double>(hitBounds.left), "px"});
        result.measurements.push_back({"hit_window_top", static_cast<double>(hitBounds.top), "px"});
        result.measurements.push_back({"hit_window_right", static_cast<double>(hitBounds.right), "px"});
        result.measurements.push_back({"hit_window_bottom", static_cast<double>(hitBounds.bottom), "px"});
    }
}

} // namespace

SpikeResult RunGifSpike(const SpikeOptions& options) {
    if (options.seconds < 1 || options.seconds > 60) {
        throw std::invalid_argument("GIF spike duration must be 1..60 seconds");
    }
    SpikeResult result{"gif-correctness", false, {}, {}};
    const auto monitors = EnumerateMonitors();
    const auto chosenMonitor = std::find_if(monitors.begin(), monitors.end(),
        [](const MonitorInfo& monitor) {
            return monitor.bounds.right - monitor.bounds.left >= 1280 &&
                   monitor.bounds.bottom - monitor.bounds.top >= 720;
        });
    if (chosenMonitor == monitors.end()) {
        result.notes.push_back("No monitor can display the required 1280x720 physical-pixel pattern.");
        return result;
    }
    const auto& monitorBounds = chosenMonitor->bounds;
    const LONG left = monitorBounds.left + (monitorBounds.right - monitorBounds.left - 1280) / 2;
    const LONG top = monitorBounds.top + (monitorBounds.bottom - monitorBounds.top - 720) / 2;
    const RECT captureBounds{left, top, left + 1280, top + 720};
    AnimationWindow pattern;
    pattern.Create({L"VisPick.Spike.GifCorrectness", 0, IDC_ARROW}, L"VisPick GIF capture test pattern", WS_POPUP,
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, captureBounds, WindowActivation::Passive);
    DisableWindowAnimations(pattern.Handle());
    ShowWindow(pattern.Handle(), SW_SHOWNOACTIVATE);
    UpdateWindow(pattern.Handle());
    MessageTimer timer;
    timer.WaitUntil(Clock::now() + std::chrono::milliseconds(150));
    CaptureSession capture(captureBounds);
    const auto outputPath = options.outputDirectory / L"gif-spike.gif";
    TemporaryArtifact artifact(outputPath);
    gif::GifWriter writer(outputPath, 1280, 720);
    std::vector<double> captureSamples;
    std::vector<double> encodeSamples;
    std::vector<double> frameSamples;
    std::vector<double> renderSamples;
    std::vector<double> presentSamples;
    std::vector<double> verificationSamples;
    std::vector<double> writeSamples;
    std::vector<double> acquisitionAndEncodeSamples;
    std::vector<double> latenessSamples;
    std::vector<std::uint64_t> capturedHashes;
    std::optional<gif::EncodedFrame> pendingFrame;
    std::optional<gif::TimestampDelayAccumulator> delays;
    std::int64_t firstCaptureNanoseconds = 0;
    gif::FrameEncoder encoder;
    int pendingTick = 0;
    int nextTick = 0;
    int skippedTicks = 0;
    std::size_t maximumCompressedBytes = 0;
    constexpr int framesPerSecond = 30;
    const int plannedFrames = framesPerSecond * options.seconds;
    const auto start = Clock::now();
    while (nextTick < plannedFrames) {
        const auto deadline = start + std::chrono::nanoseconds(
            static_cast<std::int64_t>(nextTick) * 1000000000LL / framesPerSecond);
        timer.WaitUntil(deadline);
        const auto frameStart = Clock::now();
        latenessSamples.push_back(Milliseconds(frameStart - deadline));
        if (pattern.Handle() == nullptr) {
            throw std::runtime_error("GIF test pattern window was closed during recording");
        }
        // The test source must remain visible above other ordinary topmost
        // windows. Reassert its Z order without stealing keyboard activation;
        // this sets up the harness before sampling and is not a capture retry.
        if (!SetWindowPos(pattern.Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            throw std::runtime_error("Cannot bring GIF test pattern above other topmost windows");
        }
        pattern.UpdatePattern(nextTick);
        if (!pattern.PaintedSuccessfully()) {
            throw std::runtime_error("GIF test pattern painting failed");
        }
        const auto renderEnd = Clock::now();
        renderSamples.push_back(Milliseconds(renderEnd - frameStart));
        // Make the real desktop compositor display the newly painted pattern.
        // On Win7 with composition disabled this may return a failure; GdiFlush
        // still completes the direct GDI drawing before the capture.
        const BOOL gdiFlushSucceeded = GdiFlush();
        const HRESULT compositionFlushStatus = DwmFlush();
        const auto captureStart = Clock::now();
        presentSamples.push_back(Milliseconds(captureStart - renderEnd));
        capture.Capture(false);
        const auto captureEnd = Clock::now();
        const auto captureNanoseconds = QpcNanoseconds(QpcNow());
        if (!delays) { firstCaptureNanoseconds = captureNanoseconds; delays.emplace(captureNanoseconds); }
        captureSamples.push_back(Milliseconds(captureEnd - captureStart));
        const auto capturedHash = gif::PixelHash(capture.Image().Pixels());
        if (!std::equal(capture.Image().Pixels().begin(), capture.Image().Pixels().end(),
                        pattern.Image().Pixels().begin(), SameRgb)) {
            // Diagnose the failed sample only; do not capture again, relax pixel
            // equality, or persist any unexpected desktop image content.
            std::size_t mismatchCount = 0;
            std::size_t firstMismatch = 0;
            const auto capturedPixels = capture.Image().Pixels();
            const auto expectedPixels = pattern.Image().Pixels();
            for (std::size_t pixelIndex = 0; pixelIndex < capturedPixels.size(); ++pixelIndex) {
                if (!SameRgb(capturedPixels[pixelIndex], expectedPixels[pixelIndex])) {
                    if (mismatchCount == 0) {
                        firstMismatch = pixelIndex;
                    }
                    ++mismatchCount;
                }
            }
            const auto firstColumn = static_cast<LONG>(firstMismatch % 1280);
            const auto firstRow = static_cast<LONG>(firstMismatch / 1280);
            const POINT firstScreenPoint{captureBounds.left + firstColumn,
                                         captureBounds.top + firstRow};
            const HWND hitWindow = WindowFromPoint(firstScreenPoint);
            const bool patternAtMismatch = hitWindow == pattern.Handle() ||
                (hitWindow != nullptr && GetAncestor(hitWindow, GA_ROOT) == pattern.Handle());
            RECT actualBounds{};
            const bool boundsMatch = GetWindowRect(pattern.Handle(), &actualBounds) != FALSE &&
                EqualRect(&actualBounds, &captureBounds) != FALSE;
            bool previousFrameMatches = false;
            if (!capturedHashes.empty()) {
                Bitmap previousPattern(1280, 720);
                FillPattern(previousPattern, pendingTick);
                previousFrameMatches = std::equal(capturedPixels.begin(), capturedPixels.end(),
                    previousPattern.Pixels().begin(), SameRgb);
            }
            result.measurements.push_back({"pattern_mismatch_at_tick", static_cast<double>(nextTick), "tick"});
            result.measurements.push_back({"pattern_mismatched_pixels", static_cast<double>(mismatchCount), "pixels"});
            result.measurements.push_back({"first_mismatch_local_x", static_cast<double>(firstColumn), "px"});
            result.measurements.push_back({"first_mismatch_local_y", static_cast<double>(firstRow), "px"});
            result.measurements.push_back({"first_mismatch_screen_x", static_cast<double>(firstScreenPoint.x), "px"});
            result.measurements.push_back({"first_mismatch_screen_y", static_cast<double>(firstScreenPoint.y), "px"});
            result.measurements.push_back({"previous_frame_available", capturedHashes.empty() ? 0.0 : 1.0, "boolean"});
            result.measurements.push_back({"previous_frame_tick", capturedHashes.empty() ? -1.0 : static_cast<double>(pendingTick), "tick"});
            result.measurements.push_back({"matches_previous_frame_rgb", previousFrameMatches ? 1.0 : 0.0, "boolean"});
            result.measurements.push_back({"foreign_window_hit_at_first_mismatch", patternAtMismatch ? 0.0 : 1.0, "boolean"});
            result.measurements.push_back({"pattern_window_visible", IsWindowVisible(pattern.Handle()) != FALSE ? 1.0 : 0.0, "boolean"});
            result.measurements.push_back({"pattern_bounds_match_capture", boundsMatch ? 1.0 : 0.0, "boolean"});
            result.measurements.push_back({"gdi_flush_succeeded", gdiFlushSucceeded != FALSE ? 1.0 : 0.0, "boolean"});
            result.measurements.push_back({"dwm_flush_hresult", static_cast<double>(compositionFlushStatus), "HRESULT"});
            result.measurements.push_back({"verified_frames_before_failure", static_cast<double>(capturedHashes.size()), "frames"});
            DescribeHitWindow(result, hitWindow);
            AddTiming(result, "capture_before_failure", std::move(captureSamples));
            AddTiming(result, "pattern_render_before_failure", std::move(renderSamples));
            AddTiming(result, "pattern_present_wait_before_failure", std::move(presentSamples));
            result.notes.push_back("Captured desktop RGB differs from the current known pattern. "
                "The failed sample is not retried; the partial GIF is deleted. Diagnostics "
                "record only counts, coordinates, status flags, hit-window class/geometry and "
                "timings, never window titles or unexpected pixels.");
            result.notes.push_back("Previous-frame comparison regenerates the last verified pattern "
                "and compares every RGB pixel. WindowFromPoint is only a hit-test observation at "
                "the first mismatch; it cannot establish or exclude visual occlusion. "
                "No cause is inferred from these flags alone.");
            return result;
        }
        const auto verificationEnd = Clock::now();
        verificationSamples.push_back(Milliseconds(verificationEnd - captureEnd));
        if (pendingFrame.has_value()) {
            const auto delay = delays->AdvanceTo(captureNanoseconds);
            writer.WriteFrame(*pendingFrame, static_cast<std::uint16_t>(delay));
            pendingFrame.reset();
        }
        writeSamples.push_back(Milliseconds(Clock::now() - verificationEnd));
        capturedHashes.push_back(capturedHash);
        const auto encodeStart = Clock::now();
        pendingFrame = encoder.Encode(capture.Image().Pixels());
        const double encodeMilliseconds = Milliseconds(Clock::now() - encodeStart);
        encodeSamples.push_back(encodeMilliseconds);
        acquisitionAndEncodeSamples.push_back(captureSamples.back() + encodeMilliseconds);
        maximumCompressedBytes = std::max(maximumCompressedBytes, pendingFrame->compressedPixels.size());
        pendingTick = nextTick;
        const double elapsedSeconds = std::chrono::duration<double>(Clock::now() - start).count();
        const int earliestFutureTick = static_cast<int>(std::ceil(elapsedSeconds * framesPerSecond));
        const int newTick = std::max(nextTick + 1, earliestFutureTick);
        skippedTicks += std::max(0, std::min(newTick, plannedFrames) - nextTick - 1);
        nextTick = newTick;
        frameSamples.push_back(Milliseconds(Clock::now() - frameStart));
    }
    timer.WaitUntil(start + std::chrono::seconds(options.seconds));
    const auto recordingStopNanoseconds = QpcNanoseconds(QpcNow());
    const double wallSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    const double elapsedSeconds = static_cast<double>(recordingStopNanoseconds - firstCaptureNanoseconds) / 1000000000.0;
    if (pendingFrame.has_value()) {
        const auto finalDelay = delays->AdvanceTo(recordingStopNanoseconds);
        if (finalDelay != 0) { writer.WriteFrame(*pendingFrame, static_cast<std::uint16_t>(finalDelay)); }
        else {
            capturedHashes.pop_back();
            result.notes.emplace_back("The final sample had no representable 10ms display interval and was coalesced, without forcing a fabricated delay.");
        }
    }
    writer.Finish();
    pattern.Destroy();
    const auto inspection = gif::InspectGif(outputPath, capturedHashes);
    artifact.Keep();
    const double gifSeconds = static_cast<double>(inspection.durationCentiseconds) / 100;
    const double durationError = std::abs(gifSeconds - elapsedSeconds) / elapsedSeconds * 100;
    result.measurements.push_back({"width", 1280, "px"});
    result.measurements.push_back({"height", 720, "px"});
    result.measurements.push_back({"target_fps", framesPerSecond, "fps"});
    result.measurements.push_back({"planned_frames", static_cast<double>(plannedFrames), "frames"});
    result.measurements.push_back({"captured_frames", static_cast<double>(capturedHashes.size()), "frames"});
    result.measurements.push_back({"decoded_frames", static_cast<double>(inspection.frameCount), "frames"});
    result.measurements.push_back({"dropped_ticks", static_cast<double>(skippedTicks), "ticks"});
    result.measurements.push_back({"dropped_percent", 100.0 * skippedTicks / plannedFrames, "%"});
    result.measurements.push_back({"wall_duration", wallSeconds, "s"});
    result.measurements.push_back({"captured_timeline_duration", elapsedSeconds, "s"});
    result.measurements.push_back({"gif_duration", gifSeconds, "s"});
    result.measurements.push_back({"duration_error", durationError, "%"});
    result.measurements.push_back({"decoded_mismatched_frames", static_cast<double>(inspection.mismatchedFrames), "frames"});
    result.measurements.push_back({"file_size", static_cast<double>(std::filesystem::file_size(outputPath)), "bytes"});
    result.measurements.push_back({"queued_compressed_frames_max", 1, "frames"});
    result.measurements.push_back({"compressed_frame_max", static_cast<double>(maximumCompressedBytes), "bytes"});
    AddTiming(result, "capture", std::move(captureSamples));
    AddTiming(result, "encode", std::move(encodeSamples));
    AddTiming(result, "frame_work", std::move(frameSamples));
    AddTiming(result, "pattern_render", std::move(renderSamples));
    AddTiming(result, "pattern_present_wait", std::move(presentSamples));
    AddTiming(result, "pattern_pixel_verification", std::move(verificationSamples));
    AddTiming(result, "gif_write", std::move(writeSamples));
    AddTiming(result, "capture_plus_encode", std::move(acquisitionAndEncodeSamples));
    AddTiming(result, "scheduler_lateness", std::move(latenessSamples));
    result.notes.push_back("Real desktop BitBlt capture of a controlled 1280x720 animated pattern; "
        "SRCCOPY without CAPTUREBLT, full opaque GIF frames; delay accumulator uses actual capture-completion timestamps.");
    result.notes.push_back("Before each sample, the harness reasserts HWND_TOPMOST without activation "
        "to keep its test pattern above ordinary topmost windows. A failed RGB sample is never retried.");
    result.notes.push_back("Every captured frame matches the displayed source pattern; every decoded "
        "WIC frame is checked against its captured RGB hash. WIC validates decoding, not browser/Photos playback.");
    result.notes.push_back("Harness costs are reported separately: pattern generation/painting, "
        "GdiFlush+DwmFlush presentation wait, RGB verification, and file writes. The drop count "
        "measures this complete sequential experiment, not a production recorder. Use "
        "the separate --spike-gif-pipeline experiment for throughput; these sequential timings cannot identify a production bottleneck.");
    result.notes.push_back("P0 uses one UI/capture/encode thread and one pending compressed frame. "
        "P5 still needs parallel encoding, changed rectangles, transparency, cursor capture, "
        "backpressure, lock-screen pause, and preview UI. Wu fallback is unit-tested; this <=8-color workload exercises exact palettes.");
    result.notes.emplace_back("Correctness-only experiment: source presentation is synchronized deliberately, and skipped scheduling ticks do not fail it. GIF timeline begins at first completed capture and ends at recording stop; it does not fabricate exposure before the first image exists.");
    if (skippedTicks != 0) {
        result.notes.push_back("This sequential correctness check has no throughput acceptance gate. "
            "Use these timings to budget P5 worker parallelism; preserved GIF duration does not hide dropped ticks.");
    }
    result.passed = durationError < 1.0 &&
        inspection.mismatchedFrames == 0 && inspection.frameCount == capturedHashes.size();
    return result;
}

} // namespace vispick
