#ifndef PSAPI_VERSION
#define PSAPI_VERSION 1
#endif
#include "capture/ScreenGrab.h"
#include "editor/Annotation.h"
#include "editor/Renderer.h"
#include "gif/GifWriter.h"
#include "imaging/ImageCodec.h"
#include "pin/PinManager.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/Gdi.h"
#include "platform/Window.h"
#include "recording/Recorder.h"
#include "SoakGifWorker.h"
#include "SoakMemory.h"

#include <dwmapi.h>
#include <psapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT RecordingFinished = WM_APP + 72;
constexpr double SchedulingToleranceSeconds = 0.250;
constexpr std::uint64_t GifTickNanoseconds = 10000000;
std::atomic<HANDLE> consoleStop{nullptr};

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) { if (!value || value == INVALID_HANDLE_VALUE) { throw std::runtime_error("Create soak handle failed"); } }
    ~Handle() { CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
private:
    HANDLE value_;
};

BOOL WINAPI ConsoleControl(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT && control != CTRL_CLOSE_EVENT &&
        control != CTRL_LOGOFF_EVENT && control != CTRL_SHUTDOWN_EVENT) { return FALSE; }
    if (const auto event = consoleStop.load(std::memory_order_relaxed)) { SetEvent(event); }
    return TRUE;
}

class ConsoleStopScope {
public:
    explicit ConsoleStopScope(HANDLE event) {
        consoleStop.store(event, std::memory_order_relaxed);
        if (!SetConsoleCtrlHandler(ConsoleControl, TRUE)) { throw std::runtime_error("Install soak stop handler failed"); }
    }
    ~ConsoleStopScope() { consoleStop.store(nullptr, std::memory_order_relaxed); SetConsoleCtrlHandler(ConsoleControl, FALSE); }
    ConsoleStopScope(const ConsoleStopScope&) = delete;
    ConsoleStopScope& operator=(const ConsoleStopScope&) = delete;
};

struct Options {
    int seconds = 60;
    int intervalSeconds = 30;
    bool diagnoseMemory = false;
    bool validateGifInProcess = false;
    std::filesystem::path output = std::filesystem::path(L"out/reports") /
        (L"soak-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
};

int PositiveInteger(std::wstring_view text, int maximum) {
    int value = 0;
    if (text.empty()) { throw std::invalid_argument("Empty soak numeric option"); }
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9' || value > (maximum - (character - L'0')) / 10) {
            throw std::invalid_argument("Soak numeric option is outside its range");
        }
        value = value * 10 + character - L'0';
    }
    if (value < 1 || value > maximum) { throw std::invalid_argument("Soak numeric options must be positive"); }
    return value;
}

Options ParseOptions(int count, wchar_t** arguments) {
    Options options;
    bool secondsSeen = false, intervalSeen = false, outputSeen = false;
    for (int argument = 1; argument < count; ++argument) {
        const std::wstring_view option(arguments[argument]);
        if ((option == L"--diagnose-memory" || option == L"--diagnose-isolated-memory") && !options.diagnoseMemory) {
            options.diagnoseMemory = true;
            options.validateGifInProcess = option == L"--diagnose-memory";
            continue;
        }
        if (argument + 1 == count) { throw std::invalid_argument("Missing soak option value"); }
        const std::wstring_view value(arguments[++argument]);
        if (option == L"--seconds" && !secondsSeen) { options.seconds = PositiveInteger(value, 604800); secondsSeen = true; }
        else if (option == L"--interval-seconds" && !intervalSeen) { options.intervalSeconds = PositiveInteger(value, 3600); intervalSeen = true; }
        else if (option == L"--output" && !outputSeen && !value.empty()) { options.output = value; outputSeen = true; }
        else { throw std::invalid_argument("Unknown or duplicate soak option"); }
    }
    options.output = std::filesystem::absolute(options.output).lexically_normal();
    return options;
}

std::string JsonString(std::string_view text) {
    std::string result = "\"";
    for (const unsigned char character : text) {
        if (character == '\\' || character == '"') { result += '\\'; result += static_cast<char>(character); }
        else if (character == '\n') { result += "\\n"; }
        else if (character == '\r') { result += "\\r"; }
        else if (character == '\t') { result += "\\t"; }
        else if (character < 32) { result += '?'; }
        else { result += static_cast<char>(character); }
    }
    return result + '"';
}

