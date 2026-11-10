// Developer-only real screen-capture fixture. It renders only a known pattern,
// never uses SendInput, and operates only on windows created by this process.
#include "recording/Recorder.h"
#include "recording/GifPreview.h"
#include "recording/FrameChange.h"
#include "gif/GifReader.h"
#include "gif/GifWriter.h"
#include "imaging/Bitmap.h"
#include "imaging/ImageCodec.h"
#include "platform/Gdi.h"
#include "platform/Window.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "spikes/TestEnvironment.h"

#include <dwmapi.h>
#include <psapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT RecordingDone = WM_APP + 20;
constexpr UINT PreviewClosed = WM_APP + 21;
int patternWidth = 400;
int patternHeight = 240;
unsigned recordingSeconds = 2;
struct VisibilityObservation {
    double milliseconds;
    std::wstring windowClass;
    RECT rectangle;
};
std::vector<VisibilityObservation> visibilityObservations;

class Owner final : public vispick::Window {
public:
    bool recordingDone = false;
    bool previewClosed = false;
    Owner() {
        Create({L"VisPick.RecordingTestOwner", 0, IDC_ARROW}, L"", WS_OVERLAPPED, WS_EX_TOOLWINDOW, {0, 0, 0, 0},
               vispick::WindowActivation::Passive);
    }
    ~Owner() override { Destroy(); }

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == RecordingDone) {
            recordingDone = true;
            return 0;
        }
        if (message == PreviewClosed) {
            previewClosed = true;
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
};

vispick::Bitmap Pattern(unsigned frame) {
    vispick::Bitmap image(patternWidth, patternHeight);
    constexpr std::array<std::uint32_t, 5> colors{0xFF193C5B, 0xFF6D486D, 0xFF63A375, 0xFFFFD166, 0xFFDFDFDF};
    for (int row = 0; row < patternHeight; ++row) {
        for (int column = 0; column < patternWidth; ++column) {
            image.Row(row)[column] = colors[static_cast<std::size_t>((column / 37 + row / 29) % 5)];
            if (row >= 65 && row < 95 && column >= 25 + static_cast<int>(frame) * 35 &&
                column < 55 + static_cast<int>(frame) * 35) {
                image.Row(row)[column] = 0xFFED2939;
            }
        }
    }
    return image;
}

class PatternWindow final : public vispick::Window {
public:
    RECT bounds{};
    PatternWindow() : image_(Pattern(0)) {
        const auto monitors = vispick::EnumerateMonitors();
        if (monitors.empty()) { throw std::runtime_error("No display for recording fixture"); }
        const auto fitting = std::find_if(monitors.begin(), monitors.end(), [](const vispick::MonitorInfo& monitor) {
            return monitor.bounds.right - monitor.bounds.left >= patternWidth &&
                   monitor.bounds.bottom - monitor.bounds.top >= patternHeight;
        });
        if (fitting == monitors.end()) {
            throw std::runtime_error(
                "No monitor fits the requested recording dimensions; fixture was not silently resized");
        }
        const auto work = fitting->bounds;
        // Keep the fixture away from the lower-right system notification area
        // where the monitor has spare space; dimensions remain exact.
        const LONG left = work.left;
        const LONG top = work.top;
        bounds = {left, top, left + patternWidth, top + patternHeight};
        Create({L"VisPick.RecordingPattern", 0, IDC_ARROW}, L"VisPick known recording pattern", WS_POPUP,
               WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, bounds, vispick::WindowActivation::Passive);
        SetWindowPos(Handle(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(Handle(), SW_SHOWNOACTIVATE);
        UpdateWindow(Handle());
        static_cast<void>(DwmFlush());
        if (!SetTimer(Handle(), 1, 80, nullptr)) { throw std::runtime_error("Cannot animate known pattern"); }
    }
    ~PatternWindow() override { Destroy(); }

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_TIMER) {
            ObserveVisibility();
            frame_ = (frame_ + 1) % 8;
            image_ = Pattern(frame_);
            InvalidateRect(Handle(), nullptr, FALSE);
            UpdateWindow(Handle());
            return 0;
        }
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto context = BeginPaint(Handle(), &paint);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = patternWidth;
            bitmap.bmiHeader.biHeight = -patternHeight;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(context, 0, 0, patternWidth, patternHeight, 0, 0, patternWidth, patternHeight,
                          image_.Pixels().data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(Handle(), &paint);
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }

private:
    void ObserveVisibility() const {
        if (visibilityObservations.size() >= 32) { return; }
        const std::array<POINT, 5> points{
            {{bounds.left + 1, bounds.top + 1},
             {bounds.right - 2, bounds.top + 1},
             {bounds.left + 1, bounds.bottom - 2},
             {bounds.right - 2, bounds.bottom - 2},
             {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2}}};
        for (const auto& point : points) {
            const auto hit = WindowFromPoint(point);
            DWORD process = 0;
            GetWindowThreadProcessId(hit, &process);
            if (hit == nullptr || process == GetCurrentProcessId()) { continue; }
            std::array<wchar_t, 128> name{};
            RECT rectangle{};
            if (!GetClassNameW(hit, name.data(), static_cast<int>(name.size())) || !GetWindowRect(hit, &rectangle)) {
                continue;
            }
            const auto existing = std::find_if(visibilityObservations.begin(), visibilityObservations.end(),
                                               [&](const VisibilityObservation& observation) {
                                                   return observation.windowClass == name.data() &&
                                                          EqualRect(&observation.rectangle, &rectangle);
                                               });
            if (existing == visibilityObservations.end()) {
                visibilityObservations.push_back(
                    {std::chrono::duration<double, std::milli>(Clock::now() - created_).count(), name.data(),
                     rectangle});
            }
        }
    }
    vispick::Bitmap image_;
    Clock::time_point created_ = Clock::now();
    unsigned frame_ = 0;
};

void PumpFor(std::chrono::milliseconds duration) {
    const auto end = Clock::now() + duration;
    do {
        vispick::PumpWindowMessages();
        Sleep(1);
    } while (Clock::now() < end);
}

void WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(15),
               const std::function<void()>& sample = {}) {
    const auto end = Clock::now() + timeout;
    auto nextSample = Clock::now();
    while (!predicate()) {
        if (Clock::now() >= end) { throw std::runtime_error("Recording integration timed out"); }
        vispick::PumpWindowMessages();
        if (sample && Clock::now() >= nextSample) {
            sample();
            nextSample = Clock::now() + std::chrono::milliseconds(50);
        }
        Sleep(1);
    }
    if (sample) { sample(); }
}

struct MemorySamples {
    std::uint64_t maximumPrivate = 0;
    std::uint64_t peakWorkingSet = 0;
    std::uint64_t peakCommit = 0;
    void Sample() {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters))) {
            throw std::runtime_error("Cannot measure recording process memory");
        }
        maximumPrivate = std::max(maximumPrivate, static_cast<std::uint64_t>(counters.PrivateUsage));
        peakWorkingSet = std::max(peakWorkingSet, static_cast<std::uint64_t>(counters.PeakWorkingSetSize));
        peakCommit = std::max(peakCommit, static_cast<std::uint64_t>(counters.PeakPagefileUsage));
    }
};

