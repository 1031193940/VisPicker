#include "recording/GifPreview.h"
#include "recording/FrameChange.h"
#include "recording/RecordingFiles.h"
#include "gif/GifReader.h"
#include "imaging/Clipboard.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/FileCommit.h"
#include "platform/Gdi.h"
#include "platform/Window.h"

#include <Windowsx.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace vispick {
namespace {
using PreviewClock = std::chrono::steady_clock;
constexpr UINT WorkerUpdated = WM_APP + 1;
constexpr UINT_PTR PlaybackTimer = 1;

struct FrameTiming {
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

struct TimelineEntry {
    std::int64_t end;
    unsigned restartFrame;
};

class PreparedGifCopy final {
public:
    explicit PreparedGifCopy(const std::filesystem::path& source)
        : data_(StageFile(source, file_.path)) {}

    [[nodiscard]] GifClipboardFormats Publish(HWND owner) {
        const auto formats = data_.Publish(owner);
        file_.keep = formats.fileDrop;
        // Clipboard publication has completed. Previous file drops may now be
        // pruned when old, while startup leaves the current paste source alone.
        try {
            recording::CleanOldFiles(file_.path.parent_path(),
                std::filesystem::file_time_type::clock::now() - std::chrono::hours(24),
                recording::TemporaryGifKind::Clipboard, file_.path);
        } catch (...) { /* Best-effort cleanup must not change the copy result. */ }
        return formats;
    }

private:
    static const std::filesystem::path& StageFile(const std::filesystem::path& source,
                                                  const std::filesystem::path& destination) {
        std::filesystem::copy_file(ExtendedFilePath(source), ExtendedFilePath(destination),
            std::filesystem::copy_options::overwrite_existing);
        return destination;
    }

    recording::TemporaryGif file_{recording::TemporaryGifKind::Clipboard};
    GifClipboardData data_;
};

std::wstring FormatTime(std::int64_t milliseconds) {
    const auto seconds = milliseconds / 1000;
    const auto minuteSeconds = seconds % 60;
    return std::to_wstring(seconds / 60) + (minuteSeconds < 10 ? L":0" : L":") +
        std::to_wstring(minuteSeconds) + L"." + std::to_wstring(milliseconds / 100 % 10);
}
}

struct GifPreview::Implementation final : Window {
    RecordingResult result;
    HWND owner;
    UINT closedMessage;
    GifSavePathCallback chooseSavePath;
    bool english;
    // The worker owns WIC and GIF composition. One prefetched frame is shared
    // with the UI, which draws only its own displayed buffer.
    std::thread worker;
    std::mutex workerMutex;
    std::condition_variable workerChanged;
    bool stopWorker = false;
    std::optional<FrameTiming> prefetchedTiming;
    std::vector<std::uint32_t> prefetchedPixels;
    std::optional<std::int64_t> seekRequest;
    std::optional<std::filesystem::path> saveDestination;
    bool copyRequested = false;
    std::unique_ptr<PreparedGifCopy> preparedCopy;
    std::exception_ptr copyFailure;
    bool saveComplete = false;
    std::exception_ptr saveFailure;
    std::exception_ptr playbackFailure;
    std::vector<std::uint32_t> displayedPixels;
    FrameTiming displayedTiming;
    std::int64_t duration = 0;
    std::int64_t position = 0;
    PreviewClock::time_point playbackAnchor;
    std::optional<std::int64_t> pendingSeek;
    std::optional<std::int64_t> scrubPosition;
    bool resumeAfterScrub = false;
    bool paused = false;
    bool modal = false;
    bool saving = false;
    bool copying = false;
    bool resumeAfterOutput = false;
    bool playbackUnavailable = false;
    bool closed = false;
    bool ownsTemporary = true;
    int dpi = 96;
    UniqueGdiObject<HFONT> interfaceFont;
    std::optional<DibSection> paintBuffer;
    std::wstring durationText;
    std::wstring information;
    std::wstring outputStatus;

    struct BusyScope {
        Implementation& preview;
        bool previous;
        explicit BusyScope(Implementation& value) : preview(value), previous(value.modal) { preview.modal = true; }
        ~BusyScope() {
            preview.modal = previous;
            PostMessageW(preview.owner,preview.closedMessage,0,0);
        }
        BusyScope(const BusyScope&) = delete;
        BusyScope& operator=(const BusyScope&) = delete;
    };