std::string Timestamp() {
    SYSTEMTIME time{}; GetSystemTime(&time);
    char text[32]{};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}

bool StopRequested(HANDLE stop) noexcept { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }

void Pump(HANDLE stop) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { SetEvent(stop); continue; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}

void WaitMessages(HANDLE stop, DWORD milliseconds) {
    const DWORD result = MsgWaitForMultipleObjects(1, &stop, FALSE, milliseconds, QS_ALLINPUT);
    if (result == WAIT_FAILED) { throw std::runtime_error("Soak message wait failed"); }
    Pump(stop);
}

std::uint32_t PatternPixel(int column, int row, unsigned phase) noexcept {
    constexpr std::uint32_t colors[]{0xFF192D45, 0xFFB9314F, 0xFF27AE60, 0xFFF2C94C,
        0xFF2D9CDB, 0xFF9B51E0, 0xFFF4F4F4, 0xFF111111};
    const auto band = (static_cast<unsigned>(column / 32) + static_cast<unsigned>(row / 24)) % 8;
    return colors[(band + phase) % 8];
}

class PatternWindow final : public vispick::Window {
public:
    PatternWindow(RECT bounds, HANDLE stop) : bounds_(bounds), stop_(stop), surface_(256, 144) {
        Create({L"VisPick.Test.SoakPattern", 0, IDC_ARROW}, L"VisPick owned 256 x 144 soak pattern", WS_POPUP,
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT, bounds, vispick::WindowActivation::Passive);
        vispick::DisableWindowAnimations(Handle());
        UpdatePattern(0);
        // The console wrapper is hidden. Its startup show state must not hide the fixture.
        if (!SetWindowPos(Handle(), HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Cannot show the passive soak fixture");
        }
        UpdateWindow(Handle()); GdiFlush(); DwmFlush();
        sessionNotificationsRegistered_ = WTSRegisterSessionNotification(Handle(), NOTIFY_FOR_THIS_SESSION) != FALSE;
    }
    ~PatternWindow() override {
        if (sessionNotificationsRegistered_) { WTSUnRegisterSessionNotification(Handle()); }
        Destroy();
    }
    void UpdatePattern(unsigned phase) {
        GdiFlush();
        for (int row = 0; row < 144; ++row) {
            for (int column = 0; column < 256; ++column) {
                surface_.Pixels()[static_cast<std::size_t>(row) * 256 + column] = PatternPixel(column, row, phase);
            }
        }
        InvalidateRect(Handle(), nullptr, FALSE); UpdateWindow(Handle());
    }
    [[nodiscard]] RECT Bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool SessionNotificationsRegistered() const noexcept { return sessionNotificationsRegistered_; }
    [[nodiscard]] const char* Interruption() const noexcept { return interruption_; }
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_NCHITTEST) { return HTTRANSPARENT; }
        if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
        if (message == WM_CLOSE) { SetEvent(stop_); return 0; }
        if (message == WM_DISPLAYCHANGE || (message == WM_WTSSESSION_CHANGE &&
            (parameter == WTS_SESSION_LOCK || parameter == WTS_CONSOLE_DISCONNECT || parameter == WTS_REMOTE_DISCONNECT))) {
            if (!interruption_) {
                if (message == WM_DISPLAYCHANGE) { interruption_ = "Display configuration changed"; }
                else if (parameter == WTS_SESSION_LOCK) { interruption_ = "Capture session locked"; }
                else { interruption_ = "Capture session disconnected"; }
            }
            SetEvent(stop_);
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(Handle(), &paint);
            BitBlt(context, 0, 0, 256, 144, surface_.DeviceContext(), 0, 0, SRCCOPY);
            EndPaint(Handle(), &paint); return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
private:
    RECT bounds_;
    HANDLE stop_;
    vispick::DibSection surface_;
    bool sessionNotificationsRegistered_ = false;
    const char* interruption_ = nullptr;
};

RECT PatternBounds() {
    const auto monitors = vispick::EnumerateMonitors();
    if (monitors.empty()) { throw std::runtime_error("No display is available for the soak pattern"); }
    const auto primary = std::find_if(monitors.begin(), monitors.end(), [](const auto& monitor) { return monitor.primary; });
    const RECT work = (primary == monitors.end() ? monitors.front() : *primary).workArea;
    if (work.right - work.left < 288 || work.bottom - work.top < 220) { throw std::runtime_error("Display work area is too small for soak pattern"); }
    return {work.left + 16, work.top + 60, work.left + 272, work.top + 204};
}

struct Resources {
    DWORD gdi = 0, user = 0, handles = 0;
    std::uint64_t privateBytes = 0, privateWorkingSet = 0, workingSet = 0;
};

class ResourceSampler {
public:
    ResourceSampler() : workspace_(128 * 1024) {}
    Resources Read() {
        GdiFlush();
        bool read = false;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (QueryWorkingSet(GetCurrentProcess(), workspace_.data(), static_cast<DWORD>(workspace_.size() * sizeof(std::uintptr_t)))) { read = true; break; }
            if (GetLastError() != ERROR_BAD_LENGTH || workspace_.size() >= 4 * 1024 * 1024) { break; }
            workspace_.resize(workspace_.size() * 2);
        }
        if (!read) { throw std::runtime_error("Read process private working set failed"); }
        const auto* pages = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(workspace_.data());
        if (pages->NumberOfEntries > workspace_.size() - 1) { throw std::runtime_error("Invalid working set entry count"); }
        std::uint64_t privatePages = 0;
        for (ULONG_PTR page = 0; page < pages->NumberOfEntries; ++page) { privatePages += !pages->WorkingSetInfo[page].Shared; }
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        DWORD handles = 0;
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) ||
            !GetProcessHandleCount(GetCurrentProcess(), &handles)) { throw std::runtime_error("Read soak resource counters failed"); }
        return {GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), handles,
            memory.PrivateUsage, privatePages * system.dwPageSize, memory.WorkingSetSize};
    }
