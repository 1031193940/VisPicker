#include "spikes/PatternProcess.h"

#include "platform/Compat.h"
#include "platform/Window.h"
#include "spikes/VisualSupport.h"

#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace vispick {
namespace {

constexpr std::array<std::uint32_t, 8> PatternColors{
    0xFF172333U, 0xFFEEF2F6U, 0xFF3689E5U, 0xFF34B788U,
    0xFFF0BB45U, 0xFFD85C62U, 0xFF946EE0U, 0xFF54BBC8U};

std::uint32_t ExpectedPixel(const int column, const int row, const std::uint32_t frame) {
    std::size_t color = static_cast<std::size_t>(((column / 160) + (row / 90)) % 8);
    if (row < 100) { color = ((frame >> (column / 40)) & 1U) != 0 ? 1 : 0; }
    const int squareLeft = static_cast<int>((frame * 7U) % (GifPatternWidth - 100));
    const int squareTop = 180 + static_cast<int>((frame * 3U) % 400U);
    if (column >= squareLeft && column < squareLeft + 100 && row >= squareTop && row < squareTop + 100) { color = 1; }
    return PatternColors[color];
}

class HandleOwner {
public:
    HandleOwner() = default;
    explicit HandleOwner(const HANDLE handle) : handle_(handle) {}
    ~HandleOwner() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    void Assign(const HANDLE handle) { if (handle_) { throw std::logic_error("Handle already owned"); } handle_ = handle; }
private:
    HANDLE handle_ = nullptr;
};

struct alignas(8) SharedPatternState {
    volatile LONG sequence;
    volatile LONG frameNumber;
    volatile LONG failed;
    volatile LONG reserved;
    volatile LONG64 beginQpc;
    volatile LONG64 endQpc;
};

class MappedState {
public:
    explicit MappedState(const HANDLE mapping) {
        address_ = static_cast<SharedPatternState*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedPatternState)));
        if (!address_) { throw std::runtime_error("MapViewOfFile for pattern service failed"); }
    }
    ~MappedState() { UnmapViewOfFile(address_); }
    MappedState(const MappedState&) = delete;
    MappedState& operator=(const MappedState&) = delete;
    [[nodiscard]] SharedPatternState* Get() const noexcept { return address_; }
private:
    SharedPatternState* address_ = nullptr;
};

class ProcessAttributes {
public:
    explicit ProcessAttributes(std::span<HANDLE> handles) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) { throw std::runtime_error("Cannot size process attribute list"); }
        storage_.resize(bytes);
        attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!attributes_) { throw std::runtime_error("Cannot allocate process attribute list"); }
        if (!InitializeProcThreadAttributeList(attributes_, 1, 0, &bytes)) { throw std::runtime_error("InitializeProcThreadAttributeList failed"); }
        initialized_ = true;
        if (!UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles.data(), handles.size_bytes(), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes_);
            initialized_ = false;
            throw std::runtime_error("Cannot restrict inherited pattern-service handles");
        }
    }
    ~ProcessAttributes() { if (initialized_) { DeleteProcThreadAttributeList(attributes_); } }
    ProcessAttributes(const ProcessAttributes&) = delete;
    ProcessAttributes& operator=(const ProcessAttributes&) = delete;
    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return attributes_; }
private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
    bool initialized_ = false;
};

std::uint64_t ParseUnsigned(const std::wstring& value) {
    if (value.empty()) { throw std::invalid_argument("Missing pattern-service argument"); }
    std::uint64_t result = 0;
    for (const wchar_t digit : value) {
        if (digit < L'0' || digit > L'9' || result > (UINT64_MAX - static_cast<unsigned>(digit - L'0')) / 10U) {
            throw std::invalid_argument("Invalid pattern-service numeric argument");
        }
        result = result * 10U + static_cast<unsigned>(digit - L'0');
    }
    return result;
}