    Implementation(RecordingResult recording, const std::filesystem::path& temporary, HWND parent, UINT message,
                   GifSavePathCallback callback, bool englishLanguage)
        : result(std::move(recording)), owner(parent), closedMessage(message), chooseSavePath(std::move(callback)), english(englishLanguage) {
        result.path = temporary;
        if (result.path.empty() || result.cancelled || !result.error.empty() || !chooseSavePath || result.width <= 0 ||
            result.height <= 0 || result.frameCount == 0 || result.durationCentiseconds == 0 ||
            result.durationCentiseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10) ||
            owner == nullptr || closedMessage < WM_APP || closedMessage > 0xBFFF) { throw std::invalid_argument("Invalid GIF preview result or callback"); }
        duration = static_cast<std::int64_t>(result.durationCentiseconds * 10);
        durationText = FormatTime(duration);
        std::wostringstream description;
        description << result.width << L"×" << result.height << L" · " << std::fixed << std::setprecision(1)
            << static_cast<double>(result.durationCentiseconds)/100.0 << (english ? L" s · " : L" 秒 · ") << result.frameCount << (english ? L" frames · " : L" 帧 · ")
            << static_cast<double>(std::filesystem::file_size(result.path))/(1024.0*1024.0) << L" MB";
        information = description.str();
    }
    ~Implementation() override {
        JoinWorker();
        Destroy(); // Before state read by the window procedure is destroyed.
        if (ownsTemporary) { std::error_code ignored; std::filesystem::remove(result.path,ignored); }
    }

    [[nodiscard]] bool Busy() const noexcept { return modal || saving || copying; }

    void JoinWorker() noexcept {
        {
            const std::lock_guard lock(workerMutex);
            stopWorker = true;
        }
        workerChanged.notify_one();
        if (worker.joinable()) { worker.join(); }
    }