private:
    std::vector<std::uintptr_t> workspace_;
};

class OwnedRecording {
public:
    explicit OwnedRecording(std::filesystem::path path) : path_(std::move(path)) {
        const auto directory = (std::filesystem::temp_directory_path() / L"VisPick").lexically_normal();
        const auto prefix = L"rec_vispick_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        if (path_.parent_path().lexically_normal() != directory || !path_.filename().native().starts_with(prefix) || path_.extension() != L".gif") {
            throw std::runtime_error("Recorder returned a path outside this soak process's temporary files");
        }
    }
    ~OwnedRecording() { if (!path_.empty()) { std::error_code ignored; std::filesystem::remove(path_, ignored); } }
    OwnedRecording(const OwnedRecording&) = delete;
    OwnedRecording& operator=(const OwnedRecording&) = delete;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    void Remove() {
        if (!std::filesystem::remove(path_)) { throw std::runtime_error("Remove this cycle's recording failed"); }
        path_.clear();
    }
private:
    std::filesystem::path path_;
};

struct Sample {
    unsigned cycle = 0;
    bool warmup = false;
    double elapsed = 0, cycleMilliseconds = 0;
    Resources resources;
    Resources immediatelyAfterClose;
    std::uint64_t privateAfterEditor = 0, privateAfterPin = 0, privateAfterRecorder = 0, privateAfterGifValidation = 0;
    std::uint64_t gifFrames = 0, gifDurationCentiseconds = 0, gifDroppedTicks = 0;
    std::uint64_t gifActiveNanoseconds = 0, gifDurationErrorNanoseconds = 0;
};

std::uint64_t PrivateCommitBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        throw std::runtime_error("Read intermediate private commit failed");
    }
    return memory.PrivateUsage;
}

void ValidateGif(const std::filesystem::path& path, unsigned expectedFrames, std::uint64_t expectedDuration) {
    const auto gif = vispick::gif::InspectGif(path);
    if (gif.width != 256 || gif.height != 144 || gif.frameCount != expectedFrames ||
        gif.durationCentiseconds != expectedDuration) {
        throw std::runtime_error("WIC GIF dimensions, frame count or duration do not match the recorder result");
    }
}