LONG ParseCoordinate(const std::wstring& value) {
    const bool negative = !value.empty() && value.front() == L'-';
    const auto magnitude = ParseUnsigned(negative ? value.substr(1) : value);
    if (magnitude > static_cast<std::uint64_t>(LONG_MAX)) { throw std::invalid_argument("Pattern coordinate out of range"); }
    return negative ? -static_cast<LONG>(magnitude) : static_cast<LONG>(magnitude);
}

HANDLE ParseHandle(const std::wstring& value) {
    const auto number = ParseUnsigned(value);
    if (number == 0 || number > std::numeric_limits<std::uintptr_t>::max()) { throw std::invalid_argument("Invalid inherited handle"); }
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
}

class ServiceWindow final : public Window {
public:
    explicit ServiceWindow(const RECT bounds) : image_(GifPatternWidth, GifPatternHeight) {
        FillGifPattern(image_, 0);
        Create({L"VisPick.Spike.PatternService", 0, IDC_ARROW}, L"VisPick independent GIF test pattern",
               WS_POPUP, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, bounds, WindowActivation::Passive);
        DisableWindowAnimations(Handle());
        ShowWindow(Handle(), SW_SHOWNOACTIVATE);
    }
    ~ServiceWindow() override { Destroy(); }
    void Present(const std::uint32_t frame) {
        FillGifPattern(image_, frame);
        InvalidateRect(Handle(), nullptr, FALSE);
        UpdateWindow(Handle());
        if (!painted_ || !GdiFlush()) { throw std::runtime_error("Independent pattern drawing failed"); }
    }
protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_PAINT) {
            spikes::PaintScope paint(Handle());
            BITMAPINFO description{};
            description.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            description.bmiHeader.biWidth = GifPatternWidth;
            description.bmiHeader.biHeight = -GifPatternHeight;
            description.bmiHeader.biPlanes = 1;
            description.bmiHeader.biBitCount = 32;
            description.bmiHeader.biCompression = BI_RGB;
            painted_ = StretchDIBits(paint.Context(), 0, 0, GifPatternWidth, GifPatternHeight,
                0, 0, GifPatternWidth, GifPatternHeight, image_.Pixels().data(), &description,
                DIB_RGB_COLORS, SRCCOPY) == GifPatternHeight;
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
private:
    Bitmap image_;
    bool painted_ = false;
};

} // namespace

void FillGifPattern(Bitmap& image, const std::uint32_t frameNumber) {
    if (image.Width() != GifPatternWidth || image.Height() != GifPatternHeight) {
        throw std::invalid_argument("GIF pattern must be 1280x720");
    }
    for (int row = 0; row < GifPatternHeight; ++row) {
        auto* destination = image.Row(row);
        for (int column = 0; column < GifPatternWidth; ++column) { destination[column] = ExpectedPixel(column, row, frameNumber); }
    }
}

std::optional<std::uint32_t> ValidateGifPattern(const std::span<const std::uint32_t> pixels) {
    if (pixels.size() != static_cast<std::size_t>(GifPatternWidth) * GifPatternHeight) { return std::nullopt; }
    std::uint32_t frame = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const auto sample = pixels[10U * GifPatternWidth + bit * 40U + 20U] & 0xFFFFFFU;
        if (sample == (PatternColors[1] & 0xFFFFFFU)) { frame |= 1U << bit; }
        else if (sample != (PatternColors[0] & 0xFFFFFFU)) { return std::nullopt; }
    }
    for (int row = 0; row < GifPatternHeight; ++row) {
        for (int column = 0; column < GifPatternWidth; ++column) {
            const auto offset = static_cast<std::size_t>(row) * GifPatternWidth + static_cast<std::size_t>(column);
            if ((pixels[offset] & 0xFFFFFFU) != (ExpectedPixel(column, row, frame) & 0xFFFFFFU)) { return std::nullopt; }
        }
    }
    return frame;
}

std::int64_t QpcNow() {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) { throw std::runtime_error("QueryPerformanceCounter failed"); }
    return counter.QuadPart;
}

std::int64_t QpcNanoseconds(const std::int64_t counter) {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value)) { throw std::runtime_error("QueryPerformanceFrequency failed"); }
        return value.QuadPart;
    }();
    return (counter / frequency) * 1000000000LL + ((counter % frequency) * 1000000000LL) / frequency;
}