struct Check {
    std::string name;
    bool passed;
};
struct Report {
    std::vector<Check> checks;
    std::vector<std::pair<std::string, double>> values;
    bool performanceEligible = false;
    bool performanceMeasured = false;
    bool performanceGatesPassed = false;
    std::string failure;
    std::filesystem::path output;
    void Require(bool value, std::string name) {
        checks.push_back({std::move(name), value});
        std::cout << (value ? "PASS " : "FAIL ") << checks.back().name << std::endl;
        if (!value) { throw std::runtime_error(checks.back().name); }
    }
};

void DescribeMismatch(Report& report, std::span<const std::uint32_t> pixels, const std::string& label) {
    std::size_t bestCount = pixels.size() + 1;
    unsigned bestPattern = 0;
    for (unsigned candidate = 0; candidate < 8; ++candidate) {
        const auto expected = Pattern(candidate);
        std::size_t count = 0;
        for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
            if (((pixels[pixel] ^ expected.Pixels()[pixel]) & 0xFFFFFFU) != 0) { ++count; }
        }
        if (count < bestCount) {
            bestCount = count;
            bestPattern = candidate;
        }
    }
    const auto expected = Pattern(bestPattern);
    vispick::Bitmap mask(patternWidth, patternHeight);
    RECT rectangle{patternWidth, patternHeight, 0, 0};
    bool first = true;
    for (int row = 0; row < patternHeight; ++row) {
        for (int column = 0; column < patternWidth; ++column) {
            const auto position = static_cast<std::size_t>(row) * patternWidth + column;
            const bool mismatch = ((pixels[position] ^ expected.Pixels()[position]) & 0xFFFFFFU) != 0;
            mask.Pixels()[position] = mismatch ? 0xFFFFFFFFU : 0xFF000000U;
            if (!mismatch) { continue; }
            rectangle.left = std::min(rectangle.left, static_cast<LONG>(column));
            rectangle.top = std::min(rectangle.top, static_cast<LONG>(row));
            rectangle.right = std::max(rectangle.right, static_cast<LONG>(column + 1));
            rectangle.bottom = std::max(rectangle.bottom, static_cast<LONG>(row + 1));
            if (first) {
                report.values.emplace_back(label + "_first_mismatch_x", column);
                report.values.emplace_back(label + "_first_mismatch_y", row);
                report.values.emplace_back(label + "_first_actual_rgb", pixels[position] & 0xFFFFFFU);
                report.values.emplace_back(label + "_first_expected_rgb", expected.Pixels()[position] & 0xFFFFFFU);
                first = false;
            }
        }
    }
    report.values.emplace_back(label + "_nearest_pattern", bestPattern);
    report.values.emplace_back(label + "_first_bad_frame_mismatched_pixels", static_cast<double>(bestCount));
    report.values.emplace_back(label + "_mismatch_left", rectangle.left);
    report.values.emplace_back(label + "_mismatch_top", rectangle.top);
    report.values.emplace_back(label + "_mismatch_right", rectangle.right);
    report.values.emplace_back(label + "_mismatch_bottom", rectangle.bottom);
    // Only a black/white location mask is exported; foreign desktop content is not copied.
    vispick::SaveImage(report.output / (label + "-first-mismatch-mask.png"), mask.View(), vispick::SaveFormat::Png);
}

void ValidateGif(Report& report, const vispick::RecordingResult& result, const std::string& label,
                 bool manualStop = false) {
    std::set<std::uint64_t> valid;
    for (unsigned frame = 0; frame < 8; ++frame) { valid.insert(vispick::gif::PixelHash(Pattern(frame).Pixels())); }
    vispick::gif::GifReader reader(result.path);
    report.Require(reader.Metadata().width == static_cast<unsigned>(patternWidth) &&
                       reader.Metadata().height == static_cast<unsigned>(patternHeight),
                   label + "_decoded_dimensions_match_requested");
    vispick::gif::GifCanvas canvas(reader.Metadata());
    std::uint64_t duration = 0;
    unsigned mismatched = 0;
    unsigned transparentSubframes = 0;
    bool compatibleFrameDelays = true;
    for (unsigned frameIndex = 0; frameIndex < reader.Metadata().frameCount; ++frameIndex) {
        const auto frame = reader.ReadFrame(frameIndex);
        canvas.Compose(frame);
        const bool mismatch = !valid.contains(vispick::gif::PixelHash(canvas.Pixels()));
        if (mismatch) {
            if (mismatched == 0) {
                report.values.emplace_back(label + "_first_bad_frame", frameIndex);
                report.values.emplace_back(label + "_first_bad_time_cs", static_cast<double>(duration));
                report.values.emplace_back(label + "_first_bad_subframe_left", frame.metadata.left);
                report.values.emplace_back(label + "_first_bad_subframe_top", frame.metadata.top);
                report.values.emplace_back(label + "_first_bad_subframe_width", frame.metadata.width);
                report.values.emplace_back(label + "_first_bad_subframe_height", frame.metadata.height);
                DescribeMismatch(report, canvas.Pixels(), label);
            }
            ++mismatched;
        }
        duration += frame.metadata.delayCentiseconds;
        compatibleFrameDelays = compatibleFrameDelays &&
                                frame.metadata.delayCentiseconds >= vispick::recording::MinimumFrameDelayCentiseconds;
        if (mismatch) {
            if (!report.values.empty() && report.values.back().first == label + "_last_bad_end_cs") {
                report.values.back().second = static_cast<double>(duration);
            } else {
                report.values.emplace_back(label + "_last_bad_end_cs", static_cast<double>(duration));
            }
        }
        if (frame.metadata.transparentColorIndex >= 0 && frame.metadata.width < static_cast<unsigned>(patternWidth)) {
            ++transparentSubframes;
        }
    }
    report.values.emplace_back(label + "_decoded_frames", reader.Metadata().frameCount);
    report.values.emplace_back(label + "_mismatched_frames", mismatched);
    report.values.emplace_back(label + "_transparent_subframes", transparentSubframes);
    report.values.emplace_back(label + "_duration_centiseconds", static_cast<double>(duration));
    report.values.emplace_back(label + "_first_completion_ms",
                               static_cast<double>(result.firstFrameCompletionNanoseconds) / 1000000.0);
    report.values.emplace_back(label + "_stop_ms", static_cast<double>(result.stopNanoseconds) / 1000000.0);
    report.values.emplace_back(label + "_captured_frames", static_cast<double>(result.capturedFrames));
    report.values.emplace_back(label + "_dropped_ticks", static_cast<double>(result.droppedTicks));
    report.values.emplace_back(label + "_planned_pipeline_bytes", static_cast<double>(result.peakPipelineBytes));
    report.values.emplace_back(label + "_peak_queued_frames", static_cast<double>(result.peakQueuedFrames));
    report.values.emplace_back(label + "_peak_inflight_slots", static_cast<double>(result.peakInFlightFrames));
    report.values.emplace_back(label + "_worker_count", result.workerCount);
    report.values.emplace_back(label + "_slot_count", result.slotCount);
    report.Require(mismatched == 0, label + "_wic_composed_pixels_match_known_pattern");
    report.Require(duration == result.durationCentiseconds && reader.Metadata().frameCount == result.frameCount,
                   label + "_file_metadata_matches_result");
    report.Require(compatibleFrameDelays, label + "_all_delays_at_least_two_centiseconds");
    const auto actual = result.stopNanoseconds - result.firstFrameCompletionNanoseconds;
    const auto encoded = static_cast<std::int64_t>(duration) * 10000000;
    const auto extension = manualStop ? vispick::recording::MinimumFrameDelayNanoseconds : 0;
    report.Require(actual - encoded < 10000000 && encoded - actual <= extension,
                   label + "_duration_matches_actual_completion_timestamps");
    report.Require(transparentSubframes > 0, label + "_changed_regions_use_transparency");
}

