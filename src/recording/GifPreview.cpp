#include "recording/GifPreview.h"
#include "recording/FrameChange.h"
#include "recording/RecordingFiles.h"
#include "gif/GifReader.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/Gdi.h"
#include "platform/Window.h"

#include <Windowsx.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iomanip>
#include <mutex>
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
    bool prefetched = false;
    std::vector<std::uint32_t> prefetchedPixels;
    std::uint16_t prefetchedDelay = 0;
    std::optional<std::filesystem::path> saveDestination;
    bool saveComplete = false;
    std::exception_ptr saveFailure;
    std::exception_ptr playbackFailure;
    std::vector<std::uint32_t> displayedPixels;
    bool paused = false;
    bool modal = false;
    bool saving = false;
    bool resumeAfterSave = false;
    bool playbackUnavailable = false;
    bool closed = false;
    bool ownsTemporary = true;
    int dpi = 96;
    UniqueGdiObject<HFONT> interfaceFont;
    PreviewClock::time_point deadline;
    PreviewClock::duration remaining{};
    std::wstring information;
    std::wstring saveError;

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
            owner == nullptr || closedMessage < WM_APP || closedMessage > 0xBFFF) { throw std::invalid_argument("Invalid GIF preview result or callback"); }
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

    [[nodiscard]] bool Busy() const noexcept { return modal || saving; }

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
        unsigned nextFrame = 0;
        bool decodingDone = false;
        for (;;) {
            std::optional<std::filesystem::path> destination;
            {
                std::unique_lock lock(workerMutex);
                workerChanged.wait(lock,[&] { return stopWorker || saveDestination || (!prefetched && !decodingDone); });
                // A committed UI save request finishes even during shutdown.
                if (stopWorker && !saveDestination) { return; }
                destination = std::exchange(saveDestination,std::nullopt);
            }
            if (destination) {
                reader.reset(); // WIC may hold a non-delete-sharing file handle.
                std::exception_ptr failure;
                try { recording::CommitFile(result.path,*destination); }
                catch (...) { failure = std::current_exception(); }
                if (!failure) { decodingDone = true; }
                {
                    const std::lock_guard lock(workerMutex);
                    saveFailure = failure;
                    saveComplete = true;
                }
            } else {
                try {
                    if (!reader) { reader = std::make_unique<gif::GifReader>(result.path); }
                    const auto& document = reader->Metadata();
                    if (document.width != static_cast<unsigned>(result.width) || document.height != static_cast<unsigned>(result.height) ||
                        document.frameCount != result.frameCount) { throw std::runtime_error("Recorded GIF metadata does not match completion result"); }
                    if (nextFrame == document.frameCount) { canvas.reset(); nextFrame = 0; }
                    if (!canvas) { canvas.emplace(document); }
                    const auto frame = reader->ReadFrame(nextFrame++);
                    canvas->Compose(frame);
                    if (frame.metadata.delayCentiseconds < recording::MinimumFrameDelayCentiseconds) {
                        throw std::runtime_error("Recorded GIF contains an invalid frame delay");
                    }
                    // While prefetched is false, the worker exclusively owns this buffer.
                    prefetchedPixels.assign(canvas->Pixels().begin(),canvas->Pixels().end());
                    decodingDone = document.frameCount == 1;
                    const std::lock_guard lock(workerMutex);
                    prefetchedDelay = frame.metadata.delayCentiseconds;
                    prefetched = true;
                } catch (...) {
                    reader.reset();
                    decodingDone = true;
                    const std::lock_guard lock(workerMutex);
                    playbackFailure = std::current_exception();
                }
            }
            PostMessageW(window,WorkerUpdated,0,0);
        }
    }

    int Scale(int logical) const noexcept { return MulDiv(logical,dpi,96); }
    int FooterHeight() const noexcept { return Scale(66); }
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
    RECT SaveButton() const noexcept {
        RECT client{}; GetClientRect(Handle(),&client);
        return {client.right-Scale(190),client.bottom-Scale(36),client.right-Scale(100),client.bottom-Scale(6)};
    }
    RECT DiscardButton() const noexcept {
        const auto save = SaveButton();
        const LONG width = save.right-save.left;
        return {save.right+Scale(10),save.top,save.right+Scale(10)+width,save.bottom};
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
            static_cast<double>(std::max(1,availableHeight-Scale(90)))/result.height});
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
    void Schedule() {
        if (paused || closed || !Handle()) { return; }
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-PreviewClock::now()).count();
        const auto milliseconds = (std::max<std::int64_t>(0,nanoseconds)+999999)/1000000;
        const UINT due = static_cast<UINT>(std::clamp<std::int64_t>(milliseconds,1,60000));
        WakeWithin(due);
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
                saveError = (english ? L"Save failed. " : L"保存失败。") + ExceptionMessage(error);
            } catch (...) { saveError = english ? L"Could not save the recording." : L"无法保存录制。"; }
            if (resumeAfterSave) { SetPaused(false); }
            InvalidateRect(Handle(),nullptr,FALSE);
            PostMessageW(owner,closedMessage,0,0);
            return;
        }
        const auto now = PreviewClock::now();
        const bool first = displayedPixels.empty();
        bool presented = false;
        std::exception_ptr failure;
        {
            const std::lock_guard lock(workerMutex);
            failure = std::exchange(playbackFailure,{});
            if (!failure && (paused || playbackUnavailable || (!first && result.frameCount == 1))) { return; }
            if (!failure && prefetched && (first || now >= deadline)) {
                displayedPixels.swap(prefetchedPixels);
                if (first) { deadline = now; }
                deadline += std::chrono::milliseconds(prefetchedDelay*10);
                prefetched = false;
                presented = true;
            }
        }
        if (failure) {
            playbackUnavailable = true;
            InvalidateRect(Handle(),nullptr,FALSE);
            std::rethrow_exception(failure);
        }
        if (presented) {
            workerChanged.notify_one();
            InvalidateRect(Handle(),nullptr,FALSE);
        }
        if (displayedPixels.empty() || (!presented && now >= deadline)) { WakeWithin(50); }
        else if (result.frameCount != 1) { Schedule(); }
    }
    void SetPaused(bool pause) {
        if (pause == paused || closed) { return; }
        if (pause) { remaining = std::max(PreviewClock::duration::zero(),deadline-PreviewClock::now()); KillTimer(Handle(),PlaybackTimer); }
        else { deadline = PreviewClock::now()+remaining; }
        paused = pause;
        if (!pause && !playbackUnavailable) { Schedule(); }
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
        const bool resume = !paused;
        SetPaused(true);
        const BusyScope busy(*this);
        std::optional<std::filesystem::path> destination;
        try { destination = chooseSavePath(Handle(),result.width,result.height); }
        catch (...) { if (resume) { SetPaused(false); } throw; }
        if (!destination) { if (resume) { SetPaused(false); } return; }
        WakeWithin(50); // Also recover if the worker's window message cannot be queued.
        saveError.clear();
        resumeAfterSave = resume;
        saving = true;
        {
            const std::lock_guard lock(workerMutex);
            saveDestination = std::move(*destination);
        }
        workerChanged.notify_one();
        InvalidateRect(Handle(),nullptr,FALSE);
    }
    void Paint(HDC context) {
        RECT client{}; GetClientRect(Handle(),&client);
        SetDCBrushColor(context,RGB(28,30,35));
        FillRect(context,&client,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const int imageHeight = std::max(1,static_cast<int>(client.bottom)-FooterHeight());
        if (!displayedPixels.empty()) {
            const double scale = std::min(static_cast<double>(std::max(1L,client.right))/result.width,static_cast<double>(imageHeight)/result.height);
            const int shownWidth = std::max(1,static_cast<int>(result.width*scale));
            const int shownHeight = std::max(1,static_cast<int>(result.height*scale));
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = result.width;
            bitmap.bmiHeader.biHeight = -result.height;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(context,COLORONCOLOR);
            StretchDIBits(context,(client.right-shownWidth)/2,(imageHeight-shownHeight)/2,shownWidth,shownHeight,
                0,0,result.width,result.height,displayedPixels.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
        }
        const SelectObjectGuard font(context,interfaceFont.Get());
        SetBkMode(context,TRANSPARENT);
        SetTextColor(context,RGB(235,235,240));
        RECT informationArea{Scale(12),imageHeight+Scale(4),client.right-Scale(12),imageHeight+Scale(26)};
        const wchar_t* status = information.c_str();
        if (saving) { status = english ? L"Saving…" : L"正在保存…"; }
        else if (!saveError.empty()) { status = saveError.c_str(); }
        else if (playbackUnavailable) { status = english ? L"Preview unavailable" : L"无法预览"; }
        DrawTextW(context,status,-1,&informationArea,
            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        auto save = SaveButton();
        auto discard = DiscardButton();
        SetDCBrushColor(context,RGB(60,65,75));
        FillRect(context,&save,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        FillRect(context,&discard,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        DrawTextW(context,english ? L"Save..." : L"保存…",-1,&save,DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(context,english ? L"Discard" : L"放弃",-1,&discard,DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        try {
            switch (message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const auto context = BeginPaint(Handle(),&paint);
                try { Paint(context); }
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
                if (parameter == VK_SPACE) { if (!Busy()) { SetPaused(!paused); } return 0; }
                if (parameter == VK_ESCAPE) { Discard(); return 0; }
                if (parameter == 'S' && (GetKeyState(VK_CONTROL)&0x8000) != 0) { Save(); return 0; }
                break;
            case WM_LBUTTONUP: {
                const POINT point{GET_X_LPARAM(data),GET_Y_LPARAM(data)};
                const auto save = SaveButton();
                const auto discard = DiscardButton();
                if (PtInRect(&save,point)) { Save(); }
                else if (PtInRect(&discard,point)) { Discard(); }
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
