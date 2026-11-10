#include "spikes/Spikes.h"

#include "capture/ScreenGrab.h"
#include "platform/Compat.h"
#include "spikes/TestEnvironment.h"
#include "spikes/VisualSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace vispick {
namespace {

using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct Timings {
    double median = 0;
    double percentile95 = 0;
    double maximum = 0;
};

struct CaptureSamples {
    Timings complete;
    Timings bitBlt;
    Timings flush;
};

Timings Summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[(samples.size() * 95 - 1) / 100], samples.back()};
}

CaptureSamples MeasureCapture(CaptureSession& session, bool includeLayeredWindows, int sampleCount) {
    session.Capture(includeLayeredWindows);
    std::vector<double> complete;
    std::vector<double> blit;
    std::vector<double> flush;
    complete.reserve(static_cast<std::size_t>(sampleCount));
    blit.reserve(static_cast<std::size_t>(sampleCount));
    flush.reserve(static_cast<std::size_t>(sampleCount));
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto timing = session.CaptureTimed(includeLayeredWindows);
        complete.push_back(timing.bitBltMilliseconds + timing.gdiFlushMilliseconds);
        blit.push_back(timing.bitBltMilliseconds);
        flush.push_back(timing.gdiFlushMilliseconds);
    }
    return {Summarize(std::move(complete)), Summarize(std::move(blit)), Summarize(std::move(flush))};
}

void AddTimings(SpikeResult& result, const std::string& prefix, const Timings& timings) {
    result.measurements.push_back({prefix + ".median", timings.median, "ms"});
    result.measurements.push_back({prefix + ".p95", timings.percentile95, "ms"});
    result.measurements.push_back({prefix + ".max", timings.maximum, "ms"});
}

void AddCaptureTimings(SpikeResult& result, const std::string& prefix, const CaptureSamples& samples) {
    AddTimings(result, prefix, samples.complete);
    AddTimings(result, prefix + ".bitblt", samples.bitBlt);
    AddTimings(result, prefix + ".gdi_flush", samples.flush);
}

} // namespace