vispick::RecordingResult Finish(Owner& owner, std::unique_ptr<vispick::Recorder>& recorder,
                                std::chrono::milliseconds timeout = std::chrono::seconds(15),
                                const std::function<void()>& sample = {}) {
    WaitUntil([&] { return owner.recordingDone && recorder->Finished(); }, timeout, sample);
    auto result = recorder->TakeResult();
    if (!result) { throw std::runtime_error("Completed recorder returned no result"); }
    if (recorder->TakeResult()) { throw std::runtime_error("Recorder result was returned twice"); }
    recorder.reset();
    owner.recordingDone = false;
    return std::move(*result);
}

struct BarSearch {
    HWND owner;
    HWND found = nullptr;
};
BOOL CALLBACK FindBar(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<BarSearch*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || GetWindow(window, GW_OWNER) != search.owner) { return TRUE; }
    std::array<wchar_t, 128> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size())) != 0 &&
        std::wstring_view(name.data()) == L"VisPick.RecordBar") {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}
RECT OwnedClient(HWND window) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) {
        throw std::runtime_error("Cannot render a foreign window into the visual report");
    }
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        throw std::runtime_error("Invalid visual report client size");
    }
    return client;
}

void PrintOwnedClient(HWND window, vispick::DibSection& image) {
    std::fill(image.Pixels().begin(), image.Pixels().end(), 0xFF000000U);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(image.DeviceContext()), PRF_CLIENT);
    if (!GdiFlush()) { throw std::runtime_error("Cannot complete owned-window rendering"); }
    for (auto& pixel : image.Pixels()) { pixel |= 0xFF000000U; }
}

void SaveOwnedClient(HWND window, const std::filesystem::path& path) {
    const auto client = OwnedClient(window);
    vispick::DibSection image(client.right, client.bottom);
    PrintOwnedClient(window, image);
    vispick::SaveImage(path, vispick::BitmapView(image.Width(), image.Height(), image.Pixels()),
                       vispick::SaveFormat::Png);
}

void CheckPreviewDpi(Report& report, HWND window, const std::filesystem::path& output) {
    const auto originalDpi = vispick::DpiForWindow(window);
    const auto originalClient = OwnedClient(window);
    RECT original{};
    if (!GetWindowRect(window, &original)) { throw std::runtime_error("Cannot read owned preview bounds"); }
    MINMAXINFO minimum{};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
    const auto expected = vispick::AdjustWindowBoundsForDpi(
        {0, 0, MulDiv(360, static_cast<int>(originalDpi), 96), MulDiv(180, static_cast<int>(originalDpi), 96)},
        WS_OVERLAPPEDWINDOW, 0, originalDpi);
    report.Require(minimum.ptMinTrackSize.x >= expected.right - expected.left &&
                       minimum.ptMinTrackSize.y >= expected.bottom - expected.top,
                   "preview_minimum_client_size_scales_with_dpi");
    // A synthetic DPI message updates our renderer; Windows keeps the real monitor's nonclient metrics.
    constexpr int testDpi = 192;
    const auto scale = [](int logical) { return MulDiv(logical, testDpi, 96); };
    const LONG frameWidth = original.right - original.left - originalClient.right;
    const LONG frameHeight = original.bottom - original.top - originalClient.bottom;
    RECT suggested{original.left, original.top, original.left + scale(360) + frameWidth,
                   original.top + scale(180) + frameHeight};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(192, 192), reinterpret_cast<LPARAM>(&suggested));
    try {
        auto client = OwnedClient(window);
        if (client.right != scale(360) || client.bottom != scale(180)) {
            // Adjust from the actual post-change nonclient size, which can differ from the pre-change size.
            RECT resized{};
            if (!GetWindowRect(window, &resized) ||
                !SetWindowPos(window, nullptr, 0, 0, resized.right - resized.left + scale(360) - client.right,
                              resized.bottom - resized.top + scale(180) - client.bottom,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING)) {
                throw std::runtime_error("Cannot correct scaled preview client size");
            }
            client = OwnedClient(window);
        }
        report.values.emplace_back("preview_192dpi_client_width", client.right);
        report.values.emplace_back("preview_192dpi_client_height", client.bottom);
        RECT resized{};
        if (!GetWindowRect(window, &resized)) { throw std::runtime_error("Cannot read scaled preview bounds"); }
        MINMAXINFO scaledMinimum{};
        SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&scaledMinimum));
        // Allow only the discrepancy between the synthetic tracking minimum and the real nonclient frame.
        const LONG maximumWidth =
            std::max<LONG>(scale(360), scaledMinimum.ptMinTrackSize.x - (resized.right - resized.left - client.right));
        const LONG maximumHeight =
            std::max<LONG>(scale(180), scaledMinimum.ptMinTrackSize.y - (resized.bottom - resized.top - client.bottom));
        report.Require(client.right >= scale(360) && client.right <= maximumWidth && client.bottom >= scale(180) &&
                           client.bottom <= maximumHeight,
                       "preview_192dpi_minimum_client_size");
        vispick::DibSection image(client.right, client.bottom);
        PrintOwnedClient(window, image);
        const vispick::BitmapView rendered(image.Width(), image.Height(), image.Pixels());
        constexpr std::uint32_t copyFill = 0xFF215AADU;
        const RECT copy{scale(10), client.bottom - scale(36), scale(54), client.bottom - scale(6)};
        const auto buttonEdge = rendered.Row(copy.top + scale(3));
        const auto hasIcon = [&] {
            for (LONG row = copy.top + scale(5); row < copy.bottom - scale(5); ++row) {
                const auto pixels = rendered.Row(row);
                for (LONG column = copy.left + scale(5); column < copy.right - scale(5); ++column) {
                    const auto pixel = pixels[column];
                    if (((pixel >> 16) & 0xFF) >= 180 && ((pixel >> 8) & 0xFF) >= 180 && (pixel & 0xFF) >= 180) {
                        return true;
                    }
                }
            }
            return false;
        };
        report.Require(buttonEdge[copy.left + scale(3)] == copyFill && buttonEdge[copy.right - scale(3)] == copyFill &&
                           buttonEdge[copy.right + scale(3)] != copyFill && hasIcon(),
                       "preview_192dpi_clipboard_icon_visible_at_minimum_size");
        vispick::SaveImage(output / L"preview-dpi192-client.png", rendered, vispick::SaveFormat::Png);
    } catch (...) {
        SendMessageW(window, WM_DPICHANGED, MAKELONG(originalDpi, originalDpi), reinterpret_cast<LPARAM>(&original));
        throw;
    }
    SendMessageW(window, WM_DPICHANGED, MAKELONG(originalDpi, originalDpi), reinterpret_cast<LPARAM>(&original));
}