void ExecuteCycle(PatternWindow& pattern, HANDLE stop, Sample& sample,
                  vispick::test::GifValidationWorker* validator, const auto& captureMemory) {
    const auto started = Clock::now();
    const RECT bounds = pattern.Bounds();
    pattern.UpdatePattern(0); GdiFlush(); DwmFlush();
    vispick::Bitmap flattened;
    {
        const auto frozen = vispick::CaptureScreen(bounds, true, false);
        const auto source = frozen->Image();
        for (int row = 0; row < 144; ++row) {
            for (int column = 0; column < 256; ++column) {
                if ((source.Row(row)[column] & 0xFFFFFF) != (PatternPixel(column, row, 0) & 0xFFFFFF)) {
                    throw std::runtime_error("Owned pattern capture differs at (" + std::to_string(column) + "," +
                        std::to_string(row) + "), fixture visible=" + std::to_string(IsWindowVisible(pattern.Handle()) != FALSE));
                }
            }
        }
        vispick::AnnotationModel model(source, {bounds.left, bounds.top});
        model.SetTool(vispick::AnnotationTool::Rectangle);
        model.PointerDown({bounds.left + 20, bounds.top + 20});
        model.PointerUp({bounds.left + 100, bounds.top + 80}, true);
        vispick::EditorRenderer renderer(model);
        flattened = renderer.Flatten(bounds);
        const auto png = vispick::EncodeImage(flattened.View(), vispick::SaveFormat::Png);
        const auto decoded = vispick::DecodeImage(png);
        if (decoded.Width() != flattened.Width() || decoded.Height() != flattened.Height() ||
            !std::equal(decoded.Pixels().begin(), decoded.Pixels().end(), flattened.Pixels().begin())) {
            throw std::runtime_error("PNG round trip changed annotation pixels");
        }
    }
    sample.privateAfterEditor = PrivateCommitBytes();
    captureMemory("editor_closed");
    {
        bool pinError = false;
        vispick::PinManager pins(pattern.Handle(), {}, [&](std::wstring, bool error) { pinError = pinError || error; });
        vispick::Config::Pin pinOptions;
        pinOptions.dustBox = 0;
        pins.Add(std::move(flattened), {bounds.left, bounds.top}, pinOptions);
        if (pins.Memory().windows != 1) { throw std::runtime_error("Production pin creation failed"); }
        pins.CloseAll(); pins.ProcessPending();
        if (pinError || pins.Memory().windows != 0 || pins.Memory().recycledBytes != 0) { throw std::runtime_error("Production pin close did not release the image"); }
    }
    sample.privateAfterPin = PrivateCommitBytes();
    captureMemory("pin_closed");
    Pump(stop);
    if (StopRequested(stop)) { return; }
    pattern.UpdatePattern(0); GdiFlush(); DwmFlush();
    vispick::Config::Gif options;
    options.fps = 30; options.maxSeconds = 1; options.showCursor = false; options.scale = 100;
    vispick::RecordingResult result;
    {
        vispick::Recorder recorder(bounds, options, pattern.Handle(), RecordingFinished, true);
        recorder.Start();
        const auto recordingStarted = Clock::now();
        unsigned phase = 0;
        bool stopping = false;
        while (!recorder.Finished()) {
            const auto elapsed = Clock::now() - recordingStarted;
            if (StopRequested(stop) && !stopping) { recorder.Stop(true); stopping = true; }
            if (elapsed > std::chrono::seconds(15)) { recorder.Stop(true); throw std::runtime_error("One-second recording did not finish within fifteen seconds"); }
            const auto nextPhase = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 100);
            if (phase != nextPhase && !stopping) { phase = nextPhase; pattern.UpdatePattern(phase); }
            WaitMessages(stop, 20);
        }
        auto completed = recorder.TakeResult();
        if (!completed) { throw std::runtime_error("Finished recorder has no result"); }
        result = std::move(*completed);
    } // Recorder joins and releases its real border, toolbar, slots, threads and handles.
    if (result.cancelled && StopRequested(stop)) { return; }
    if (result.path.empty()) {
        throw std::runtime_error("Production recording failed or was cancelled: " + vispick::WideToUtf8(result.error));
    }
    OwnedRecording recording(std::move(result.path));
    sample.privateAfterRecorder = PrivateCommitBytes();
    captureMemory("recorder_closed");
    if (result.cancelled || !result.error.empty()) {
        throw std::runtime_error("Production recording failed or was cancelled: " + vispick::WideToUtf8(result.error));
    }
    const auto duration = result.durationCentiseconds;
    if (result.frameCount == 0 || result.frameCount > (std::numeric_limits<unsigned>::max)() || duration == 0 ||
        duration > std::numeric_limits<std::uint64_t>::max() / GifTickNanoseconds || result.firstFrameCompletionNanoseconds < 0 ||
        result.stopNanoseconds <= result.firstFrameCompletionNanoseconds) {
        throw std::runtime_error("GIF duration or recording timestamps are invalid");
    }
    const auto activeNanoseconds = static_cast<std::uint64_t>(result.stopNanoseconds - result.firstFrameCompletionNanoseconds);
    const auto encodedNanoseconds = duration * GifTickNanoseconds;
    const auto errorNanoseconds = encodedNanoseconds > activeNanoseconds ? encodedNanoseconds - activeNanoseconds : activeNanoseconds - encodedNanoseconds;
    if (errorNanoseconds > GifTickNanoseconds) { throw std::runtime_error("GIF duration differs from the actual recording by more than one 10ms GIF tick"); }
    // Whole-file validation is test work. Keep its WIC allocator out of the long-lived resource gate.
    const auto frameCount = static_cast<unsigned>(result.frameCount);
    if (!validator) { ValidateGif(recording.Path(), frameCount, duration); }
    else if (!validator->Validate(recording.Path(), frameCount, duration, stop)) { return; }
    sample.gifFrames = result.frameCount; sample.gifDurationCentiseconds = duration; sample.gifDroppedTicks = result.droppedTicks;
    sample.gifActiveNanoseconds = activeNanoseconds; sample.gifDurationErrorNanoseconds = errorNanoseconds;
    sample.privateAfterGifValidation = PrivateCommitBytes();
    captureMemory("gif_decode_closed");
    recording.Remove();
    Pump(stop);
    sample.cycleMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