MessageTimer::MessageTimer() {
    using CreateTimer = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const FARPROC address = kernel ? GetProcAddress(kernel, "CreateWaitableTimerExW") : nullptr;
    CreateTimer createTimer = nullptr;
    static_assert(sizeof(createTimer) == sizeof(address));
    std::memcpy(&createTimer, &address, sizeof(createTimer));
    if (createTimer) { timer_ = createTimer(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS); }
    if (!timer_) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        timerResolution_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
    if (!timer_) {
        if (timerResolution_) { timeEndPeriod(1); }
        throw std::runtime_error("Cannot create waitable sampling timer");
    }
}

MessageTimer::~MessageTimer() {
    CloseHandle(timer_);
    if (timerResolution_) { timeEndPeriod(1); }
}

bool MessageTimer::WaitUntil(const std::chrono::steady_clock::time_point deadline,
                             const std::span<const HANDLE> cancellationHandles) {
    if (cancellationHandles.size() > 4) { throw std::invalid_argument("Too many timer cancellation handles"); }
    std::array<HANDLE, 5> handles{};
    std::copy(cancellationHandles.begin(), cancellationHandles.end(), handles.begin());
    handles[cancellationHandles.size()] = timer_;
    while (true) {
        // Cancellation wins even when the requested deadline has already passed.
        // Preserve WM_QUIT for the caller's outer message loop.
        MSG quitMessage{};
        if (PeekMessageW(&quitMessage, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) { return false; }
        for (const HANDLE cancellation : cancellationHandles) {
            if (WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0) { return false; }
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) { return true; }
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(1, (nanoseconds + 99) / 100);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) { throw std::runtime_error("SetWaitableTimer failed"); }
        const DWORD handleCount = static_cast<DWORD>(cancellationHandles.size() + 1);
        const DWORD status = MsgWaitForMultipleObjectsEx(handleCount, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (status >= WAIT_OBJECT_0 && status < WAIT_OBJECT_0 + cancellationHandles.size()) { return false; }
        if (status == WAIT_OBJECT_0 + handleCount) {
            PumpWindowMessages();
        }
        else if (status == WAIT_FAILED) { throw std::runtime_error("MsgWaitForMultipleObjectsEx failed"); }
    }
}

struct PatternProcess::State {
    HandleOwner mapping;
    HandleOwner stop;
    HandleOwner ready;
    HandleOwner parent;
    HandleOwner child;
    std::unique_ptr<MappedState> shared;
    void Stop() noexcept {
        if (stop.Get()) { SetEvent(stop.Get()); }
        if (child.Get() && WaitForSingleObject(child.Get(), 3000) == WAIT_TIMEOUT) {
            TerminateProcess(child.Get(), 4); // Only our dedicated test child, after graceful shutdown timeout.
            WaitForSingleObject(child.Get(), 1000);
        }
    }
    ~State() { Stop(); }
};

PatternProcess::PatternProcess(const RECT bounds, const int seconds) : state_(std::make_unique<State>()) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    state_->mapping.Assign(CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0, sizeof(SharedPatternState), nullptr));
    state_->stop.Assign(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    state_->ready.Assign(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    state_->parent.Assign(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
    if (!state_->mapping.Get() || !state_->stop.Get() || !state_->ready.Get() || !state_->parent.Get()) {
        throw std::runtime_error("Cannot initialize independent pattern-process handles");
    }
    state_->shared = std::make_unique<MappedState>(state_->mapping.Get());
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) { throw std::runtime_error("Cannot locate spike executable"); }
    const auto executable = std::filesystem::path(std::wstring(modulePath.data(), length)).parent_path() / L"VisPickSpikes.exe";
    std::wstring command = L"\"" + executable.native() + L"\" --pattern-service " + std::to_wstring(GetCurrentProcessId());
    std::array<HANDLE, 4> inherited{state_->parent.Get(), state_->mapping.Get(), state_->stop.Get(), state_->ready.Get()};
    for (const HANDLE handle : inherited) { command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle)); }
    command += L" " + std::to_wstring(bounds.left) + L" " + std::to_wstring(bounds.top) + L" " + std::to_wstring(seconds);
    ProcessAttributes attributes(inherited);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process)) {
        throw std::runtime_error("Cannot start VisPickSpikes.exe --pattern-service");
    }
    HandleOwner thread(process.hThread);
    state_->child.Assign(process.hProcess);
    const std::array<HANDLE, 2> readyOrExit{state_->ready.Get(), state_->child.Get()};
    const DWORD status = WaitForMultipleObjects(static_cast<DWORD>(readyOrExit.size()), readyOrExit.data(), FALSE, 10000);
    if (status != WAIT_OBJECT_0 || state_->shared->Get()->failed) { throw std::runtime_error("Independent pattern service did not become ready"); }
}