void CheckPreviewDirtyPaint(Report& report, HWND window) {
    const auto client = OwnedClient(window);
    const auto dpi = static_cast<int>(vispick::DpiForWindow(window));
    const LONG y = (client.bottom - MulDiv(100, dpi, 96)) / 2;
    const POINT damaged{client.right / 2 - 8, y};
    const POINT unchanged{client.right / 2 + 8, y};
    const auto isVisible = [window](POINT point) {
        return ClientToScreen(window, &point) && WindowFromPoint(point) == window;
    };
    if (!isVisible(damaged) || !isVisible(unchanged)) {
        std::cout << "SKIP preview_dirty_paint_preserves_unrelated_pixels (fixture is covered)" << std::endl;
        return;
    }
    UpdateWindow(window);
    const vispick::ScreenDc dc(window);
    const COLORREF expected = GetPixel(dc.Get(), damaged.x, damaged.y);
    constexpr auto sentinel = RGB(239, 79, 163);
    const RECT damage{damaged.x, damaged.y, damaged.x + 1, damaged.y + 1};
    const bool presented = expected != CLR_INVALID && expected != sentinel &&
                           SetPixelV(dc.Get(), damaged.x, damaged.y, sentinel) &&
                           SetPixelV(dc.Get(), unchanged.x, unchanged.y, sentinel) && GdiFlush() &&
                           InvalidateRect(window, &damage, FALSE) && UpdateWindow(window) && GdiFlush() &&
                           GetPixel(dc.Get(), damaged.x, damaged.y) == expected &&
                           GetPixel(dc.Get(), unchanged.x, unchanged.y) == sentinel;
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
    report.Require(presented, "preview_dirty_paint_preserves_unrelated_pixels");
}

struct PreviewFixture {
    HWND window;
    RECT client;
    int dpi;
    vispick::DibSection image;
    int left;
    int right;
    int trackY;

    explicit PreviewFixture(HWND handle)
        : window(handle), client(OwnedClient(window)), dpi(static_cast<int>(vispick::DpiForWindow(window))),
          image(client.right, client.bottom), left(Scale(12) + Scale(4)),
          right(client.right - Scale(12) - Scale(112) - 1),
          trackY(client.bottom - Scale(100) + (Scale(4) + Scale(30)) / 2) {}

    int Scale(int value) const { return MulDiv(value, dpi, 96); }
    LPARAM AtPercent(int percent) const { return MAKELPARAM(left + (right - left) * percent / 100, trackY); }
    void Click(LPARAM at) const {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at);
        SendMessageW(window, WM_LBUTTONUP, 0, at);
    }
    std::array<std::uint32_t, 2> FrameColors() {
        PrintOwnedClient(window, image);
        const auto row =
            vispick::BitmapView(image.Width(), image.Height(), image.Pixels()).Row((client.bottom - Scale(100)) / 2);
        return {row[client.right / 2 - Scale(16)], row[client.right / 2 + Scale(16)]};
    }
    std::uint32_t ProgressColor(int percent) {
        PrintOwnedClient(window, image);
        return vispick::BitmapView(image.Width(), image.Height(), image.Pixels())
            .Row(trackY)[left + (right - left) * percent / 100];
    }
};