    void PrepareFrames(HWND window) noexcept {
        std::unique_ptr<gif::GifReader> reader;
        std::optional<gif::GifCanvas> canvas;
        std::vector<TimelineEntry> timeline;
        std::optional<std::int64_t> activeSeek;
        unsigned nextFrame = 0;
        bool decodingDone = false;
        const auto interrupted = [&] {
            const std::lock_guard lock(workerMutex);
            return stopWorker || seekRequest || (activeSeek && (saveDestination || copyRequested));
        };
        for (;;) {
            std::optional<std::filesystem::path> destination;
            bool copy = false;
            {
                std::unique_lock lock(workerMutex);
                workerChanged.wait(lock,[&] { return stopWorker || saveDestination || copyRequested || seekRequest ||
                    (!prefetchedTiming && (!decodingDone || activeSeek)); });
                // A committed UI save request finishes even during shutdown.
                if (stopWorker && !saveDestination) { return; }
                destination = std::exchange(saveDestination,std::nullopt);
                copy = std::exchange(copyRequested,false);
                if (seekRequest) { activeSeek = std::exchange(seekRequest,std::nullopt); }
            }
            if (destination) {
                reader.reset(); // WIC may hold a non-delete-sharing file handle.
                std::exception_ptr failure;
                try { recording::CommitFile(result.path,*destination); }
                catch (...) { failure = std::current_exception(); }
                if (!failure) { decodingDone = true; activeSeek.reset(); }
                {
                    const std::lock_guard lock(workerMutex);
                    saveFailure = failure;
                    saveComplete = true;
                }
            } else if (copy) {
                std::unique_ptr<PreparedGifCopy> prepared;
                std::exception_ptr failure;
                try { prepared = std::make_unique<PreparedGifCopy>(result.path); }
                catch (...) { failure = std::current_exception(); }
                const std::lock_guard lock(workerMutex);
                preparedCopy = std::move(prepared);
                copyFailure = failure;
            } else {
                try {
                    if (!reader) { reader = std::make_unique<gif::GifReader>(result.path); }
                    const auto& document = reader->Metadata();
                    if (document.width != static_cast<unsigned>(result.width) || document.height != static_cast<unsigned>(result.height) ||
                        document.frameCount != result.frameCount) { throw std::runtime_error("Recorded GIF metadata does not match completion result"); }
                    const auto indexFrame = [&](const gif::GifFrameMetadata& frame) {
                        const auto begin = timeline.empty() ? 0 : timeline.back().end;
                        const auto delay = static_cast<std::int64_t>(frame.delayCentiseconds) * 10;
                        if (frame.delayCentiseconds < recording::MinimumFrameDelayCentiseconds || delay > duration - begin) {
                            throw std::runtime_error("Recorded GIF contains an invalid frame delay");
                        }
                        const auto frameIndex = static_cast<unsigned>(timeline.size());
                        const bool restart = frame.left == 0 && frame.top == 0 && frame.width == document.width &&
                            frame.height == document.height && frame.transparentColorIndex < 0 && frame.disposal != gif::DisposalMethod::Previous;
                        timeline.push_back({begin + delay, restart || timeline.empty() ? frameIndex : timeline.back().restartFrame});
                    };
                    unsigned targetFrame = nextFrame == document.frameCount ? 0 : nextFrame;
                    if (activeSeek) {
                        // Index the delays without decoding pixels. Partial and transparent frames
                        // still require composition from a known canvas or an opaque full frame.
                        const auto targetTime = std::min(*activeSeek, duration - 1);
                        while (timeline.size() < document.frameCount && (timeline.empty() || timeline.back().end <= targetTime)) {
                            if (interrupted()) { break; }
                            indexFrame(reader->ReadFrameMetadata(static_cast<unsigned>(timeline.size())));
                        }
                        if (interrupted()) { continue; }
                        const auto frame = std::upper_bound(timeline.begin(),timeline.end(),targetTime,
                            [](std::int64_t time, const TimelineEntry& entry) { return time < entry.end; });
                        if (frame == timeline.end()) { throw std::runtime_error("Recorded GIF duration does not match completion result"); }
                        targetFrame = static_cast<unsigned>(frame - timeline.begin());
                    }
                    const unsigned restartFrame = targetFrame < timeline.size() ? timeline[targetFrame].restartFrame : 0;
                    if (!canvas || nextFrame > targetFrame + 1 || nextFrame < restartFrame || (!activeSeek && targetFrame == 0)) {
                        canvas.emplace(document);
                        nextFrame = restartFrame;
                    }
                    while (nextFrame <= targetFrame) {
                        if (interrupted()) { break; }
                        const auto frame = reader->ReadFrame(nextFrame);
                        if (nextFrame == timeline.size()) { indexFrame(frame.metadata); }
                        canvas->Compose(frame);
                        ++nextFrame;
                    }
                    if (interrupted()) { continue; }
                    const auto end = timeline.at(targetFrame).end;
                    if (targetFrame + 1 == document.frameCount && end != duration) {
                        throw std::runtime_error("Recorded GIF duration does not match completion result");
                    }
                    // With no published timing, the worker exclusively owns this buffer.
                    prefetchedPixels.assign(canvas->Pixels().begin(),canvas->Pixels().end());
                    const std::lock_guard lock(workerMutex);
                    if (stopWorker || seekRequest || (activeSeek && (saveDestination || copyRequested))) { continue; }
                    prefetchedTiming = FrameTiming{targetFrame == 0 ? 0 : timeline[targetFrame - 1].end, end};
                    activeSeek.reset();
                    decodingDone = document.frameCount == 1;
                } catch (...) {
                    reader.reset();
                    decodingDone = true;
                    activeSeek.reset();
                    const std::lock_guard lock(workerMutex);
                    seekRequest.reset();
                    playbackFailure = std::current_exception();
                }
            }
            PostMessageW(window,WorkerUpdated,0,0);
        }
    }