double Growth(std::uint64_t current, std::uint64_t baseline) noexcept {
    if (baseline == 0) { return current == 0 ? 0 : 100; }
    return (static_cast<double>(current) - static_cast<double>(baseline)) * 100.0 / static_cast<double>(baseline);
}

struct Report {
    Options options;
    std::string startedUtc = Timestamp();
    Clock::time_point started = Clock::now();
    std::vector<Sample> samples;
    std::optional<Resources> baseline;
    vispick::TestEnvironment environment;
    std::string error;
    bool stopped = false, finished = false;
    bool sessionNotificationsRegistered = false;
    double elapsed = 0, maximumCycleGap = 0, maximumCycleLateness = 0, lastMeasuredCycleStart = -1;

    [[nodiscard]] double MaximumGap() const noexcept {
        return lastMeasuredCycleStart < 0 ? maximumCycleGap : std::max(maximumCycleGap, elapsed - lastMeasuredCycleStart);
    }

    [[nodiscard]] bool CadenceWithinLimit() const noexcept {
        return MaximumGap() <= options.intervalSeconds + SchedulingToleranceSeconds && maximumCycleLateness <= SchedulingToleranceSeconds;
    }

    [[nodiscard]] bool ResourcesWithinLimit() const {
        if (!baseline || samples.size() <= 3) { return false; }
        for (const auto& sample : samples) {
            if (sample.warmup) { continue; }
            const auto& current = sample.resources;
            if (Growth(current.gdi, baseline->gdi) >= 5 || Growth(current.user, baseline->user) >= 5 ||
                Growth(current.handles, baseline->handles) >= 5 || Growth(current.privateBytes, baseline->privateBytes) >= 5) { return false; }
        }
        return true;
    }
    void Write(std::ostream& output) const {
        const bool cadence = CadenceWithinLimit();
        const bool completed = finished && !stopped && error.empty() && elapsed >= options.seconds;
        const bool passed = completed && ResourcesWithinLimit() && cadence;
        output << std::boolalpha << std::fixed << std::setprecision(3)
            << "{\n  \"schema_version\": 7,\n  \"started_utc\": " << JsonString(startedUtc)
            << ",\n  \"architecture_bits\": " << sizeof(void*) * 8
            << ",\n  \"requested_seconds\": " << options.seconds << ",\n  \"interval_seconds\": " << options.intervalSeconds
            << ",\n  \"elapsed_seconds\": " << elapsed << ",\n  \"finished\": " << finished
            << ",\n  \"stopped\": " << stopped << ",\n  \"passed\": " << passed
            << ",\n  \"eight_hour_elapsed\": " << (elapsed >= 28800.0)
            << ",\n  \"eight_hour_passed\": " << (passed && elapsed >= 28800.0 && options.intervalSeconds == 30 && !options.diagnoseMemory)
            << ",\n  \"memory_diagnostics_enabled\": " << options.diagnoseMemory
            << ",\n  \"gif_validation_process\": \"" << (options.validateGifInProcess ? "soak" : "persistent_child") << '"'
            << ",\n  \"physical_environment_eligible\": " << environment.physicalAcceptanceEligible
            << ",\n  \"remote_session\": " << environment.remoteSession
            << ",\n  \"session_notifications_registered\": " << sessionNotificationsRegistered
            << ",\n  \"warmup_cycles\": 3,\n  \"baseline_cycle\": " << (baseline ? 3 : 0)
            << ",\n  \"resource_idle_sample_ms\": " << std::min(2000, options.intervalSeconds * 250)
            << ",\n  \"resource_growth_below_five_percent\": " << ResourcesWithinLimit()
            << ",\n  \"maximum_cycle_gap_seconds\": " << MaximumGap()
            << ",\n  \"maximum_cycle_lateness_seconds\": " << maximumCycleLateness
            << ",\n  \"scheduling_tolerance_seconds\": " << SchedulingToleranceSeconds << ",\n  \"cadence_within_limit\": " << cadence
            << ",\n  \"error\": " << JsonString(error)
            << ",\n  \"coverage\": [\"production CaptureScreen on an owned 256x144 screen pattern\",\"screen-anchored rectangle and PNG exact roundtrip\",\"production PinManager create/close with DustBox=0\",\"production 30fps one-second Recorder and WIC GIF frame/duration decode\"],"
            << "\n  \"excluded\": [\"full interactive end-to-end workflow\",\"fullscreen Overlay and repeated focus changes\",\"user clipboard\",\"real IME typing\",\"cross-application paste and save dialogs\"],"
            << "\n  \"notes\": [\"Three warmup cycles establish the baseline; measured starts follow a fixed interval schedule.\",\"A measured start over 250ms late fails without catch-up cycles.\",\"WIC duration is checked against actual first capture completion through stop within one 10ms GIF tick.\",\"Identical post-close idle intervals precede all gated GDI, USER, handle and private commit samples. Intermediate private commit identifies the editor, pin and recorder stages.\",\"Working sets are never trimmed.\",\"Only each cycle's returned rec_vispick_<this PID> temporary GIF is deleted; reports are retained.\",\"Eight-hour fields require measured elapsed time, completed cadence and resource checks.\"],"
            << "\n  \"samples\": [\n";
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto& sample = samples[index]; const auto& resources = sample.resources;
            const auto& immediate = sample.immediatelyAfterClose;
            if (index != 0) { output << ",\n"; }
            output << "    {\"cycle\":" << sample.cycle << ",\"warmup\":" << sample.warmup << ",\"elapsed_seconds\":" << sample.elapsed
                << ",\"cycle_ms\":" << sample.cycleMilliseconds << ",\"gdi\":" << resources.gdi << ",\"user\":" << resources.user
                << ",\"handles\":" << resources.handles << ",\"private_bytes\":" << resources.privateBytes
                << ",\"private_working_set_bytes\":" << resources.privateWorkingSet << ",\"working_set_bytes\":" << resources.workingSet
                << ",\"immediate_gdi\":" << immediate.gdi << ",\"immediate_user\":" << immediate.user
                << ",\"immediate_handles\":" << immediate.handles << ",\"immediate_private_bytes\":" << immediate.privateBytes
                << ",\"private_after_editor_bytes\":" << sample.privateAfterEditor << ",\"private_after_pin_bytes\":" << sample.privateAfterPin
                << ",\"private_after_recorder_bytes\":" << sample.privateAfterRecorder
                << ",\"private_after_gif_validation_bytes\":" << sample.privateAfterGifValidation
                << ",\"immediate_private_working_set_bytes\":" << immediate.privateWorkingSet
                << ",\"immediate_working_set_bytes\":" << immediate.workingSet
                << ",\"gif_frames\":" << sample.gifFrames << ",\"gif_duration_cs\":" << sample.gifDurationCentiseconds
                << ",\"gif_active_duration_ms\":" << static_cast<double>(sample.gifActiveNanoseconds) / 1000000.0
                << ",\"gif_duration_error_ms\":" << static_cast<double>(sample.gifDurationErrorNanoseconds) / 1000000.0
                << ",\"gif_dropped_ticks\":" << sample.gifDroppedTicks << '}';
        }
        output << "\n  ]\n}\n";
    }
};