void CheckPreviewTimeline(Report& report, Owner& owner, const std::filesystem::path& output) {
    constexpr std::uint32_t green = 0xFF63A375;
    constexpr std::uint32_t blue = 0xFF193C5B;
    constexpr std::uint32_t red = 0xFFED2939;
    constexpr std::uint32_t filledRail = 0xFF63A4FF;
    constexpr std::uint32_t emptyRail = 0xFF484C56;
    constexpr auto fixtureTimeout = std::chrono::seconds(5);
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L".gif";
    const auto variableFile = output / (L"preview-variable-" + suffix);
    const auto trimFile = output / (L"preview-trim-" + suffix);
    const auto trimmedFile = output / (L"preview-trimmed-" + suffix);
    const auto singleFile = output / (L"preview-single-" + suffix);
    const vispick::gif::IndexedImage base{{green}, {0, 0, 0, 0, 0, 0, 0, 0}};
    const auto fullFrame = vispick::gif::EncodeIndexedFrame(base);
    const auto noSave = [](HWND, int, int) -> std::optional<std::filesystem::path> { return std::nullopt; };
    {
        vispick::gif::GifWriter writer(variableFile, 4, 2);
        writer.WriteFrame(fullFrame, 60);
        const vispick::gif::IndexedImage transparent{{green, blue}, {1, 1, 0, 0, 1, 1, 0, 0}};
        writer.WriteFrame(vispick::gif::EncodeIndexedFrame(transparent),
                          {0, 0, 4, 2, 280, vispick::gif::DisposalMethod::Keep, 0});
        const vispick::gif::IndexedImage partial{{red}, {0, 0, 0, 0}};
        writer.WriteFrame(vispick::gif::EncodeIndexedFrame(partial), {2, 0, 2, 2, 60});
        writer.Finish();
    }
    std::filesystem::copy_file(variableFile, trimFile, std::filesystem::copy_options::overwrite_existing);
    {
        vispick::GifPreview preview({variableFile, 4, 2, 3, 400}, {}, owner.Handle(), PreviewClosed, noSave, true);
        preview.Show();
        const auto window = preview.Handle();
        SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
        PreviewFixture fixture(window);
        constexpr std::array initial{green, green};
        constexpr std::array middle{blue, green};
        constexpr std::array last{blue, red};
        WaitUntil([&] { return fixture.FrameColors() == initial; }, fixtureTimeout);
        CheckPreviewDirtyPaint(report, window);
        // 20% by duration is the second frame, unlike equal frame divisions.
        fixture.Click(fixture.AtPercent(20));
        WaitUntil([&] { return fixture.FrameColors() == middle; }, fixtureTimeout);
        report.Require(fixture.ProgressColor(10) == filledRail, "preview_click_uses_variable_delays_and_transparency");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, fixture.AtPercent(20));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, fixture.AtPercent(100));
        report.Require(GetCapture() == window && fixture.FrameColors() == middle &&
                           fixture.ProgressColor(80) == filledRail,
                       "preview_drag_updates_progress_before_seek");
        SendMessageW(window, WM_LBUTTONUP, 0, fixture.AtPercent(100));
        WaitUntil([&] { return fixture.FrameColors() == last; }, fixtureTimeout);
        report.Require(GetCapture() != window, "preview_drag_releases_capture_and_composes_target");
        SaveOwnedClient(window, output / L"preview-timeline-end.png");
        fixture.Click(fixture.AtPercent(0));
        WaitUntil([&] { return fixture.FrameColors() == initial; }, fixtureTimeout);
        PumpFor(std::chrono::milliseconds(650));
        report.Require(fixture.FrameColors() == initial && fixture.ProgressColor(10) == emptyRail,
                       "preview_rewind_preserves_paused_state");
        CheckPreviewDpi(report, window, output);
    }
    {
        unsigned saves = 0;
        vispick::GifPreview preview(
            {trimFile, 4, 2, 3, 400}, {}, owner.Handle(), PreviewClosed,
            [&](HWND, int, int) -> std::optional<std::filesystem::path> {
                if (++saves == 1) { return output / L"missing-trim-directory" / L"failed.gif"; }
                return trimmedFile;
            },
            true);
        preview.Show();
        const auto window = preview.Handle();
        SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
        PreviewFixture fixture(window);
        WaitUntil([&] { return fixture.FrameColors() == std::array{green, green}; }, fixtureTimeout);
        fixture.Click(fixture.AtPercent(20));
        WaitUntil([&] { return fixture.FrameColors() == std::array{blue, green}; }, fixtureTimeout);
        SendMessageW(window, WM_KEYDOWN, VK_OEM_4, 0);
        SendMessageW(window, WM_KEYDOWN, VK_OEM_6, 0);
        const auto savePoint =
            MAKELPARAM(fixture.client.right - fixture.Scale(145), fixture.client.bottom - fixture.Scale(20));
        SendMessageW(window, WM_LBUTTONUP, 0, savePoint);
        WaitUntil([&] { return saves == 1 && !preview.Busy(); }, fixtureTimeout);
        report.Require(!preview.Closed() && std::filesystem::exists(trimFile) &&
                           vispick::gif::InspectGif(trimFile).durationCentiseconds == 400,
                       "trim_save_failure_preserves_complete_original");
        SendMessageW(window, WM_LBUTTONUP, 0, savePoint);
        WaitUntil([&] { return saves == 2 && preview.Closed(); }, fixtureTimeout);
        const std::array pixels{blue, blue, green, green, blue, blue, green, green};
        const std::array hashes{vispick::gif::PixelHash(pixels)};
        const auto inspection = vispick::gif::InspectGif(trimmedFile, hashes);
        report.Require(inspection.frameCount == 1 && inspection.durationCentiseconds == 280 &&
                           inspection.mismatchedFrames == 0,
                       "trim_save_uses_frame_boundaries_variable_duration_and_composited_first_frame");
    }
    report.Require(!std::filesystem::exists(trimFile), "trim_success_closes_preview_and_releases_original_temporary");
    {
        vispick::gif::GifWriter writer(singleFile, 4, 2);
        writer.WriteFrame(fullFrame, static_cast<std::uint16_t>(vispick::Config::Gif::MaximumSeconds * 100));
        writer.Finish();
    }
    {
        vispick::GifPreview preview({singleFile, 4, 2, 1, vispick::Config::Gif::MaximumSeconds * 100}, {},
                                    owner.Handle(), PreviewClosed, noSave, true);
        preview.Show();
        const auto window = preview.Handle();
        SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
        PreviewFixture fixture(window);
        WaitUntil([&] { return fixture.FrameColors() == std::array{green, green}; }, fixtureTimeout);
        fixture.Click(MAKELPARAM(fixture.right - 1, fixture.trackY));
        WaitUntil([&] { return fixture.ProgressColor(90) == filledRail; }, fixtureTimeout);
        SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
        WaitUntil([&] { return fixture.ProgressColor(90) == emptyRail; }, fixtureTimeout);
        report.Require(fixture.FrameColors() == std::array{green, green},
                       "preview_five_minute_single_frame_seeks_and_loops");
    }
}

void ClickRecordingButton(Report& report, HWND owner, bool cancel, const std::filesystem::path& visualPath = {}) {
    BarSearch search{owner};
    EnumWindows(FindBar, reinterpret_cast<LPARAM>(&search));
    report.Require(search.found != nullptr && IsWindowVisible(search.found),
                   cancel ? "cancel_button_visible" : "stop_button_visible");
    if (!search.found) { throw std::runtime_error("Recording toolbar was not found"); }
    if (!visualPath.empty()) { SaveOwnedClient(search.found, visualPath); }
    RECT client{};
    GetClientRect(search.found, &client);
    const int dpi = static_cast<int>(vispick::DpiForWindow(search.found));
    const int margin = MulDiv(8, dpi, 96);
    const int buttonWidth = MulDiv(82, dpi, 96);
    const int x = client.right - margin - buttonWidth / 2 - (cancel ? 0 : buttonWidth + margin);
    const int y = client.bottom / 2;
    if (!PostMessageW(search.found, WM_LBUTTONUP, 0, MAKELPARAM(x, y))) {
        throw std::runtime_error("Cannot click owned recording button");
    }
}

HWND ClickRecordingPause(Report& report, HWND owner) {
    BarSearch search{owner};
    EnumWindows(FindBar, reinterpret_cast<LPARAM>(&search));
    report.Require(search.found != nullptr && IsWindowVisible(search.found), "pause_button_visible");
    if (!search.found) { throw std::runtime_error("Recording toolbar was not found"); }
    RECT client{};
    GetClientRect(search.found, &client);
    const int dpi = static_cast<int>(vispick::DpiForWindow(search.found));
    const int margin = MulDiv(8, dpi, 96);
    const int buttonWidth = MulDiv(82, dpi, 96);
    const int x = client.right - 3 * margin - 2 * buttonWidth - MulDiv(16, dpi, 96);
    SendMessageW(search.found, WM_LBUTTONUP, 0, MAKELPARAM(x, client.bottom / 2));
    return search.found;
}

std::size_t OwnTemporaryFiles() {
    const auto directory = std::filesystem::temp_directory_path() / L"VisPick";
    if (!std::filesystem::exists(directory)) { return 0; }
    const auto prefix = L"rec_vispick_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::size_t count = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().filename().wstring().starts_with(prefix) && file.path().extension() == L".gif") { ++count; }
    }
    return count;
}

constexpr std::array<std::uint32_t, 4> FullscreenColors{0xFF193C5B, 0xFFFFD166, 0xFF63A375, 0xFF6D486D};

