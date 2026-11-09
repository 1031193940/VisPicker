#include "recording/Recorder.h"
#include "recording/FrameChange.h"
#include "recording/RecordingFiles.h"
#include "gif/Timing.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/Gdi.h"
#include "platform/Window.h"

#include <Windowsx.h>
#include <mmsystem.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace vispick {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::int64_t Second = 1000000000;

class KernelHandle final {
public:
    explicit KernelHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~KernelHandle() { if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
private:
    HANDLE handle_;
};

class CaptureTimer final {
public:
    CaptureTimer() {
        using CreateTimer = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,LPCWSTR,DWORD,DWORD);
        const auto module = GetModuleHandleW(L"kernel32.dll");
        const auto address = module == nullptr ? nullptr : GetProcAddress(module, "CreateWaitableTimerExW");
        CreateTimer create = nullptr;
        static_assert(sizeof(create) == sizeof(address));
        std::memcpy(&create, &address, sizeof(create));
        if (create != nullptr) { timer_ = create(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS); }
        if (timer_ == nullptr) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            periodSet_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
        if (timer_ == nullptr) { if (periodSet_) { timeEndPeriod(1); } throw std::runtime_error("Cannot create recording timer"); }
    }
    ~CaptureTimer() { CancelWaitableTimer(timer_); CloseHandle(timer_); if (periodSet_) { timeEndPeriod(1); } }
    CaptureTimer(const CaptureTimer&) = delete;
    CaptureTimer& operator=(const CaptureTimer&) = delete;
    void Wait(std::int64_t nanoseconds, HANDLE wake) {
        if (nanoseconds <= 0) { return; }
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(1, nanoseconds / 100);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) { throw std::runtime_error("Cannot arm recording timer"); }
        const HANDLE events[]{wake, timer_};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_FAILED) { throw std::runtime_error("Recording timer wait failed"); }
    }
private:
    HANDLE timer_ = nullptr;
    bool periodSet_ = false;
};

class JoinThread final {
public:
    explicit JoinThread(std::thread& thread) noexcept : thread_(thread) {}
    ~JoinThread() { if (thread_.joinable()) { thread_.join(); } }
    JoinThread(const JoinThread&) = delete;
    JoinThread& operator=(const JoinThread&) = delete;
private:
    std::thread& thread_;
};

class JoinWorkers final {
public:
    explicit JoinWorkers(std::vector<std::thread>& threads) noexcept : threads_(threads) {}
    ~JoinWorkers() { for (auto& thread : threads_) { if (thread.joinable()) { thread.join(); } } }
    JoinWorkers(const JoinWorkers&) = delete;
    JoinWorkers& operator=(const JoinWorkers&) = delete;
private:
    std::vector<std::thread>& threads_;
};