PatternProcess::~PatternProcess() = default;

void PatternProcess::Stop() noexcept { state_->Stop(); }

PatternSnapshot PatternProcess::Snapshot() const noexcept {
    const auto shared = state_->shared->Get();
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = InterlockedCompareExchange(&shared->sequence, 0, 0);
        if ((before & 1) != 0) { continue; }
        PatternSnapshot snapshot{static_cast<std::uint32_t>(shared->frameNumber), shared->beginQpc, shared->endQpc, true};
        const LONG after = InterlockedCompareExchange(&shared->sequence, 0, 0);
        if (before == after) { return snapshot; }
    }
    return {};
}

bool PatternProcess::Running() const noexcept {
    return WaitForSingleObject(state_->child.Get(), 0) == WAIT_TIMEOUT && state_->shared->Get()->failed == 0;
}

int RunPatternService(const std::span<const std::wstring> arguments) {
    if (arguments.size() != 8) { throw std::invalid_argument("Pattern service expects eight internal numeric arguments"); }
    const auto parentId = ParseUnsigned(arguments[0]);
    HandleOwner parent(ParseHandle(arguments[1]));
    HandleOwner mapping(ParseHandle(arguments[2]));
    HandleOwner stop(ParseHandle(arguments[3]));
    HandleOwner ready(ParseHandle(arguments[4]));
    const LONG left = ParseCoordinate(arguments[5]);
    const LONG top = ParseCoordinate(arguments[6]);
    const auto seconds = ParseUnsigned(arguments[7]);
    if (parentId == 0 || parentId > MAXDWORD || seconds < 1 || seconds > 60 ||
        left > LONG_MAX - GifPatternWidth || top > LONG_MAX - GifPatternHeight) {
        throw std::invalid_argument("Pattern service bounds or duration is invalid");
    }
    MappedState mappingView(mapping.Get());
    auto* shared = mappingView.Get();
    try {
        ServiceWindow pattern({left, top, left + GifPatternWidth, top + GifPatternHeight});
        MessageTimer timer;
        const auto start = std::chrono::steady_clock::now();
        const auto safetyDeadline = start + std::chrono::seconds(seconds + 15);
        const std::array<HANDLE, 2> cancellation{stop.Get(), parent.Get()};
        std::uint32_t frame = 0;
        while (std::chrono::steady_clock::now() < safetyDeadline && pattern.Handle()) {
            const auto deadline = start + std::chrono::nanoseconds(static_cast<std::int64_t>(frame) * 1000000000LL / 30);
            if (!timer.WaitUntil(deadline, cancellation)) { break; }
            const auto begin = QpcNow();
            pattern.Present(frame);
            const auto end = QpcNow();
            InterlockedIncrement(&shared->sequence);
            shared->frameNumber = static_cast<LONG>(frame);
            shared->beginQpc = begin;
            shared->endQpc = end;
            InterlockedIncrement(&shared->sequence);
            if (frame == 0) { SetEvent(ready.Get()); }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            frame = std::max(frame + 1, static_cast<std::uint32_t>(elapsed * 30 / 1000000000LL));
        }
        return 0;
    } catch (...) {
        InterlockedExchange(&shared->failed, 1);
        SetEvent(ready.Get());
        throw;
    }
}

} // namespace vispick