class FullscreenPattern final : public vispick::Window {
public:
    explicit FullscreenPattern(RECT bounds) : bounds_(bounds) {
        Create({L"VisPick.FullscreenRecordingPattern", 0, IDC_ARROW}, L"VisPick fullscreen recording test pattern",
               WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, bounds, vispick::WindowActivation::Passive);
        if (!SetWindowPos(Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Cannot show the owned fullscreen pattern");
        }
        UpdateWindow(Handle());
        static_cast<void>(DwmFlush());
    }
    ~FullscreenPattern() override { Destroy(); }

    [[nodiscard]] bool CoversCapture() const noexcept {
        RECT actual{};
        if (!IsWindowVisible(Handle()) || !GetWindowRect(Handle(), &actual) || !EqualRect(&actual, &bounds_)) {
            return false;
        }
        // Only a fully covered display may be captured. Our own recorder
        // controls are allowed above the opaque pattern, foreign windows are not.
        unsigned inspected = 0;
        for (HWND above = GetWindow(Handle(), GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
            if (++inspected > 512) { return false; }
            if (!IsWindowVisible(above) || IsIconic(above)) { continue; }
            DWORD process = 0;
            GetWindowThreadProcessId(above, &process);
            if (process == GetCurrentProcessId()) { continue; }
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
                continue;
            }
            RECT foreign{}, intersection{};
            if (!GetWindowRect(above, &foreign) || IntersectRect(&intersection, &foreign, &bounds_)) { return false; }
        }
        return true;
    }

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message != WM_PAINT) { return Window::OnMessage(message, parameter, data); }
        PAINTSTRUCT paint{};
        const auto context = BeginPaint(Handle(), &paint);
        const LONG width = bounds_.right - bounds_.left;
        const LONG height = bounds_.bottom - bounds_.top;
        const std::array<RECT, 4> quadrants{{{0, 0, width / 2, height / 2},
                                             {width / 2, 0, width, height / 2},
                                             {0, height / 2, width / 2, height},
                                             {width / 2, height / 2, width, height}}};
        for (std::size_t quadrant = 0; quadrant < quadrants.size(); ++quadrant) {
            const auto color = FullscreenColors[quadrant];
            SetDCBrushColor(context, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
            FillRect(context, &quadrants[quadrant], static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        EndPaint(Handle(), &paint);
        return 0;
    }

private:
    RECT bounds_;
};

void CheckFullscreenRecording(Report& report, RECT bounds, const std::string& label) {
    Owner owner;
    FullscreenPattern pattern(bounds);
    const auto requireCoverage = [&] {
        if (!pattern.CoversCapture()) {
            throw std::runtime_error("Owned fullscreen test pattern is obscured; capture stopped");
        }
    };
    requireCoverage();
    const int sourceWidth = bounds.right - bounds.left;
    const int sourceHeight = bounds.bottom - bounds.top;
    vispick::Config::Gif options;
    options.maxSeconds = 2;
    options.showCursor = false;
    const auto plan = vispick::recording::PlanRecording(sourceWidth, sourceHeight, options.scale,
                                                        std::thread::hardware_concurrency());
    auto recorder = std::make_unique<vispick::Recorder>(bounds, options, owner.Handle(), RecordingDone, true);
    recorder->Start();
    auto result = Finish(owner, recorder, std::chrono::seconds(20), requireCoverage);
    // Keep recorded pixels out of the report, even when a validation fails.
    struct RemoveFixture {
        const std::filesystem::path& path;
        ~RemoveFixture() {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } removeFixture{result.path};
    report.values.emplace_back(label + "_source_width", sourceWidth);
    report.values.emplace_back(label + "_source_height", sourceHeight);
    report.values.emplace_back(label + "_encoded_width", result.width);
    report.values.emplace_back(label + "_encoded_height", result.height);
    report.values.emplace_back(label + "_planned_pipeline_bytes", static_cast<double>(result.peakPipelineBytes));
    report.values.emplace_back(label + "_captured_frames", static_cast<double>(result.capturedFrames));
    report.values.emplace_back(label + "_dropped_ticks", static_cast<double>(result.droppedTicks));
    report.values.emplace_back(label + "_duration_centiseconds", static_cast<double>(result.durationCentiseconds));
    if (!result.error.empty()) { throw std::runtime_error(vispick::WideToUtf8(result.error)); }
    report.Require(!result.cancelled && !result.path.empty() && result.capturedFrames > 0 &&
                       !result.captureInterrupted && result.captureFailures == 0,
                   label + "_captures_without_errors");
    report.Require(result.width == plan.width && result.height == plan.height &&
                       result.peakPipelineBytes == plan.memory.maximumBytes &&
                       result.peakPipelineBytes <= plan.memory.memoryLimit,
                   label + "_uses_budgeted_dimensions");
    vispick::gif::GifReader reader(result.path);
    const auto& metadata = reader.Metadata();
    report.Require(metadata.width == static_cast<unsigned>(plan.width) &&
                       metadata.height == static_cast<unsigned>(plan.height) &&
                       metadata.frameCount == result.frameCount,
                   label + "_file_metadata_matches_plan");
    vispick::gif::GifCanvas canvas(metadata);
    std::uint64_t duration = 0;
    bool hasCorners = true;
    for (unsigned frameIndex = 0; frameIndex < metadata.frameCount; ++frameIndex) {
        const auto frame = reader.ReadFrame(frameIndex);
        canvas.Compose(frame);
        duration += frame.metadata.delayCentiseconds;
        const vispick::BitmapView image(result.width, result.height, canvas.Pixels());
        const int left = result.width / 10;
        const int right = result.width - 1 - left;
        const int top = result.height / 10;
        const int bottom = result.height - 1 - top;
        const std::array samples{image.Row(top)[left], image.Row(top)[right], image.Row(bottom)[left],
                                 image.Row(bottom)[right]};
        for (std::size_t index = 0; index < samples.size(); ++index) {
            hasCorners = hasCorners && (((samples[index] ^ FullscreenColors[index]) & 0xFFFFFFU) == 0);
        }
    }
    report.Require(hasCorners, label + "_all_four_screen_corners_are_preserved");
    const auto elapsed = result.stopNanoseconds - result.firstFrameCompletionNanoseconds;
    const auto encoded = static_cast<std::int64_t>(duration) * 10000000;
    report.Require(duration > 0 && duration == result.durationCentiseconds &&
                       result.stopNanoseconds == static_cast<std::int64_t>(options.maxSeconds) * 1000000000LL &&
                       elapsed >= encoded && elapsed - encoded < 10000000,
                   label + "_automatic_stop_and_duration_match");
}

void RunFullscreenOnly(Report& report) {
    vispick::InitializeDpiAwareness();
    if (FindWindowW(L"VisPick.Main", nullptr) != nullptr) {
        throw std::runtime_error("Existing product instance detected; no test windows were created");
    }
    MONITORINFO primary{sizeof(primary)};
    if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary)) {
        throw std::runtime_error("Cannot locate the primary monitor");
    }
    CheckFullscreenRecording(report, primary.rcMonitor, "primary_fullscreen");
    const auto monitors = vispick::EnumerateMonitors();
    if (monitors.size() == 2) {
        const auto& first = monitors[0].bounds;
        const auto& second = monitors[1].bounds;
        const bool adjacent = (first.top == second.top && first.bottom == second.bottom &&
                               (first.right == second.left || second.right == first.left)) ||
                              (first.left == second.left && first.right == second.right &&
                               (first.bottom == second.top || second.bottom == first.top));
        if (adjacent) { CheckFullscreenRecording(report, vispick::VirtualDesktopBounds(), "dual_fullscreen"); }
    }
    report.Require(vispick::Window::ExceptionCount() == 0, "no_window_callback_exceptions");
}

class EscapeReservation final {
public:
    explicit EscapeReservation(HWND window)
        : window_(window), acquired_(RegisterHotKey(window_, 0x1600, MOD_NOREPEAT, VK_ESCAPE) != FALSE) {}
    ~EscapeReservation() {
        if (acquired_) { UnregisterHotKey(window_, 0x1600); }
    }
    EscapeReservation(const EscapeReservation&) = delete;
    EscapeReservation& operator=(const EscapeReservation&) = delete;
    [[nodiscard]] bool Acquired() const noexcept { return acquired_; }

private:
    HWND window_;
    bool acquired_;
};

void Run(Report& report, const std::filesystem::path& output) {
    report.output = output;
    vispick::InitializeDpiAwareness();
    report.performanceEligible = vispick::DetectTestEnvironment().physicalAcceptanceEligible;
    if (FindWindowW(L"VisPick.Main", nullptr) != nullptr) {
        throw std::runtime_error("Existing product instance detected; no test windows were created");
    }
    Owner owner;
    PatternWindow pattern;
    report.values.emplace_back("capture_screen_left", pattern.bounds.left);
    report.values.emplace_back("capture_screen_top", pattern.bounds.top);
    report.values.emplace_back("capture_screen_right", pattern.bounds.right);
    report.values.emplace_back("capture_screen_bottom", pattern.bounds.bottom);
    vispick::Config::Gif options;
    options.showCursor = false;
    options.maxSeconds = static_cast<int>(recordingSeconds);
    auto recorder = std::make_unique<vispick::Recorder>(pattern.bounds, options, owner.Handle(), RecordingDone, true);
    double startMilliseconds = 0;
    // Keep Escape occupied only while starting. This uses an owned window and
    // never sends keyboard input or modifies another app's registration.
    const bool escapeInitiallyFree = [&] {
        const EscapeReservation reserved(owner.Handle());
        const auto start = Clock::now();
        recorder->Start();
        startMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return reserved.Acquired();
    }();
    if (escapeInitiallyFree) {
        const EscapeReservation reserved(owner.Handle());
        report.Require(reserved.Acquired(), "active_recording_leaves_global_escape_available");
    }
    report.Require(!recorder->Finished(), "recording_starts_when_escape_is_reserved");
    MemorySamples memory;
    auto automatic = Finish(owner, recorder, std::chrono::seconds(recordingSeconds + 15), [&] { memory.Sample(); });
    report.values.emplace_back("ui_start_milliseconds", startMilliseconds);
    report.Require(!automatic.cancelled && automatic.error.empty() && !automatic.path.empty(),
                   "automatic_stop_produces_recording");
    report.Require(automatic.stopNanoseconds == static_cast<std::int64_t>(recordingSeconds) * 1000000000LL,
                   "configured_limit_uses_active_time");
    const auto attempted = automatic.capturedFrames + automatic.droppedTicks;
    const double droppedPercent =
        attempted == 0 ? 100.0 : static_cast<double>(automatic.droppedTicks) * 100.0 / static_cast<double>(attempted);
    report.values.emplace_back("requested_width", patternWidth);
    report.values.emplace_back("requested_height", patternHeight);
    report.values.emplace_back("requested_seconds", recordingSeconds);
    report.values.emplace_back("requested_fps", options.fps);
    report.values.emplace_back("automatic_dropped_tick_percent", droppedPercent);
    report.values.emplace_back("automatic_capture_failures", static_cast<double>(automatic.captureFailures));
    report.values.emplace_back("automatic_cursor_failures", static_cast<double>(automatic.cursorFailures));
    report.values.emplace_back("automatic_capture_layered_windows", automatic.captureLayeredWindows ? 1.0 : 0.0);
    report.Require(!automatic.captureLayeredWindows, "default_recording_uses_srccopy");
    report.values.emplace_back("sampled_peak_private_bytes", static_cast<double>(memory.maximumPrivate));
    report.values.emplace_back("os_peak_working_set_bytes", static_cast<double>(memory.peakWorkingSet));
    report.values.emplace_back("os_peak_commit_bytes", static_cast<double>(memory.peakCommit));
    const std::uint64_t memoryLimit = (sizeof(void*) == 8 ? 300ULL : 200ULL) * 1024 * 1024;
    report.values.emplace_back("process_memory_limit_bytes", static_cast<double>(memoryLimit));
    report.performanceMeasured =
        recordingSeconds == 60 && (sizeof(void*) == 8 ? patternWidth == 1920 && patternHeight == 1080
                                                      : patternWidth == 1280 && patternHeight == 720);
    report.performanceGatesPassed =
        droppedPercent <= 1.0 && memory.peakCommit <= memoryLimit && memory.peakWorkingSet <= memoryLimit;
    ValidateGif(report, automatic, "automatic");
    const auto automaticPath = automatic.path;
    const auto saved = output / L"automatic-saved.gif";
    {
        unsigned saveRequests = 0;
        vispick::GifPreview preview(
            std::move(automatic), {}, owner.Handle(), PreviewClosed,
            [&](HWND dialogOwner, int width, int height) -> std::optional<std::filesystem::path> {
                if (dialogOwner == nullptr || width != patternWidth || height != patternHeight) {
                    throw std::runtime_error("Invalid preview save callback arguments");
                }
                if (++saveRequests == 1) { return output / L"missing-save-directory" / L"not-saved.gif"; }
                return saved;
            },
            true);
        preview.Show();
        PumpFor(std::chrono::milliseconds(150));
        SaveOwnedClient(preview.Handle(), output / L"preview-client.png");
        CheckPreviewDpi(report, preview.Handle(), output);
        RECT client{};
        GetClientRect(preview.Handle(), &client);
        const auto dpi = static_cast<int>(vispick::DpiForWindow(preview.Handle()));
        const int x = client.right - MulDiv(145, dpi, 96);
        const int y = client.bottom - MulDiv(20, dpi, 96);
        if (!PostMessageW(preview.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, y))) {
            throw std::runtime_error("Cannot test an owned preview save failure");
        }
        WaitUntil([&] { return saveRequests == 1 && !preview.Busy(); });
        report.Require(!preview.Closed() && std::filesystem::exists(automaticPath),
                       "failed_background_save_preserves_source_and_preview");
        owner.previewClosed = false;
        if (!PostMessageW(preview.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, y))) {
            throw std::runtime_error("Cannot click owned preview Save button");
        }
        WaitUntil([&] { return owner.previewClosed && preview.Closed() && !preview.Busy(); });
        report.Require(saveRequests == 2 && preview.Closed() && std::filesystem::exists(saved) &&
                           !std::filesystem::exists(automaticPath),
                       "streaming_preview_saves_and_closes");
    }
    options.maxSeconds = 5;
    options.captureLayeredWindows = true;
    recorder = std::make_unique<vispick::Recorder>(pattern.bounds, options, owner.Handle(), RecordingDone, true);
    recorder->Start();
    PumpFor(std::chrono::milliseconds(350));
    const auto pausedToolbar = ClickRecordingPause(report, owner.Handle());
    SendMessageW(pausedToolbar, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    PumpFor(std::chrono::milliseconds(200));
    SendMessageW(pausedToolbar, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    PumpFor(std::chrono::milliseconds(250));
    ClickRecordingPause(report, owner.Handle());
    PumpFor(std::chrono::milliseconds(350));
    ClickRecordingButton(report, owner.Handle(), false, output / L"recordbar-client.png");
    auto paused = Finish(owner, recorder);
    report.Require(paused.error.empty() && !paused.path.empty(), "paused_recording_completes");
    report.Require(paused.captureLayeredWindows, "explicit_recording_uses_captureblt");
    ValidateGif(report, paused, "paused", true);
    report.Require(paused.stopNanoseconds >= 650000000 && paused.stopNanoseconds < 1000000000,
                   "toolbar_pause_survives_session_unlock_and_excludes_paused_wall_time");
    std::filesystem::copy_file(paused.path, output / L"paused.gif", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(paused.path);
    const auto temporaryBefore = OwnTemporaryFiles();
    recorder = std::make_unique<vispick::Recorder>(pattern.bounds, options, owner.Handle(), RecordingDone, true);
    recorder->Start();
    PumpFor(std::chrono::milliseconds(150));
    ClickRecordingButton(report, owner.Handle(), true);
    const auto cancelled = Finish(owner, recorder);
    report.Require(cancelled.cancelled && cancelled.path.empty() && OwnTemporaryFiles() == temporaryBefore,
                   "cancel_leaves_no_temporary_recording");
    recorder = std::make_unique<vispick::Recorder>(pattern.bounds, options, owner.Handle(), RecordingDone, true);
    recorder->Start();
    recorder->Stop();
    const auto immediate = Finish(owner, recorder);
    report.Require(immediate.error.empty() && !immediate.cancelled, "immediate_stop_is_a_clean_completion");
    if (!immediate.path.empty()) { std::filesystem::remove(immediate.path); }
    options.maxSeconds = 1;
    recorder = std::make_unique<vispick::Recorder>(pattern.bounds, options, owner.Handle(), RecordingDone, true);
    recorder->Start();
    WaitUntil([&] { return owner.recordingDone && recorder->Finished(); });
    report.Require(OwnTemporaryFiles() == temporaryBefore + 1,
                   "completed_unclaimed_recording_exists_before_destruction");
    recorder.reset();
    owner.recordingDone = false;
    report.Require(OwnTemporaryFiles() == temporaryBefore, "destructor_removes_unclaimed_completed_recording");
    report.Require(vispick::Window::ExceptionCount() == 0, "no_window_callback_exceptions");
}

std::string Escape(std::string_view text) {
    std::string escaped;
    for (const auto character : text) {
        if (character == '\\' || character == '"') { escaped += '\\'; }
        if (character == '\n' || character == '\r') {
            escaped += ' ';
        } else {
            escaped += character;
        }
    }
    return escaped;
}

} // namespace

int wmain() {
    Report report;
    auto output = std::filesystem::current_path() / L"out/reports/recording-integration";
    try {
        bool previewOnly = false;
        bool fullscreenOnly = false;
        const auto arguments = vispick::ProcessArguments();
        for (std::size_t argument = 0; argument < arguments.size(); ++argument) {
            const auto name = arguments[argument];
            if (name == L"--preview-only") {
                previewOnly = true;
                continue;
            }
            if (name == L"--fullscreen-only") {
                fullscreenOnly = true;
                continue;
            }
            if (++argument >= arguments.size()) {
                throw std::invalid_argument("Missing recording fixture option value");
            }
            if (name == L"--output") {
                output = arguments[argument];
                continue;
            }
            std::size_t consumed = 0;
            const auto value = std::stoul(arguments[argument], &consumed);
            if (consumed != arguments[argument].size()) {
                throw std::invalid_argument("Invalid recording fixture number");
            }
            if (name == L"--seconds" && value >= 1 && value <= vispick::Config::Gif::MaximumSeconds) {
                recordingSeconds = static_cast<unsigned>(value);
            } else if (name == L"--width" && value >= 320 && value <= 7680) {
                patternWidth = static_cast<int>(value);
            } else if (name == L"--height" && value >= 120 && value <= 4320) {
                patternHeight = static_cast<int>(value);
            } else {
                throw std::invalid_argument("Usage: VisPickRecordingIntegration [--output DIR] [--preview-only | "
                                            "--fullscreen-only] [--seconds 1..300] [--width N] [--height N]");
            }
        }
        output = std::filesystem::absolute(output);
        std::filesystem::create_directories(output);
        if (previewOnly && fullscreenOnly) {
            throw std::invalid_argument("Choose one isolated recording fixture mode");
        }
        if (fullscreenOnly) {
            RunFullscreenOnly(report);
        } else if (previewOnly) {
            vispick::InitializeDpiAwareness();
            Owner owner;
            CheckPreviewTimeline(report, owner, output);
        } else {
            Run(report, output);
        }
    } catch (const std::exception& error) {
        report.failure = error.what();
        std::cerr << "FAIL " << report.failure << std::endl;
    }
    std::ofstream json(output / L"recording-integration.json", std::ios::binary);
    json << std::setprecision(17);
    json << "{\n  \"checks_passed\":" << (report.failure.empty() ? "true" : "false")
         << ",\n  \"performance_acceptance_eligible\":" << (report.performanceEligible ? "true" : "false")
         << ",\n  \"performance_acceptance_measured\":" << (report.performanceMeasured ? "true" : "false")
         << ",\n  \"performance_acceptance_status\":\""
         << (!report.performanceEligible
                 ? "not_eligible"
                 : (!report.performanceMeasured ? "not_measured"
                                                : (report.performanceGatesPassed ? "passed" : "failed")))
         << "\",\n  \"memory_includes_fixture_and_probe\":true,\n  \"memory_sample_interval_ms\":50,\n  \"failure\":\""
         << Escape(report.failure) << "\",\n  \"checks\":[";
    for (std::size_t index = 0; index < report.checks.size(); ++index) {
        json << (index == 0 ? "\n    " : ",\n    ") << "{\"name\":\"" << report.checks[index].name
             << "\",\"passed\":" << (report.checks[index].passed ? "true" : "false") << '}';
    }
    json << "\n  ],\n  \"foreign_window_observations\":[";
    for (std::size_t index = 0; index < visibilityObservations.size(); ++index) {
        const auto& observation = visibilityObservations[index];
        json << (index == 0 ? "\n    " : ",\n    ")
             << "{\"milliseconds_from_fixture_creation\":" << observation.milliseconds << ",\"class\":\""
             << Escape(vispick::WideToUtf8(observation.windowClass)) << "\",\"rect\":[" << observation.rectangle.left
             << ',' << observation.rectangle.top << ',' << observation.rectangle.right << ','
             << observation.rectangle.bottom << "]}";
    }
    json << "\n  ],\n  \"measurements\":{";
    for (std::size_t index = 0; index < report.values.size(); ++index) {
        json << (index == 0 ? "\n    " : ",\n    ") << '"' << report.values[index].first
             << "\":" << report.values[index].second;
    }
    json << "\n  }\n}\n";
    if (!json) {
        std::cerr << "Cannot save recording integration report\n";
        return 2;
    }
    return report.failure.empty() ? 0 : 1;
}