SpikeResult RunCaptureSpike(const SpikeOptions&) {
    InitializeDpiAwareness();
    SpikeResult result;
    result.name = "capture";
    const RECT desktop = VirtualDesktopBounds();
    const auto environment = DetectTestEnvironment();
    const auto& monitors = environment.monitors;
    const auto version = GetWindowsVersion();
    result.measurements = {
        {"desktop.left", static_cast<double>(desktop.left), "px"},
        {"desktop.top", static_cast<double>(desktop.top), "px"},
        {"desktop.width", static_cast<double>(desktop.right - desktop.left), "px"},
        {"desktop.height", static_cast<double>(desktop.bottom - desktop.top), "px"},
        {"monitor_count", static_cast<double>(monitors.size()), "count"},
        {"windows.build", static_cast<double>(version.build), "build"},
        {"physical_acceptance_eligible", environment.physicalAcceptanceEligible ? 1.0 : 0.0, "boolean"}};
    double coldCaptureMilliseconds = 0;
    CaptureSamples desktopTimings;
    {
        const auto coldBegin = Clock::now();
        CaptureSession capture(desktop);
        capture.Capture(true);
        coldCaptureMilliseconds = Milliseconds(coldBegin, Clock::now());
        desktopTimings = MeasureCapture(capture, true, 20);
    }
    result.measurements.push_back({"desktop.cold_allocate_and_capture", coldCaptureMilliseconds, "ms"});
    AddCaptureTimings(result, "desktop.captureblt", desktopTimings);

    RECT monitorBounds = desktop;
    for (const auto& monitor : monitors) {
        if (monitor.primary) {
            monitorBounds = monitor.bounds;
            break;
        }
    }
    const LONG regionWidth = std::min<LONG>(1920, monitorBounds.right - monitorBounds.left);
    const LONG regionHeight = std::min<LONG>(1080, monitorBounds.bottom - monitorBounds.top);
    const RECT recordingRegion{monitorBounds.left, monitorBounds.top, monitorBounds.left + regionWidth,
                               monitorBounds.top + regionHeight};
    CaptureSamples withoutLayered;
    CaptureSamples withLayered;
    {
        CaptureSession regionCapture(recordingRegion);
        withoutLayered = MeasureCapture(regionCapture, false, 60);
        withLayered = MeasureCapture(regionCapture, true, 60);
    }
    AddCaptureTimings(result, "region.srccopy", withoutLayered);
    AddCaptureTimings(result, "region.captureblt", withLayered);
    result.measurements.push_back({"region.width", static_cast<double>(regionWidth), "px"});
    result.measurements.push_back({"region.height", static_cast<double>(regionHeight), "px"});

    const RECT smallRegion{monitorBounds.left, monitorBounds.top,
                           monitorBounds.left + std::min<LONG>(320, monitorBounds.right - monitorBounds.left),
                           monitorBounds.top + std::min<LONG>(240, monitorBounds.bottom - monitorBounds.top)};
    CaptureSession smallCapture(smallRegion);
    AddCaptureTimings(result, "small.srccopy", MeasureCapture(smallCapture, false, 60));
    const auto* storageBefore = smallCapture.Image().Pixels().data();
    const auto memoryBefore = spikes::ReadProcessMemory();
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto loopStart = Clock::now();
    for (int iteration = 0; iteration < 1000; ++iteration) {
        // Intentionally no DwmFlush, pattern generation, pixel scan, allocation,
        // per-iteration timing, or encoding in the small-area microbenchmark.
        smallCapture.Capture(false);
    }
    const double loopMilliseconds = Milliseconds(loopStart, Clock::now());
    const auto memoryAfter = spikes::ReadProcessMemory();
    const auto privateDelta = static_cast<std::int64_t>(memoryAfter.privateCommitBytes) -
                              static_cast<std::int64_t>(memoryBefore.privateCommitBytes);
    const int gdiDelta =
        static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - static_cast<int>(gdiBefore);
    const bool sameStorage = storageBefore == smallCapture.Image().Pixels().data();
    const bool memoryStable = sameStorage && std::abs(privateDelta) <= 1024 * 1024 && std::abs(gdiDelta) <= 5;
    result.measurements.push_back({"small.width", static_cast<double>(smallRegion.right - smallRegion.left), "px"});
    result.measurements.push_back({"small.height", static_cast<double>(smallRegion.bottom - smallRegion.top), "px"});
    result.measurements.push_back({"small.repeated_captures", 1000, "count"});
    result.measurements.push_back({"small.loop_mean", loopMilliseconds / 1000.0, "ms"});
    result.measurements.push_back({"small.same_storage", sameStorage ? 1.0 : 0.0, "boolean"});
    result.measurements.push_back(
        {"small.private_commit_before", static_cast<double>(memoryBefore.privateCommitBytes), "bytes"});
    result.measurements.push_back(
        {"small.private_commit_after", static_cast<double>(memoryAfter.privateCommitBytes), "bytes"});
    result.measurements.push_back({"small.private_commit_delta", static_cast<double>(privateDelta), "bytes"});
    result.measurements.push_back({"small.gdi_object_delta", static_cast<double>(gdiDelta), "objects"});
    result.measurements.push_back({"small.memory_stable", memoryStable ? 1.0 : 0.0, "boolean"});
    result.measurements.push_back({"samples.desktop", 20, "count"});
    result.measurements.push_back({"samples.region_per_mode", 60, "count"});
    result.passed =
        desktopTimings.complete.percentile95 <= 150.0 && regionWidth == 1920 && regionHeight == 1080 && memoryStable;
    result.notes.emplace_back("Capture writes into one reusable DIB. Timings separate BitBlt and required GdiFlush; "
                              "there is no full-frame alpha fixup or Bitmap copy. The borrowed view is valid until the "
                              "next capture/destruction; output alpha is normalized only after cropping.");
    result.notes.emplace_back(
        "The 320x240 microbenchmark performs 1000 same-buffer BitBlt+GdiFlush calls with no DwmFlush or pixel work. "
        "Private commit is measured after warmup before/after the loop; the stability gate allows 1 MiB for system "
        "bookkeeping and reports the exact delta. No desktop pixels are saved.");
    result.notes.emplace_back("Pass is conditional on a verified local physical display environment; an invalid "
                              "environment keeps diagnostic timings but cannot decide AC0-7 or pass acceptance.");
    result.notes.insert(result.notes.end(), environment.invalidReasons.begin(), environment.invalidReasons.end());
    if (regionWidth != 1920 || regionHeight != 1080) {
        result.notes.emplace_back(
            "Primary monitor is smaller than 1920x1080: AC0-7 remains unverified at its required resolution.");
    } else if (!environment.physicalAcceptanceEligible) {
        result.notes.emplace_back(
            "DXGI decision deferred: rerun on verified physical displays at >=50 Hz. Similar small/large-region "
            "latency here may be synchronization overhead; no bandwidth conclusion is justified.");
    } else if (withLayered.complete.percentile95 > 15.0) {
        result.notes.emplace_back(
            "Valid physical environment: 1080p CAPTUREBLT p95 exceeds 15 ms; investigate a Windows 8+ DXGI prototype "
            "while retaining the Win7 BitBlt fallback. Validate the alternate backend before choosing it.");
    } else {
        result.notes.emplace_back("Valid physical environment: 1080p CAPTUREBLT p95 <=15 ms in this run. Keep BitBlt "
                                  "as the initial backend pending animated-content recording tests.");
    }
    if (desktop.right - desktop.left != 7680 || desktop.bottom - desktop.top != 2160) {
        result.notes.emplace_back(
            "Actual desktop size differs from AC0-4's 7680x2160 target; that target configuration remains unverified.");
    }
    return result;
}

} // namespace vispick