    int Scale(int logical) const noexcept { return MulDiv(logical,dpi,96); }
    int FooterHeight() const noexcept { return Scale(100); }
    void UpdateDpi(UINT requested) {
        const auto version = GetWindowsVersion();
        const bool hasUiFont = version.major > 6 || (version.major == 6 && version.minor >= 2);
        const int targetDpi = static_cast<int>(requested);
        const auto font = CreateFontW(-MulDiv(14,targetDpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,
            english ? L"Segoe UI" : (hasUiFont ? L"Microsoft YaHei UI" : L"Microsoft YaHei"));
        if (!font) { throw std::runtime_error("Cannot create GIF preview font"); }
        interfaceFont.Reset(font);
        dpi = targetDpi;
    }
    SIZE OuterSize(int clientWidth, int clientHeight) const {
        const auto bounds = AdjustWindowBoundsForDpi({0,0,clientWidth,clientHeight},WS_OVERLAPPEDWINDOW,0,static_cast<UINT>(dpi));
        return {bounds.right-bounds.left,bounds.bottom-bounds.top};
    }
    struct ActionButtons { RECT clipboard; RECT save; RECT discard; };
    ActionButtons LayoutActions() const noexcept {
        RECT client{}; GetClientRect(Handle(),&client);
        const LONG top = client.bottom-Scale(36);
        const LONG bottom = client.bottom-Scale(6);
        const LONG right = client.right-Scale(10);
        const LONG discardLeft = right-Scale(78);
        const LONG saveRight = discardLeft-Scale(8);
        return {{Scale(10),top,Scale(164),bottom}, {saveRight-Scale(76),top,saveRight,bottom},
            {discardLeft,top,right,bottom}};
    }
    RECT ProgressArea() const noexcept {
        RECT client{}; GetClientRect(Handle(),&client);
        return {Scale(12),client.bottom-FooterHeight()+Scale(4),client.right-Scale(12),client.bottom-FooterHeight()+Scale(30)};
    }
    RECT ProgressTrack() const noexcept {
        auto area = ProgressArea();
        area.left += Scale(4);
        area.right -= Scale(112);
        return area;
    }
    void InvalidateProgress() noexcept {
        auto area = ProgressArea();
        area.bottom += Scale(29);
        InvalidateRect(Handle(),&area,FALSE);
    }
    void Show() {
        if (Handle()) { ShowWindow(Handle(),SW_SHOWNORMAL); return; }
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor)) { throw std::runtime_error("Cannot locate GIF preview monitor"); }
        UINT initialDpi = DpiForWindow(owner);
        for (const auto& display : EnumerateMonitors()) {
            if (EqualRect(&display.bounds,&monitor.rcMonitor)) { initialDpi = display.dpiX; break; }
        }
        UpdateDpi(initialDpi);
        const int availableWidth = (monitor.rcWork.right-monitor.rcWork.left)*8/10;
        const int availableHeight = (monitor.rcWork.bottom-monitor.rcWork.top)*8/10;
        const double scale = std::min({1.0,static_cast<double>(availableWidth)/result.width,
            static_cast<double>(std::max(1,availableHeight-FooterHeight()-Scale(24)))/result.height});
        const int width = std::max(Scale(360),static_cast<int>(result.width*scale));
        const int height = std::max(Scale(180),static_cast<int>(result.height*scale)+FooterHeight());
        const auto outer = OuterSize(width,height);
        const LONG left = monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-outer.cx)/2;
        const LONG top = monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-outer.cy)/2;
        Create({L"VisPick.GifPreview",0,IDC_ARROW},english ? L"GIF Preview – VisPick" : L"GIF 预览 – VisPick",WS_OVERLAPPEDWINDOW,0,
            {left,top,left+outer.cx,top+outer.cy},WindowActivation::Interactive);
        // A normal unowned top-level window remains visible in the taskbar.
        worker = std::thread([this, window = Handle()] { PrepareFrames(window); });
        WakeWithin(50);
        ShowWindow(Handle(),SW_SHOWNORMAL);
        // A tray process may have been launched with STARTF_USESHOWWINDOW /
        // SW_HIDE. An explicitly requested preview must still become visible.
        if (!IsWindowVisible(Handle()) && !SetWindowPos(Handle(),HWND_TOP,0,0,0,0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Cannot show GIF preview");
        }
    }
    void WakeWithin(UINT milliseconds) {
        if (!SetTimer(Handle(),PlaybackTimer,milliseconds,nullptr)) { throw std::runtime_error("Cannot schedule GIF playback"); }
    }
    [[nodiscard]] std::int64_t PlaybackPosition(PreviewClock::time_point now) const noexcept {
        if (paused || pendingSeek || displayedPixels.empty()) { return position; }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now-playbackAnchor).count();
        return position + std::clamp<std::int64_t>(elapsed,0,displayedTiming.end-position);
    }
    void Schedule() {
        if (closed || playbackUnavailable || scrubPosition || !Handle()) { return; }
        if (displayedPixels.empty() || pendingSeek) { WakeWithin(50); }
        else if (!paused) {
            const auto untilFrame = displayedTiming.end - PlaybackPosition(PreviewClock::now());
            WakeWithin(untilFrame > 0 ? static_cast<UINT>(std::min<std::int64_t>(untilFrame,100)) : 20U);
        }
    }
    void Advance() {
        if (modal || closed) { return; }
        KillTimer(Handle(),PlaybackTimer);
        if (saving) {
            std::exception_ptr failure;
            bool complete = false;
            {
                const std::lock_guard lock(workerMutex);
                complete = std::exchange(saveComplete,false);
                if (complete) { failure = std::exchange(saveFailure,{}); }
            }
            if (!complete) { WakeWithin(50); return; }
            saving = false;
            if (!failure) { ownsTemporary = false; Close(); return; }
            try { std::rethrow_exception(failure); }
            catch (const std::exception& error) {
                outputStatus = (english ? L"Save failed. " : L"保存失败。") + ExceptionMessage(error);
            } catch (...) { outputStatus = english ? L"Could not save the recording." : L"无法保存录制。"; }
            if (resumeAfterOutput) { SetPaused(false); }
            InvalidateRect(Handle(),nullptr,FALSE);
            PostMessageW(owner,closedMessage,0,0);
            return;
        }
        if (copying) {
            CompleteCopy();
            return;
        }
        const auto now = PreviewClock::now();
        const bool first = displayedPixels.empty();
        const bool frameDue = !paused && !pendingSeek && PlaybackPosition(now) >= displayedTiming.end;
        bool presented = false;
        std::exception_ptr failure;
        {
            const std::lock_guard lock(workerMutex);
            failure = std::exchange(playbackFailure,{});
            if (!failure && !scrubPosition && prefetchedTiming && (first || pendingSeek || frameDue)) {
                displayedPixels.swap(prefetchedPixels);
                playbackAnchor = (first || pendingSeek) ? now : playbackAnchor + std::chrono::milliseconds(displayedTiming.end-position);
                displayedTiming = *prefetchedTiming;
                position = pendingSeek.value_or(displayedTiming.begin);
                pendingSeek.reset();
                prefetchedTiming.reset();
                presented = true;
            }
        }
        if (failure) {
            playbackUnavailable = true;
            pendingSeek.reset();
            InvalidateRect(Handle(),nullptr,FALSE);
            std::rethrow_exception(failure);
        }
        if (presented) {
            workerChanged.notify_one();
            InvalidateRect(Handle(),nullptr,FALSE);
        } else if (!paused && !pendingSeek && !scrubPosition) {
            if (frameDue && !first && result.frameCount == 1) { position = 0; playbackAnchor = now; }
            InvalidateProgress();
        }
        Schedule();
    }
    void SetPaused(bool pause) {
        if (pause == paused || closed) { return; }
        const auto now = PreviewClock::now();
        if (pause) { position = PlaybackPosition(now); KillTimer(Handle(),PlaybackTimer); }
        else { playbackAnchor = now; }
        paused = pause;
        if (!pause) { Schedule(); }
        InvalidateProgress();
    }
    [[nodiscard]] std::int64_t SeekPosition(LONG x) const noexcept {
        const auto track = ProgressTrack();
        const auto width = std::max(1L,track.right-track.left-1);
        const auto offset = std::clamp(x-track.left,0L,width);
        // Split the multiply so even a large valid duration fits in int64_t.
        return (duration / width) * offset + (duration % width) * offset / width;
    }
    void FinishScrub(bool seek) {
        if (!scrubPosition) { return; }
        const auto target = *std::exchange(scrubPosition,std::nullopt);
        if (GetCapture() == Handle()) { ReleaseCapture(); }
        if (seek && !playbackUnavailable) {
            pendingSeek = target;
            {
                const std::lock_guard lock(workerMutex);
                seekRequest = target;
                prefetchedTiming.reset();
            }
            workerChanged.notify_one();
        }
        if (resumeAfterScrub && (!seek || target != duration)) { SetPaused(false); }
        Schedule();
        InvalidateProgress();
    }
    void Close() {
        if (closed) { return; }
        closed = true;
        KillTimer(Handle(),PlaybackTimer);
        JoinWorker();
        Destroy();
        PostMessageW(owner,closedMessage,0,0);
    }
    void Discard(bool closePins = false) {
        if (Busy() || closed) { return; }
        FinishScrub(false);
        const bool resume = !paused;
        SetPaused(true);
        const BusyScope busy(*this);
        const auto question = closePins
            ? (english ? L"Discard this GIF recording, delete its temporary file, and close all pinned images?" : L"放弃这段 GIF 录制、删除临时文件，并关闭所有参考图？")
            : (english ? L"Discard this GIF recording and delete its temporary file?" : L"放弃这段 GIF 录制并删除临时文件？");
        const int answer = MessageBoxW(Handle(),question,L"VisPick",MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer == IDYES) {
            Close();
            // The decoder is closed. A locked file is cleaned on a later startup.
            std::error_code ignored;
            if (std::filesystem::remove(result.path,ignored)) { ownsTemporary = false; }
        } else if (resume) { SetPaused(false); }
    }
    void Save() {
        if (Busy() || closed) { return; }
        FinishScrub(false);
        const bool resume = !paused;
        SetPaused(true);
        const BusyScope busy(*this);
        std::optional<std::filesystem::path> destination;
        try { destination = chooseSavePath(Handle(),result.width,result.height); }
        catch (...) { if (resume) { SetPaused(false); } throw; }
        if (!destination) { if (resume) { SetPaused(false); } return; }
        WakeWithin(50); // Also recover if the worker's window message cannot be queued.
        outputStatus.clear();
        resumeAfterOutput = resume;
        saving = true;
        {
            const std::lock_guard lock(workerMutex);
            saveDestination = std::move(*destination);
        }
        workerChanged.notify_one();
        InvalidateRect(Handle(),nullptr,FALSE);
    }
    void Copy() {
        if (Busy() || closed) { return; }
        FinishScrub(false);
        resumeAfterOutput = !paused;
        SetPaused(true);
        WakeWithin(50);
        outputStatus.clear();
        copying = true;
        {
            const std::lock_guard lock(workerMutex);
            copyRequested = true;
        }
        workerChanged.notify_one();
        InvalidateProgress();
    }
    void CompleteCopy() {
        std::unique_ptr<PreparedGifCopy> prepared;
        std::exception_ptr failure;
        {
            const std::lock_guard lock(workerMutex);
            prepared = std::move(preparedCopy);
            failure = std::exchange(copyFailure,{});
        }
        if (!prepared && !failure) { WakeWithin(50); return; }
        try {
            if (failure) { std::rethrow_exception(failure); }
            const auto formats = prepared->Publish(Handle());
            outputStatus = formats.animation ? (english ? L"GIF copied to clipboard" : L"GIF 已复制到剪贴板") :
                (english ? L"GIF copied as a file" : L"已将 GIF 文件复制到剪贴板");
        } catch (const std::exception& error) {
            outputStatus = (english ? L"Copy failed. " : L"复制失败。") + ExceptionMessage(error);
        } catch (...) { outputStatus = english ? L"Could not copy the recording." : L"无法复制录制。"; }
        copying = false;
        if (resumeAfterOutput) { SetPaused(false); }
        Schedule();
        InvalidateProgress();
        PostMessageW(owner,closedMessage,0,0);
    }
    void PaintProgress(HDC context) {
        const auto area = ProgressArea();
        const auto track = ProgressTrack();
        const auto shownPosition = scrubPosition.value_or(pendingSeek.value_or(PlaybackPosition(PreviewClock::now())));
        const auto width = std::max(1L,track.right-track.left-1);
        const auto marker = track.left+static_cast<LONG>(static_cast<double>(shownPosition) / static_cast<double>(duration) * width);
        const LONG centerY = (track.top+track.bottom)/2;
        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        const RECT rail{track.left,centerY-Scale(2),track.right,centerY+Scale(2)};
        SetDCBrushColor(context,RGB(72,76,86));
        FillRect(context,&rail,brush);
        const RECT filled{rail.left,rail.top,marker,rail.bottom};
        SetDCBrushColor(context,RGB(99,164,255));
        FillRect(context,&filled,brush);
        if (!playbackUnavailable) {
            const RECT thumb{marker-Scale(3),centerY-Scale(7),marker+Scale(4),centerY+Scale(7)};
            SetDCBrushColor(context,RGB(226,238,255));
            FillRect(context,&thumb,brush);
        }
        RECT timeArea{track.right+Scale(8),area.top,area.right,area.bottom};
        const auto time = FormatTime(shownPosition) + L" / " + durationText;
        DrawTextW(context,time.c_str(),-1,&timeArea,DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
    }
    void DrawClient(HDC context, const RECT& client, const RECT& dirty) {
        SetDCBrushColor(context,RGB(28,30,35));
        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        const auto fillBackground = [&](const RECT& bounds) {
            RECT clipped{};
            if (IntersectRect(&clipped,&dirty,&bounds)) { FillRect(context,&clipped,brush); }
        };
        const int imageHeight = std::max(1,static_cast<int>(client.bottom)-FooterHeight());
        RECT intersection{};
        if (displayedPixels.empty()) { fillBackground(client); }
        else {
            const double scale = std::min(static_cast<double>(std::max(1L,client.right))/result.width,static_cast<double>(imageHeight)/result.height);
            const int shownWidth = std::max(1,static_cast<int>(result.width*scale));
            const int shownHeight = std::max(1,static_cast<int>(result.height*scale));
            const int left = (client.right-shownWidth)/2;
            const int top = (imageHeight-shownHeight)/2;
            const RECT frame{left,top,left+shownWidth,top+shownHeight};
            // Even in the low-memory drawing path, leave the previous image visible until its replacement is ready.
            fillBackground({0,0,client.right,frame.top});
            fillBackground({0,frame.bottom,client.right,client.bottom});
            fillBackground({0,frame.top,frame.left,frame.bottom});
            fillBackground({frame.right,frame.top,client.right,frame.bottom});
            if (IntersectRect(&intersection,&dirty,&frame)) {
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = result.width;
                bitmap.bmiHeader.biHeight = -result.height;
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(context,COLORONCOLOR);
                StretchDIBits(context,left,top,shownWidth,shownHeight,0,0,result.width,result.height,
                    displayedPixels.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
            }
        }
        const SelectObjectGuard font(context,interfaceFont.Get());
        SetBkMode(context,TRANSPARENT);
        SetTextColor(context,RGB(235,235,240));
        const auto progress = ProgressArea();
        if (IntersectRect(&intersection,&dirty,&progress)) { PaintProgress(context); }
        RECT informationArea{Scale(12),imageHeight+Scale(35),client.right-Scale(12),imageHeight+Scale(58)};
        const wchar_t* status = information.c_str();
        if (saving) { status = english ? L"Saving…" : L"正在保存…"; }
        else if (copying) { status = english ? L"Copying GIF…" : L"正在复制 GIF…"; }
        else if (pendingSeek) { status = english ? L"Seeking…" : L"正在跳转…"; }
        else if (!outputStatus.empty()) { status = outputStatus.c_str(); }
        else if (playbackUnavailable) { status = english ? L"Preview unavailable" : L"无法预览"; }
        DrawTextW(context,status,-1,&informationArea,
            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        auto actions = LayoutActions();
        SetDCBrushColor(context,RGB(60,65,75));
        FillRect(context,&actions.save,brush);
        FillRect(context,&actions.discard,brush);
        SetDCBrushColor(context,RGB(33,90,173));
        FillRect(context,&actions.clipboard,brush);
        DrawTextW(context,english ? L"Save..." : L"保存…",-1,&actions.save,DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(context,english ? L"Discard" : L"放弃",-1,&actions.discard,DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(context,english ? L"Copy to Clipboard" : L"复制到剪贴板",-1,&actions.clipboard,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    void Paint(HDC context, const RECT* update = nullptr) {
        RECT client{};
        GetClientRect(Handle(),&client);
        RECT dirty{};
        if (!IntersectRect(&dirty,&client,update ? update : &client)) { return; }
        if (!update) { DrawClient(context,client,dirty); return; } // WM_PRINTCLIENT supplies its own surface.

        constexpr std::int64_t maximumBufferPixels = 64 * 1024 * 1024 / sizeof(std::uint32_t);
        if (static_cast<std::int64_t>(client.right)*client.bottom > maximumBufferPixels) {
            paintBuffer.reset();
            DrawClient(context,client,dirty);
            return;
        }
        if (!paintBuffer || paintBuffer->Width() != client.right || paintBuffer->Height() != client.bottom) {
            try { paintBuffer.emplace(client.right,client.bottom); }
            catch (const std::exception&) { DrawClient(context,client,dirty); return; }
        }
        // Fill and scale off-screen so intermediate draws cannot flash on the window.
        const auto bufferedContext = paintBuffer->DeviceContext();
        DrawClient(bufferedContext,client,dirty);
        if (!BitBlt(context,dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top,
                    bufferedContext,dirty.left,dirty.top,SRCCOPY)) {
            throw std::runtime_error("Present GIF preview failed");
        }
    }
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        try {
            switch (message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const auto context = BeginPaint(Handle(),&paint);
                try { Paint(context,&paint.rcPaint); }
                catch (...) { EndPaint(Handle(),&paint); throw; }
                EndPaint(Handle(),&paint);
                return 0;
            }
            case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(parameter)); return 0;
            case WM_TIMER:
            case WorkerUpdated: Advance(); return 0;
            case WM_SIZE: InvalidateRect(Handle(),nullptr,FALSE); return 0;
            case WM_GETMINMAXINFO: {
                const auto minimum = OuterSize(Scale(360),Scale(180));
                auto& sizes = *reinterpret_cast<MINMAXINFO*>(data);
                sizes.ptMinTrackSize.x = std::max(sizes.ptMinTrackSize.x,minimum.cx);
                sizes.ptMinTrackSize.y = std::max(sizes.ptMinTrackSize.y,minimum.cy);
                return 0;
            }
            case WM_DPICHANGED: {
                UpdateDpi(HIWORD(parameter));
                const auto& suggested = *reinterpret_cast<const RECT*>(data);
                if (!SetWindowPos(Handle(),nullptr,suggested.left,suggested.top,suggested.right-suggested.left,
                    suggested.bottom-suggested.top,SWP_NOZORDER | SWP_NOACTIVATE)) { throw std::runtime_error("Cannot resize GIF preview for its DPI"); }
                InvalidateRect(Handle(),nullptr,FALSE);
                return 0;
            }
            case WM_CLOSE: Discard(); return 0;
            case WM_KEYDOWN:
                if (parameter == VK_SPACE) { if (!Busy() && !scrubPosition) { SetPaused(!paused); } return 0; }
                if (parameter == VK_ESCAPE) { Discard(); return 0; }
                if (parameter == 'S' && (GetKeyState(VK_CONTROL)&0x8000) != 0) { Save(); return 0; }
                if (parameter == 'C' && (GetKeyState(VK_CONTROL)&0x8000) != 0) {
                    constexpr LPARAM wasAlreadyDown = 1L << 30;
                    if ((data & wasAlreadyDown) == 0) { Copy(); }
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN: {
                const POINT point{GET_X_LPARAM(data),GET_Y_LPARAM(data)};
                const auto track = ProgressTrack();
                if (!Busy() && !closed && !playbackUnavailable && PtInRect(&track,point)) {
                    resumeAfterScrub = !paused;
                    SetPaused(true);
                    scrubPosition = SeekPosition(point.x);
                    SetCapture(Handle());
                    if (GetCapture() != Handle()) { FinishScrub(true); }
                    InvalidateProgress();
                }
                return 0;
            }
            case WM_MOUSEMOVE:
                if (scrubPosition) { scrubPosition = SeekPosition(GET_X_LPARAM(data)); InvalidateProgress(); }
                return 0;
            case WM_CANCELMODE:
            case WM_CAPTURECHANGED:
                FinishScrub(false);
                break;
            case WM_LBUTTONUP: {
                const POINT point{GET_X_LPARAM(data),GET_Y_LPARAM(data)};
                if (scrubPosition) { scrubPosition = SeekPosition(point.x); FinishScrub(true); return 0; }
                const auto actions = LayoutActions();
                if (PtInRect(&actions.clipboard,point)) { Copy(); }
                else if (PtInRect(&actions.save,point)) { Save(); }
                else if (PtInRect(&actions.discard,point)) { Discard(); }
                return 0;
            }
            }
        } catch (const std::exception& error) {
            SetPaused(true);
            const BusyScope busy(*this);
            const auto description = ExceptionMessage(error);
            MessageBoxW(Handle(),description.c_str(),L"VisPick GIF",MB_OK | MB_ICONERROR);
            return 0;
        }
        return Window::OnMessage(message,parameter,data);
    }
};

GifPreview::GifPreview(RecordingResult result, Config::Save, HWND owner,
                       UINT closedMessage, GifSavePathCallback chooseSavePath, bool english)
{
    auto temporary = std::move(result.path);
    try { implementation_ = std::make_unique<Implementation>(std::move(result),temporary,owner,closedMessage,std::move(chooseSavePath),english); }
    catch (...) { std::error_code ignored; std::filesystem::remove(temporary,ignored); throw; }
}
GifPreview::~GifPreview() = default;
void GifPreview::Show() { implementation_->Show(); }
void GifPreview::RequestDiscard(bool closePins) { implementation_->Discard(closePins); }
void GifPreview::PreserveTemporaryForSessionEnd() noexcept {
    implementation_->JoinWorker();
    implementation_->ownsTemporary = false;
    implementation_->paused = true;
    if (implementation_->Handle()) { KillTimer(implementation_->Handle(),PlaybackTimer); }
}
HWND GifPreview::Handle() const noexcept { return implementation_->Handle(); }
bool GifPreview::Closed() const noexcept { return implementation_->closed; }
bool GifPreview::Busy() const noexcept { return implementation_->Busy(); }

} // namespace vispick