void WriteAtomic(const Report& report, const std::filesystem::path& destination) {
    auto temporary = destination;
    temporary += L".pending-" + std::to_wstring(GetCurrentProcessId());
    if (std::filesystem::exists(temporary)) { throw std::runtime_error("Soak report temporary path already exists"); }
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.imbue(std::locale::classic());
            report.Write(output); output.flush();
            if (!output) { throw std::runtime_error("Write soak report failed"); }
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Atomically replace soak report failed");
        }
    } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
}

int Run(Options options) {
    std::filesystem::create_directories(options.output);
    if (std::filesystem::exists(options.output / L"report.json")) { throw std::runtime_error("Output directory already contains a soak report; choose a fresh directory"); }
    const Handle lock(CreateFileW((options.output / L"soak.lock").c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    const Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const ConsoleStopScope console(stop.Get());
    Report report; report.options = std::move(options);
    report.samples.reserve(static_cast<std::size_t>(report.options.seconds / report.options.intervalSeconds) + 8);
    bool checkpoint10Minutes = false, checkpoint1Hour = false;
    const auto writeProgress = [&] {
        report.elapsed = std::chrono::duration<double>(Clock::now() - report.started).count();
        WriteAtomic(report, report.options.output / L"report.json");
        if (!checkpoint10Minutes && report.elapsed >= 600) { WriteAtomic(report, report.options.output / L"checkpoint-10m.json"); checkpoint10Minutes = true; }
        if (!checkpoint1Hour && report.elapsed >= 3600) { WriteAtomic(report, report.options.output / L"checkpoint-1h.json"); checkpoint1Hour = true; }
    };
    try {
        vispick::InitializeDpiAwareness();
        report.environment = vispick::DetectTestEnvironment();
        // Start the validator before warmup. Per-cycle process creation also perturbs heap commit.
        std::optional<vispick::test::GifValidationWorker> validator;
        if (!report.options.validateGifInProcess) { validator.emplace(); }
        ResourceSampler sampler;
        PatternWindow pattern(PatternBounds(), stop.Get());
        report.sessionNotificationsRegistered = pattern.SessionNotificationsRegistered();
        std::optional<vispick::test::SoakMemoryProbe> memoryProbe;
        std::ofstream memoryOutput;
        if (report.options.diagnoseMemory) {
            memoryOutput.open(report.options.output / L"memory.jsonl", std::ios::binary | std::ios::trunc);
            if (!memoryOutput) { throw std::runtime_error("Open soak memory diagnostics failed"); }
            memoryProbe.emplace();
        }
        writeProgress();
        auto nextCycle = Clock::now();
        std::optional<Clock::time_point> previousMeasuredStart;
        while (!StopRequested(stop.Get())) {
            const auto now = Clock::now();
            const bool warmup = report.samples.size() < 3;
            if (!warmup && now - report.started >= std::chrono::seconds(report.options.seconds) && report.samples.size() > 3) { break; }
            if (now < nextCycle) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextCycle - now).count();
                WaitMessages(stop.Get(), static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 1000)));
                // Atomic heartbeat retains actual elapsed time even between sparse cycles.
                if (std::chrono::duration<double>(Clock::now() - report.started).count() - report.elapsed >= 30) { writeProgress(); }
                continue;
            }
            const auto cycleStarted = Clock::now();
            if (!warmup && previousMeasuredStart) {
                report.maximumCycleGap = std::max(report.maximumCycleGap, std::chrono::duration<double>(cycleStarted - *previousMeasuredStart).count());
            }
            if (!warmup) {
                const double lateness = std::chrono::duration<double>(cycleStarted - nextCycle).count();
                report.maximumCycleLateness = std::max(report.maximumCycleLateness, lateness);
                if (lateness > SchedulingToleranceSeconds) { throw std::runtime_error("Soak cycle started more than 250ms late; missed cycles are not replayed"); }
                previousMeasuredStart = cycleStarted;
                report.lastMeasuredCycleStart = std::chrono::duration<double>(cycleStarted - report.started).count();
            }
            Sample sample;
            sample.cycle = static_cast<unsigned>(report.samples.size() + 1); sample.warmup = warmup;
            const auto captureMemory = [&](std::string_view stage) {
                if (memoryProbe) { memoryProbe->Capture(memoryOutput, sample.cycle, stage); }
            };
            captureMemory("cycle_start");
            ExecuteCycle(pattern, stop.Get(), sample, validator ? &*validator : nullptr, captureMemory);
            if (StopRequested(stop.Get())) { break; }
            sample.immediatelyAfterClose = sampler.Read();
            captureMemory("immediate");
            // Use the same quiescent measurement for warmup and every cycle.
            // Keep the immediate snapshot so delayed runtime cleanup is visible.
            const auto idleSampleAt = Clock::now() + std::chrono::milliseconds(std::min(2000, report.options.intervalSeconds * 250));
            while (!StopRequested(stop.Get())) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(idleSampleAt - Clock::now()).count();
                if (remaining <= 0) { break; }
                WaitMessages(stop.Get(), static_cast<DWORD>(remaining));
            }
            if (StopRequested(stop.Get())) { break; }
            sample.resources = sampler.Read();
            captureMemory("settled");
            if (memoryProbe) {
                memoryOutput.flush();
                if (!memoryOutput) { throw std::runtime_error("Write soak memory diagnostics failed"); }
            }
            sample.elapsed = std::chrono::duration<double>(Clock::now() - report.started).count();
            report.samples.push_back(sample);
            if (report.samples.size() == 3) { report.baseline = sample.resources; }
            writeProgress();
            std::cout << "cycle=" << sample.cycle << " warmup=" << sample.warmup << " elapsed=" << sample.elapsed
                << " private_bytes=" << sample.resources.privateBytes << " gdi=" << sample.resources.gdi
                << " user=" << sample.resources.user << " handles=" << sample.resources.handles << std::endl;
            if (warmup) { nextCycle = Clock::now(); }
            else { nextCycle += std::chrono::seconds(report.options.intervalSeconds); }
        }
        report.stopped = StopRequested(stop.Get());
        if (const auto* interruption = pattern.Interruption()) { report.error = interruption; }
    } catch (const std::exception& error) { report.error = error.what(); }
    report.finished = true;
    writeProgress();
    if (!report.error.empty()) { std::cerr << "Soak failed: " << report.error << '\n'; return 1; }
    if (report.stopped) { std::cerr << "Soak stopped; partial report preserved.\n"; return 2; }
    const bool cadence = report.CadenceWithinLimit();
    return report.ResourcesWithinLimit() && cadence ? 0 : 1;
}

} // namespace

int wmain(int count, wchar_t** arguments) {
    try {
        if (count > 1 && std::wstring_view(arguments[1]) == L"--gif-validation-worker") {
            SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            return vispick::test::ServeGifValidationRequests(count, arguments, ValidateGif);
        }
        return Run(ParseOptions(count, arguments));
    }
    catch (const std::exception& error) { std::cerr << "Soak could not start: " << error.what() << '\n'; return 1; }
}