class TemporaryRecording final {
public:
    TemporaryRecording() {
        const auto directory = std::filesystem::temp_directory_path() / L"VisPick";
        std::filesystem::create_directories(directory);
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            path = directory / (L"rec_vispick_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(sequence.fetch_add(1)) + L".gif");
            const KernelHandle reserved(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (reserved.Get() != INVALID_HANDLE_VALUE) { return; }
            if (GetLastError() != ERROR_FILE_EXISTS) { throw std::runtime_error("Cannot reserve temporary recording"); }
        }
        throw std::runtime_error("Cannot find an unused recording filename");
    }
    ~TemporaryRecording() { if (!keep) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
    TemporaryRecording(const TemporaryRecording&) = delete;
    TemporaryRecording& operator=(const TemporaryRecording&) = delete;
    std::filesystem::path path;
    bool keep = false;
};

class RecordBorder final : public Window {
public:
    ~RecordBorder() override { Destroy(); }
    void Open(RECT bounds, HWND owner) {
        Create({L"VisPick.RecordBorder",0,IDC_ARROW}, L"", WS_POPUP,
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST,
            {bounds.left-2,bounds.top-2,bounds.right+2,bounds.bottom+2}, WindowActivation::Passive, owner);
        if (!SetLayeredWindowAttributes(Handle(), RGB(0,0,0), 0, LWA_COLORKEY)) { throw std::runtime_error("Cannot create recording border"); }
        static_cast<void>(TryExcludeFromCapture(Handle()));
        if (!SetWindowPos(Handle(),nullptr,0,0,0,0,SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Cannot show recording border");
        }
    }
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_NCHITTEST) { return HTTRANSPARENT; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto context = BeginPaint(Handle(), &paint);
            RECT client{};
            GetClientRect(Handle(), &client);
            FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetDCBrushColor(context, RGB(235,60,65));
            const RECT edges[]{{0,0,client.right,2},{0,client.bottom-2,client.right,client.bottom},
                {0,2,2,client.bottom-2},{client.right-2,2,client.right,client.bottom-2}};
            for (const auto& edge : edges) { FillRect(context, &edge, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
            EndPaint(Handle(), &paint);
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
};

class RecordBar final : public Window {
public:
    RecordBar(std::function<void(bool)> stop, std::function<void(bool)> session,
              std::function<std::int64_t()> elapsed, std::function<void()> retryCompletion, int maximum, bool english)
        : stop_(std::move(stop)), session_(std::move(session)), elapsed_(std::move(elapsed)),
          retryCompletion_(std::move(retryCompletion)), maximum_(maximum), english_(english) {}
    ~RecordBar() override {
        if (Handle()) {
            if (sessionRegistered_) { WTSUnRegisterSessionNotification(Handle()); }
        }
        Destroy();
    }
    void Open(RECT capture, HWND owner) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&capture, MONITOR_DEFAULTTONEAREST), &monitor)) { throw std::runtime_error("Cannot locate recording toolbar monitor"); }
        dpi_ = static_cast<int>(DpiForWindow(owner));
        for (const auto& display : EnumerateMonitors()) {
            if (EqualRect(&display.bounds,&monitor.rcMonitor)) { dpi_ = static_cast<int>(display.dpiX); break; }
        }
        const int width = MulDiv(350,dpi_,96);
        const int height = MulDiv(40,dpi_,96);
        RECT bar{std::clamp(capture.left, monitor.rcWork.left, std::max(monitor.rcWork.left,monitor.rcWork.right-width)),
            capture.bottom+6,0,0};
        bool outside = bar.top + height <= monitor.rcWork.bottom;
        if (!outside) { bar.top = capture.top-height-6; outside = bar.top >= monitor.rcWork.top; }
        if (!outside) { bar.top = std::max(monitor.rcWork.top,capture.bottom-height-6); }
        bar.right = bar.left+width;
        bar.bottom = bar.top+height;
        Create({L"VisPick.RecordBar",0,IDC_ARROW}, english_ ? L"VisPick Recording" : L"VisPick 录制", WS_POPUP,
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, bar, WindowActivation::Passive, owner);
        dpi_ = static_cast<int>(DpiForWindow(Handle()));
        CreateFontForDpi();
        const bool excluded = TryExcludeFromCapture(Handle());
        sessionRegistered_ = WTSRegisterSessionNotification(Handle(), NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!sessionRegistered_) { throw std::runtime_error("Cannot register recording lock-screen notifications"); }
        if (!SetTimer(Handle(), 1, 250, nullptr)) { throw std::runtime_error("Cannot start recording toolbar updates"); }
        if ((outside || excluded) && !SetWindowPos(Handle(),nullptr,0,0,0,0,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Cannot show recording toolbar");
        }
        // Win7/full-screen: no safe room outside capture, so keep the bar hidden.
        // The application's recording shortcut can still stop to preview.
    }
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
        if (message == WM_TIMER) { retryCompletion_(); InvalidateRect(Handle(), nullptr, FALSE); return 0; }
        if (message == WM_WTSSESSION_CHANGE) {
            if (parameter == WTS_SESSION_LOCK) { session_(true); }
            else if (parameter == WTS_SESSION_UNLOCK) { session_(false); }
            return 0;
        }
        if (message == WM_LBUTTONUP) {
            const POINT position{GET_X_LPARAM(data),GET_Y_LPARAM(data)};
            const auto rectangles = Layout();
            if (PtInRect(&rectangles[1],position)) { stop_(false); }
            else if (PtInRect(&rectangles[2],position)) { Cancel(); }
            return 0;
        }
        if (message == WM_DPICHANGED) {
            dpi_ = HIWORD(parameter);
            CreateFontForDpi();
            const auto& rectangle = *reinterpret_cast<const RECT*>(data);
            SetWindowPos(Handle(),nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,
                rectangle.bottom-rectangle.top,SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        if (message == WM_CLOSE) { Cancel(); return 0; }
        if (message == WM_PRINTCLIENT) { Paint(reinterpret_cast<HDC>(parameter)); return 0; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto context = BeginPaint(Handle(), &paint);
            try { Paint(context); }
            catch (...) { EndPaint(Handle(), &paint); throw; }
            EndPaint(Handle(), &paint);
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
private:
    void Cancel() {
        // Preserve longer recordings for an explicit discard in the preview.
        stop_(elapsed_() < 3 * Second);
    }
    void Paint(HDC context) {
        RECT client{};
        GetClientRect(Handle(), &client);
        SetDCBrushColor(context, RGB(35,38,45));
        FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context, RGB(245,245,245));
        const SelectObjectGuard font(context, font_.Get());
        const auto seconds = elapsed_() / Second;
        const auto twoDigits = [](std::int64_t value) { return (value < 10 ? L"0" : L"") + std::to_wstring(value); };
        const auto text = twoDigits(seconds/60) + L":" + twoDigits(seconds%60) + L" / " +
            twoDigits(maximum_/60) + L":" + twoDigits(maximum_%60);
        auto rectangles = Layout();
        DrawTextW(context,text.c_str(),static_cast<int>(text.size()),&rectangles[0],DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetDCBrushColor(context,RGB(65,69,79));
        FillRect(context,&rectangles[1],static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        FillRect(context,&rectangles[2],static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        DrawTextW(context,english_ ? L"Stop" : L"停止",-1,&rectangles[1],DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(context,english_ ? L"Cancel" : L"取消",-1,&rectangles[2],DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    std::array<RECT,3> Layout() const noexcept {
        RECT client{}; GetClientRect(Handle(),&client);
        const int margin = MulDiv(8,dpi_,96);
        const int buttonWidth = MulDiv(82,dpi_,96);
        const RECT cancel{client.right-margin-buttonWidth,margin,client.right-margin,client.bottom-margin};
        const RECT stop{cancel.left-margin-buttonWidth,margin,cancel.left-margin,client.bottom-margin};
        const RECT timer{margin,margin,stop.left-margin,client.bottom-margin};
        return {timer,stop,cancel};
    }
    void CreateFontForDpi() {
        const auto version = GetWindowsVersion();
        const bool hasUiFont = version.major > 6 || (version.major == 6 && version.minor >= 2);
        const auto font = CreateFontW(-MulDiv(14,dpi_,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,
            english_ ? L"Segoe UI" : (hasUiFont ? L"Microsoft YaHei UI" : L"Microsoft YaHei"));
        if (font == nullptr) { throw std::runtime_error("Cannot create recording toolbar font"); }
        font_.Reset(font);
    }
    std::function<void(bool)> stop_;
    std::function<void(bool)> session_;
    std::function<std::int64_t()> elapsed_;
    std::function<void()> retryCompletion_;
    int maximum_;
    bool english_;
    int dpi_ = 96;
    UniqueGdiObject<HFONT> font_;
    bool sessionRegistered_ = false;
};

bool DrawRecordingCursor(HDC context, const RECT& bounds, bool showCursor, bool highlight) {
    if (!showCursor && !highlight) { return true; }
    CURSORINFO cursor{sizeof(cursor)};
    if (!GetCursorInfo(&cursor)) { return false; }
    if ((cursor.flags & CURSOR_SHOWING) == 0) { return true; }
    const int x = cursor.ptScreenPos.x-bounds.left;
    const int y = cursor.ptScreenPos.y-bounds.top;
    if (highlight && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0) {
        const UniqueGdiObject<HPEN> pen(CreatePen(PS_SOLID, 3, RGB(255,190,20)));
        if (!pen.Get()) { return false; }
        try {
            const SelectObjectGuard selectedPen(context, pen.Get());
            const SelectObjectGuard selectedBrush(context, GetStockObject(NULL_BRUSH));
            if (!Ellipse(context,x-15,y-15,x+16,y+16)) { return false; }
        } catch (const std::system_error&) { return false; }
    }
    if (showCursor) {
        ICONINFO information{};
        if (!GetIconInfo(cursor.hCursor, &information)) { return false; }
        const UniqueGdiObject<HBITMAP> color(information.hbmColor);
        const UniqueGdiObject<HBITMAP> mask(information.hbmMask);
        if (!DrawIconEx(context,x-static_cast<int>(information.xHotspot),y-static_cast<int>(information.yHotspot),
            cursor.hCursor,0,0,0,nullptr,DI_NORMAL)) { return false; }
    }
    return true;
}

enum class SlotStatus { Free, Capturing, Queued, Encoding, Ready };
struct FrameSlot {
    SlotStatus status = SlotStatus::Free;
    std::uint64_t sequence = 0;
    std::int64_t timestamp = 0;
    RECT rectangle{};
    std::vector<std::uint32_t> current;
    std::vector<std::uint32_t> previous;
    recording::EncodedRegion encoded;
};

} // namespace

struct Recorder::Implementation {
    RECT bounds;
    Config::Gif options;
    HWND owner;
    UINT notification;
    bool english;
    int width = 0;
    int height = 0;
    recording::MemoryPlan plan;
    std::unique_ptr<RecordBorder> border;
    std::unique_ptr<RecordBar> bar;
    KernelHandle wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread coordinator;
    std::vector<FrameSlot> slots;
    std::exception_ptr failure;
    Clock::time_point started{};
    recording::ActiveTimeline timeline;
    bool startRequested = false;
    bool stopRequested = false;
    bool cancelled = false;
    bool userPaused = false;
    bool sessionPaused = false;
    bool captureDone = false;
    bool resultTaken = false;
    std::atomic<bool> finished{false};
    std::atomic<bool> completionPosted{false};
    std::uint64_t submitted = 0;
    std::int64_t stopTimestamp = 0;
    RecordingResult result;

    Implementation(RECT rectangle, Config::Gif settings, HWND window, UINT message, bool englishLanguage)
        : bounds(rectangle), options(settings), owner(window), notification(message), english(englishLanguage) {
        if (wake.Get() == nullptr) { throw std::runtime_error("Cannot create recording control event"); }
        const auto sourceWidth = static_cast<std::int64_t>(bounds.right)-bounds.left;
        const auto sourceHeight = static_cast<std::int64_t>(bounds.bottom)-bounds.top;
        if (sourceWidth < 1 || sourceHeight < 1 || sourceWidth > 65535 || sourceHeight > 65535 ||
            options.fps < 1 || options.fps > 50 || options.maxSeconds < 1 || options.maxSeconds > recording::MaximumRecordingSeconds ||
            options.scale < 0 || options.scale > 100 || owner == nullptr || notification < WM_APP || notification > 0xBFFF) {
            throw std::invalid_argument("Invalid recording options or completion window");
        }
        int scale = options.scale;
        if (scale == 0) {
            UINT dpi = 96;
            const POINT center{bounds.left+static_cast<LONG>(sourceWidth/2),bounds.top+static_cast<LONG>(sourceHeight/2)};
            for (const auto& monitor : EnumerateMonitors()) { if (PtInRect(&monitor.bounds,center)) { dpi = monitor.dpiX; break; } }
            scale = static_cast<int>(std::min(100U,9600U/std::max(96U,dpi)));
        }
        width = std::max(1,static_cast<int>(sourceWidth*scale/100));
        height = std::max(1,static_cast<int>(sourceHeight*scale/100));
        plan = recording::PlanMemory(static_cast<int>(sourceWidth),static_cast<int>(sourceHeight),width,height,std::thread::hardware_concurrency());
        result.width = width;
        result.height = height;
        result.peakPipelineBytes = plan.maximumBytes;
        result.workerCount = plan.workerCount;
        result.slotCount = plan.slotCount;
        result.captureLayeredWindows = options.captureLayeredWindows;
    }

    ~Implementation() {
        Stop(true);
        if (coordinator.joinable()) { coordinator.join(); }
        if (!resultTaken && !result.path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(result.path,ignored);
        }
        bar.reset();
        border.reset();
    }

    std::int64_t RelativeNow() const { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count(); }
    std::int64_t Elapsed() const {
        std::lock_guard lock(mutex);
        if (!startRequested) { return 0; }
        return stopRequested ? stopTimestamp : timeline.Elapsed(RelativeNow());
    }
    void Stop(bool discard) noexcept {
        {
            std::lock_guard lock(mutex);
            cancelled = cancelled || discard;
            if (!stopRequested && startRequested) {
                stopTimestamp = std::min(timeline.Elapsed(RelativeNow()),static_cast<std::int64_t>(options.maxSeconds)*Second);
            }
            stopRequested = true;
        }
        SetEvent(wake.Get());
        changed.notify_all();
    }
    void Pause(bool pause, bool session) noexcept {
        std::lock_guard lock(mutex);
        if (stopRequested || !startRequested) { return; }
        if (session) { sessionPaused = pause; } else { userPaused = pause; }
        timeline.Pause(RelativeNow(),userPaused || sessionPaused);
        SetEvent(wake.Get());
    }
    void Fail() noexcept {
        {
            std::lock_guard lock(mutex);
            if (!failure) { failure = std::current_exception(); }
            stopRequested = true;
        }
        SetEvent(wake.Get());
        changed.notify_all();
    }

    void NotifyCompletion() noexcept {
        if (!finished.load(std::memory_order_acquire) || completionPosted.exchange(true)) { return; }
        if (!PostMessageW(owner,notification,0,0)) { completionPosted.store(false); }
    }

    void Start() {
        if (startRequested) { throw std::logic_error("Recording already started"); }
        border = std::make_unique<RecordBorder>();
        border->Open(bounds,owner);
        bar = std::make_unique<RecordBar>([this](bool cancel) { Stop(cancel); },
            [this](bool paused) { Pause(paused,true); },[this] { return Elapsed(); },
            [this] { NotifyCompletion(); },options.maxSeconds,english);
        bar->Open(bounds,owner);
        {
            std::lock_guard lock(mutex);
            started = Clock::now();
            startRequested = true;
        }
        coordinator = std::thread([this] { Run(); });
    }

    void EncodeLoop() noexcept {
        try {
            recording::RegionEncoder encoder;
            for (;;) {
                std::size_t selected = 0;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock,[&] {
                        return failure || cancelled || captureDone || std::any_of(slots.begin(),slots.end(),
                            [](const FrameSlot& slot) { return slot.status == SlotStatus::Queued; });
                    });
                    if (failure || cancelled) { return; }
                    auto found = slots.end();
                    for (auto candidate = slots.begin(); candidate != slots.end(); ++candidate) {
                        if (candidate->status == SlotStatus::Queued && (found == slots.end() || candidate->sequence < found->sequence)) {
                            found = candidate;
                        }
                    }
                    if (found == slots.end()) { if (captureDone) { return; } continue; }
                    selected = static_cast<std::size_t>(found-slots.begin());
                    found->status = SlotStatus::Encoding;
                }
                auto& slot = slots[selected];
                auto encoded = encoder.Encode(slot.current,slot.previous,slot.rectangle.right-slot.rectangle.left,options.dither);
                {
                    std::lock_guard lock(mutex);
                    slot.encoded = std::move(encoded);
                    slot.status = SlotStatus::Ready;
                }
                changed.notify_all();
            }
        } catch (...) { Fail(); }
    }

    void WriteLoop(const std::filesystem::path& path) noexcept {
        try {
            gif::GifWriter writer(path,width,height);
            std::optional<recording::EncodedRegion> pending;
            gif::GifFrameMetadata pendingMetadata;
            std::optional<gif::TimestampDelayAccumulator> delays;
            const auto writePending = [&](std::int64_t endTimestamp) {
                pendingMetadata.delayCentiseconds = recording::RecordedFrameDelay(delays->AdvanceTo(endTimestamp));
                writer.WriteFrame(pending->frame,pendingMetadata);
                result.durationCentiseconds += pendingMetadata.delayCentiseconds;
            };
            std::uint64_t next = 0;
            for (;;) {
                recording::EncodedRegion encoded;
                gif::GifFrameMetadata metadata;
                std::int64_t timestamp = 0;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock,[&] {
                        return failure || cancelled || (captureDone && next == submitted) ||
                            std::any_of(slots.begin(),slots.end(),[&](const FrameSlot& slot) { return slot.status == SlotStatus::Ready && slot.sequence == next; });
                    });
                    if (failure || cancelled) { return; }
                    if (captureDone && next == submitted) { break; }
                    const auto found = std::find_if(slots.begin(),slots.end(),[&](const FrameSlot& slot) { return slot.status == SlotStatus::Ready && slot.sequence == next; });
                    if (found == slots.end()) { continue; }
                    encoded = std::move(found->encoded);
                    metadata = {static_cast<unsigned>(found->rectangle.left),static_cast<unsigned>(found->rectangle.top),
                        static_cast<unsigned>(found->rectangle.right-found->rectangle.left),static_cast<unsigned>(found->rectangle.bottom-found->rectangle.top),
                        0,gif::DisposalMethod::Keep,encoded.transparentColorIndex};
                    timestamp = found->timestamp;
                    found->status = SlotStatus::Free;
                    ++next;
                }
                if (pending) {
                    writePending(timestamp);
                } else { delays.emplace(timestamp); result.firstFrameCompletionNanoseconds = timestamp; }
                pending = std::move(encoded);
                pendingMetadata = metadata;
            }
            if (pending) {
                result.stopNanoseconds = stopTimestamp;
                writePending(stopTimestamp);
            }
            if (writer.FrameCount() == 0) { return; }
            writer.Finish();
            result.frameCount = static_cast<std::uint64_t>(writer.FrameCount());
        } catch (...) { Fail(); }
    }

    void CaptureLoop() {
        const int sourceWidth = bounds.right-bounds.left;
        const int sourceHeight = bounds.bottom-bounds.top;
        ScreenDc screen;
        DibSection surface(sourceWidth,sourceHeight);
        std::unique_ptr<DibSection> scaled;
        if (width != sourceWidth || height != sourceHeight) { scaled = std::make_unique<DibSection>(width,height); }
        Bitmap previous(width,height);
        CaptureTimer timer;
        std::int64_t nextTick = 0;
        std::int64_t lastSubmitted = 0;
        std::optional<std::int64_t> firstCaptureFailure;
        bool hasPrevious = false;
        const std::int64_t period = Second/options.fps;
        const std::int64_t maximumTimestamp = static_cast<std::int64_t>(options.maxSeconds)*Second;
        const DWORD rasterOperation = SRCCOPY | (options.captureLayeredWindows ? CAPTUREBLT : 0);
        for (;;) {
            std::size_t selected = 0;
            std::int64_t waitNanoseconds = 0;
            bool paused = false;
            {
                std::lock_guard lock(mutex);
                if (stopRequested || failure) { break; }
                paused = userPaused || sessionPaused;
                const auto now = timeline.Elapsed(RelativeNow());
                if (now >= maximumTimestamp) {
                    stopTimestamp = maximumTimestamp;
                    stopRequested = true;
                    break;
                }
                waitNanoseconds = nextTick-now;
            }
            if (paused) { WaitForSingleObject(wake.Get(),INFINITE); continue; }
            if (waitNanoseconds > 0) { timer.Wait(waitNanoseconds,wake.Get()); continue; }
            {
                std::lock_guard lock(mutex);
                if (stopRequested || userPaused || sessionPaused) { continue; }
                const auto now = timeline.Elapsed(RelativeNow());
                if (now > nextTick+period) { result.droppedTicks += static_cast<std::uint64_t>((now-nextTick)/period); }
                nextTick = (now/period+1)*period;
                const auto found = std::find_if(slots.begin(),slots.end(),[](const FrameSlot& slot) { return slot.status == SlotStatus::Free; });
                if (found == slots.end()) { ++result.droppedTicks; continue; }
                selected = static_cast<std::size_t>(found-slots.begin());
                found->status = SlotStatus::Capturing;
                const auto busy = static_cast<std::uint64_t>(std::count_if(slots.begin(),slots.end(),
                    [](const FrameSlot& slot) { return slot.status != SlotStatus::Free; }));
                result.peakInFlightFrames = std::max(result.peakInFlightFrames,busy);
            }
            bool captured = BitBlt(surface.DeviceContext(),0,0,sourceWidth,sourceHeight,screen.Get(),bounds.left,bounds.top,rasterOperation) != FALSE;
            if (captured && !DrawRecordingCursor(surface.DeviceContext(),bounds,options.showCursor,options.highlightClicks)) { ++result.cursorFailures; }
            if (captured && scaled) {
                SetStretchBltMode(scaled->DeviceContext(),HALFTONE);
                SetBrushOrgEx(scaled->DeviceContext(),0,0,nullptr);
                captured = StretchBlt(scaled->DeviceContext(),0,0,width,height,surface.DeviceContext(),0,0,sourceWidth,sourceHeight,SRCCOPY) != FALSE;
            }
            if (!GdiFlush()) { captured = false; }
            std::int64_t timestamp = 0;
            {
                std::lock_guard lock(mutex);
                timestamp = timeline.Elapsed(RelativeNow());
                if (!captured) {
                    ++result.captureFailures;
                    ++result.droppedTicks;
                    slots[selected].status = SlotStatus::Free;
                    if (stopRequested || userPaused || sessionPaused) { continue; }
                    if (!firstCaptureFailure) { firstCaptureFailure = timestamp; }
                    if (timestamp-*firstCaptureFailure < Second) { continue; }
                    if (!hasPrevious) { throw std::runtime_error("Screen capture remained unavailable for one second"); }
                    stopTimestamp = std::min(*firstCaptureFailure,maximumTimestamp);
                    stopRequested = true;
                    result.captureInterrupted = true;
                    break;
                }
                firstCaptureFailure.reset();
                ++result.capturedFrames;
                if (stopRequested || userPaused || sessionPaused || timestamp > maximumTimestamp-recording::MinimumFrameDelayNanoseconds) {
                    slots[selected].status = SlotStatus::Free;
                    continue;
                }
            }
            const auto pixels = scaled ? scaled->Pixels() : surface.Pixels();
            const BitmapView current(width,height,pixels);
            const auto rectangle = hasPrevious ? recording::ChangedRectangle(current,previous.View()) : std::optional<RECT>{{0,0,width,height}};
            if (!rectangle || (hasPrevious && timestamp-lastSubmitted < recording::MinimumFrameDelayNanoseconds)) {
                std::lock_guard lock(mutex);
                if (!rectangle) { ++result.unchangedTicks; } else { ++result.coalescedTicks; }
                slots[selected].status = SlotStatus::Free;
                continue;
            }
            auto& slot = slots[selected];
            recording::CopyRectangle(current,*rectangle,slot.current);
            if (hasPrevious) { recording::CopyRectangle(previous.View(),*rectangle,slot.previous); }
            else { slot.previous.clear(); }
            for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) { previous.Pixels()[pixel] = pixels[pixel] | 0xFF000000U; }
            hasPrevious = true;
            lastSubmitted = timestamp;
            {
                std::lock_guard lock(mutex);
                slot.rectangle = *rectangle;
                slot.timestamp = timestamp;
                slot.sequence = submitted++;
                slot.status = SlotStatus::Queued;
                const auto queued = static_cast<std::uint64_t>(std::count_if(slots.begin(),slots.end(),
                    [](const FrameSlot& candidate) { return candidate.status == SlotStatus::Queued; }));
                result.peakQueuedFrames = std::max(result.peakQueuedFrames,queued);
            }
            changed.notify_all();
        }
    }

    void Run() noexcept {
        try {
            TemporaryRecording temporary;
            slots.resize(plan.slotCount);
            const auto pixels = static_cast<std::size_t>(width)*static_cast<std::size_t>(height);
            for (auto& slot : slots) { slot.current.reserve(pixels); slot.previous.reserve(pixels); }
            std::thread writer;
            std::vector<std::thread> workers;
            workers.reserve(plan.workerCount);
            {
                const JoinThread joinWriter(writer);
                const JoinWorkers joinWorkers(workers);
                try {
                    writer = std::thread([&] { WriteLoop(temporary.path); });
                    for (unsigned worker = 0; worker < plan.workerCount; ++worker) { workers.emplace_back([this] { EncodeLoop(); }); }
                    CaptureLoop();
                } catch (...) { Fail(); }
                {
                    std::lock_guard lock(mutex);
                    captureDone = true;
                    if (!stopRequested) { stopTimestamp = timeline.Elapsed(RelativeNow()); stopRequested = true; }
                }
                changed.notify_all();
            }
            if (failure) { std::rethrow_exception(failure); }
            {
                std::lock_guard lock(mutex);
                result.cancelled = cancelled;
                if (!cancelled && result.frameCount != 0) { result.path = temporary.path; temporary.keep = true; }
            }
        } catch (const std::exception& error) {
            try { result.error = ExceptionMessage(error); } catch (...) { }
            std::lock_guard lock(mutex);
            result.cancelled = cancelled;
        } catch (...) {
            try { result.error = L"Unexpected recording failure"; } catch (...) { }
        }
        // All capture/encoder/writer work has ended before notifying the UI.
        finished.store(true,std::memory_order_release);
        NotifyCompletion();
    }
};

Recorder::Recorder(RECT bounds, Config::Gif options, HWND completionOwner, UINT completionMessage, bool english)
    : implementation_(std::make_unique<Implementation>(bounds,options,completionOwner,completionMessage,english)) {}
Recorder::~Recorder() = default;
void Recorder::Start() { implementation_->Start(); }
void Recorder::Stop(bool cancel) noexcept { implementation_->Stop(cancel); }
void Recorder::Pause(bool paused) noexcept { implementation_->Pause(paused,false); }
bool Recorder::Finished() const noexcept { return implementation_->finished.load(std::memory_order_acquire); }
std::optional<RecordingResult> Recorder::TakeResult() {
    if (!Finished() || implementation_->resultTaken) { return std::nullopt; }
    if (implementation_->coordinator.joinable()) { implementation_->coordinator.join(); }
    if (implementation_->cancelled) {
        implementation_->result.cancelled = true;
        if (!implementation_->result.path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(implementation_->result.path,ignored);
            implementation_->result.path.clear();
        }
    }
    implementation_->resultTaken = true;
    return std::move(implementation_->result);
}

void CleanOldRecordings() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (error) { return; }
    const auto cutoff = std::filesystem::file_time_type::clock::now()-std::chrono::hours(24);
    recording::CleanOldFiles(temporary/L"VisPick",cutoff);
}

} // namespace vispick
