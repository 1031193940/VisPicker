// Real-product lifecycle regression checks. Only our child process/windows and
// private temporary directory are modified; no global input is synthesized.
#include "core/Paths.h"
#include "gif/GifReader.h"
#include "platform/Compat.h"
#include "platform/Console.h"
#include "platform/Gdi.h"
#include "platform/SingleInstance.h"
#include "platform/Window.h"
#include "ForegroundFixture.h"

#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t MainClass[] = L"VisPick.Main";
constexpr int PatternWidth = 320;
constexpr int PatternHeight = 240;
constexpr int Inset = 32;

void Require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
    std::cout << "PASS " << message << '\n';
}

bool WaitUntil(const std::function<bool()>& predicate, DWORD timeout = 5000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { throw std::runtime_error("Lifecycle test was cancelled"); }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (predicate()) { return true; }
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
            throw std::runtime_error("Lifecycle message wait failed");
        }
    } while (Clock::now() < deadline);
    return predicate();
}

class Handle final {
public:
    explicit Handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~Handle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    void Reset(HANDLE handle) noexcept { if (handle_) { CloseHandle(handle_); } handle_ = handle; }
private:
    HANDLE handle_;
};

struct WindowSearch {
    DWORD process;
    const wchar_t* name;
    bool visibleOnly;
    HWND first = nullptr;
    unsigned count = 0;
};
BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM data) noexcept {
    auto& search = *reinterpret_cast<WindowSearch*>(data);
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    if (process != search.process || (search.visibleOnly && !IsWindowVisible(window))) { return TRUE; }
    std::array<wchar_t, 128> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size())) && std::wstring_view(name.data()) == search.name) {
        if (!search.first) { search.first = window; }
        ++search.count;
    }
    return TRUE;
}
WindowSearch Windows(DWORD process, const wchar_t* name, bool visibleOnly = false) {
    WindowSearch search{process, name, visibleOnly};
    if (!EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&search))) { throw std::runtime_error("Cannot enumerate child windows"); }
    return search;
}

std::wstring Quote(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; result += character;
    }
    result.append(slashes * 2, L'\\'); result += L'\"';
    return result;
}

std::vector<wchar_t> ChildEnvironment(const std::filesystem::path& temporary) {
    wchar_t* environment = GetEnvironmentStringsW();
    if (!environment) { throw std::runtime_error("Cannot copy process environment"); }
    struct Release { wchar_t* block; ~Release() { FreeEnvironmentStringsW(block); } } release{environment};
    std::vector<std::wstring> values;
    for (const wchar_t* current = environment; *current; ) {
        const std::wstring_view entry(current);
        if (!(entry.size() >= 5 && _wcsnicmp(current, L"TEMP=", 5) == 0) &&
            !(entry.size() >= 4 && _wcsnicmp(current, L"TMP=", 4) == 0)) { values.emplace_back(entry); }
        current += entry.size() + 1;
    }
    values.push_back(L"TEMP=" + temporary.native()); values.push_back(L"TMP=" + temporary.native());
    std::sort(values.begin(), values.end(), [](const auto& first, const auto& second) { return _wcsicmp(first.c_str(), second.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& value : values) { block.insert(block.end(), value.begin(), value.end()); block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

class Child final {
public:
    Child(const std::filesystem::path& executable, const std::filesystem::path& config,
          const std::filesystem::path& temporary, std::wstring_view command = {}) {
        std::wstring line = Quote(executable.native()) + L" --config " + Quote(config.native());
        if (!command.empty()) { line += L" " + std::wstring(command); }
        auto environment = ChildEnvironment(temporary);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(), executable.parent_path().c_str(), &startup, &process)) {
            throw std::runtime_error("Cannot start isolated product: " + std::to_string(GetLastError()));
        }
        const Handle thread(process.hThread);
        process_.Reset(process.hProcess); identifier_ = process.dwProcessId;
    }
    ~Child() {
        if (Running()) {
            WindowSearch search{identifier_, MainClass, false};
            EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&search));
            if (search.first) { PostMessageW(search.first, WM_ENDSESSION, TRUE, ENDSESSION_CLOSEAPP); }
            if (WaitForSingleObject(process_.Get(), 1500) == WAIT_TIMEOUT) {
                // Failure cleanup is restricted to the handle we created.
                TerminateProcess(process_.Get(), 1); WaitForSingleObject(process_.Get(), 1500);
            }
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    [[nodiscard]] DWORD Identifier() const noexcept { return identifier_; }
    [[nodiscard]] bool Running() const noexcept { return WaitForSingleObject(process_.Get(), 0) == WAIT_TIMEOUT; }
    [[nodiscard]] HWND Main() const { return Windows(identifier_, MainClass).first; }
    [[nodiscard]] DWORD ExitCode() const {
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(process_.Get(), &code)) { throw std::runtime_error("Cannot read product exit code"); }
        return code;
    }
private:
    Handle process_;
    DWORD identifier_ = 0;
};

class Scratch final {
public:
    explicit Scratch(const std::filesystem::path& output) : output_(output) {
        path = std::filesystem::temp_directory_path() / (L"VisPick-lifecycle-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error("Lifecycle scratch directory already exists"); }
        std::filesystem::create_directory(path / L"temp");
    }
    ~Scratch() {
        std::error_code error;
        if (std::filesystem::exists(path / L"VisPick.log", error)) {
            std::filesystem::copy_file(path / L"VisPick.log", output_ / L"product.log", std::filesystem::copy_options::overwrite_existing, error);
        }
        // path is the exact directory created successfully by this object.
        std::filesystem::remove_all(path, error);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    std::filesystem::path path;
private:
    std::filesystem::path output_;
};

std::uint32_t Pixel(int column, int row) noexcept {
    constexpr std::array<std::uint32_t, 8> colors{0xFF113355, 0xFFBB3344, 0xFF22AA66, 0xFFEEDD44,
        0xFF3388DD, 0xFF9955CC, 0xFFEEEEEE, 0xFF222222};
    return colors[static_cast<std::size_t>((column / 16 + row / 16) % 8)];
}

class Pattern final : public vispick::Window {
public:
    Pattern() : surface_(PatternWidth, PatternHeight) {
        const auto monitors = vispick::EnumerateMonitors();
        if (monitors.empty()) { throw std::runtime_error("No monitor for lifecycle test pattern"); }
        const auto& work = monitors.front().workArea;
        if (work.right - work.left < PatternWidth + 80 || work.bottom - work.top < PatternHeight + 100) {
            throw std::runtime_error("Monitor is too small for lifecycle pattern");
        }
        bounds_ = {work.left + 40, work.top + 40, work.left + 40 + PatternWidth, work.top + 40 + PatternHeight};
        for (int row = 0; row < PatternHeight; ++row) {
            for (int column = 0; column < PatternWidth; ++column) {
                surface_.Pixels()[static_cast<std::size_t>(row) * PatternWidth + column] = Pixel(column, row);
            }
        }
        Create({L"VisPick.Tests.LifecyclePattern", 0, IDC_ARROW}, L"VisPick lifecycle test pattern", WS_POPUP,
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, bounds_, vispick::WindowActivation::Passive);
        vispick::DisableWindowAnimations(Handle()); ShowWindow(Handle(), SW_SHOWNOACTIVATE); UpdateWindow(Handle());
        GdiFlush(); DwmFlush();
    }
    ~Pattern() override { Destroy(); }
    [[nodiscard]] RECT Selection() const noexcept { return {bounds_.left + Inset, bounds_.top + Inset, bounds_.right - Inset, bounds_.bottom - Inset}; }
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; const HDC context = BeginPaint(Handle(), &paint);
            BitBlt(context, 0, 0, PatternWidth, PatternHeight, surface_.DeviceContext(), 0, 0, SRCCOPY);
            EndPaint(Handle(), &paint); return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
private:
    vispick::DibSection surface_;
    RECT bounds_{};
};

void SendOwned(HWND window, UINT message, WPARAM parameter = 0, LPARAM data = 0) {
    DWORD_PTR result = 0;
    if (!window || !SendMessageTimeoutW(window, message, parameter, data, SMTO_ABORTIFHUNG, 5000, &result)) {
        throw std::runtime_error("Child window message timed out");
    }
}

void Command(const Child& child, const std::filesystem::path& config, vispick::AppCommand command) {
    vispick::LaunchOptions options; options.command = command; options.configPath = config;
    auto payload = vispick::SingleInstance::Encode(options);
    COPYDATASTRUCT data{vispick::SingleInstance::CopyDataTag, static_cast<DWORD>(payload.size()), payload.data()};
    DWORD_PTR response = 0;
    if (!SendMessageTimeoutW(child.Main(), WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, &response) ||
        response != static_cast<DWORD_PTR>(vispick::SingleInstance::Accepted)) { throw std::runtime_error("Product rejected lifecycle command"); }
}

void Select(const Child& child, RECT selection, bool duplicateConfirm = false) {
    HWND overlay = nullptr;
    Require(WaitUntil([&] { overlay = Windows(child.Identifier(), L"VisPick.Overlay", true).first; return overlay != nullptr; }), "selection_overlay_visible");
    RECT bounds{};
    if (!GetWindowRect(overlay, &bounds)) { throw std::runtime_error("Cannot read overlay bounds"); }
    const LPARAM start = MAKELPARAM(selection.left - bounds.left, selection.top - bounds.top);
    const LPARAM end = MAKELPARAM(selection.right - bounds.left, selection.bottom - bounds.top);
    SendOwned(overlay, WM_LBUTTONDOWN, MK_LBUTTON, start);
    SendOwned(overlay, WM_MOUSEMOVE, MK_LBUTTON, end);
    SendOwned(overlay, WM_LBUTTONUP, 0, end);
    if (!PostMessageW(overlay, WM_KEYDOWN, VK_RETURN, 0) ||
        (duplicateConfirm && !PostMessageW(overlay, WM_KEYDOWN, VK_RETURN, 0))) { throw std::runtime_error("Cannot confirm selection"); }
}

void Answer(const Child& child, int choice) {
    HWND dialog = nullptr;
    Require(WaitUntil([&] { dialog = Windows(child.Identifier(), L"#32770", true).first; return dialog != nullptr; }), "owned_confirmation_appeared");
    const HWND button = GetDlgItem(dialog, choice);
    DWORD process = 0; GetWindowThreadProcessId(button, &process);
    if (!button || process != child.Identifier() || !PostMessageW(button, BM_CLICK, 0, 0)) { throw std::runtime_error("Cannot answer owned confirmation"); }
    Require(WaitUntil([&] { return !IsWindow(dialog); }), "owned_confirmation_closed");
}

std::vector<std::filesystem::path> Recordings(const std::filesystem::path& temporary, DWORD process) {
    std::vector<std::filesystem::path> result;
    const auto directory = temporary / L"VisPick";
    if (!std::filesystem::exists(directory)) { return result; }
    const auto prefix = L"rec_vispick_" + std::to_wstring(process) + L"_";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().filename().native().starts_with(prefix) && entry.path().extension() == L".gif") { result.push_back(entry.path()); }
    }
    return result;
}

void ValidateKnownRecording(const std::filesystem::path& path) {
    vispick::gif::GifReader reader(path);
    const auto document = reader.Metadata();
    Require(document.width == PatternWidth - Inset * 2 && document.height == PatternHeight - Inset * 2 && document.frameCount > 0,
        "preserved_gif_has_expected_dimensions_and_frames");
    vispick::gif::GifCanvas canvas(document);
    for (unsigned frame = 0; frame < document.frameCount; ++frame) {
        canvas.Compose(reader.ReadFrame(frame));
        for (unsigned row = 0; row < document.height; ++row) {
            for (unsigned column = 0; column < document.width; ++column) {
                if ((canvas.Pixels()[static_cast<std::size_t>(row) * document.width + column] & 0xFFFFFFU) !=
                    (Pixel(static_cast<int>(column) + Inset, static_cast<int>(row) + Inset) & 0xFFFFFFU)) {
                    throw std::runtime_error("Recording contains pixels outside the owned pattern; artifact is not retained");
                }
            }
        }
    }
    Require(true, "preserved_gif_pixels_match_owned_pattern");
}

void EndOwnedSession(const Child& application, const std::filesystem::path& preserved) {
    if (!PostMessageW(application.Main(), WM_ENDSESSION, TRUE, ENDSESSION_CLOSEAPP)) {
        throw std::runtime_error("Cannot deliver owned end-session message");
    }
    bool confirmationObserved = false;
    const bool stopped = WaitUntil([&] {
        confirmationObserved = confirmationObserved || Windows(application.Identifier(), L"#32770", true).count != 0;
        return !application.Running();
    });
    Require(stopped && !confirmationObserved && application.ExitCode() == 0, "end_session_exits_without_confirmation");
    Require(std::filesystem::is_regular_file(preserved), "end_session_preserves_unsaved_gif");
}

bool UnsafeExistingUserState() {
    if (FindWindowW(MainClass, nullptr)) { return true; }
    DWORD bytes = 0;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"VisPick", RRF_RT_ANY | RRF_NOEXPAND, nullptr, nullptr, &bytes);
    return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}

void Run(const std::filesystem::path& product, const std::filesystem::path& output) {
    Scratch scratch(output);
    const auto executable = scratch.path / L"VisPick.exe";
    const auto config = scratch.path / L"lifecycle.ini";
    const auto temporary = scratch.path / L"temp";
    std::filesystem::copy_file(product, executable);
    {
        std::ofstream settings(config, std::ios::binary);
        settings << "[Hotkeys]\r\nScreenshot=\r\nGif=\r\nPin=\r\nPinClipboard=\r\nTogglePins=\r\n"
            "[General]\r\nLanguage=en\r\nLog=true\r\n[Pin]\r\nInstant=false\r\n"
            "[Capture]\r\nIncludeCursor=false\r\nMagnifier=false\r\n"
            "[Gif]\r\nFps=30\r\nMaxSeconds=5\r\nScale=100\r\nShowCursor=false\r\nHighlightClicks=false\r\nDither=false\r\n";
        if (!settings) { throw std::runtime_error("Cannot write lifecycle configuration"); }
    }
    Pattern pattern;
    vispick::test::ForegroundFixture foreground;
    {
        foreground.Activate();
        Child application(executable, config, temporary);
        Require(WaitUntil([&] { return application.Main() != nullptr; }), "pin_product_started");
        foreground.GrantTo(application.Identifier());
        Command(application, config, vispick::AppCommand::Pin); Select(application, pattern.Selection());
        Require(WaitUntil([&] { return Windows(application.Identifier(), L"VisPick.Pin", true).count == 1; }), "selection_created_one_pin");
        {
            Child exit(executable, config, temporary, L"--exit");
            Require(WaitUntil([&] { return !exit.Running(); }) && exit.ExitCode() == 0, "secondary_exit_forwarded");
        }
        Answer(application, IDNO);
        Require(application.Running() && Windows(application.Identifier(), L"VisPick.Pin", true).count == 1, "exit_no_preserves_product_and_pin");
        {
            Child exit(executable, config, temporary, L"--exit");
            Require(WaitUntil([&] { return !exit.Running(); }) && exit.ExitCode() == 0, "second_exit_forwarded");
        }
        Answer(application, IDYES);
        Require(WaitUntil([&] { return !application.Running(); }) && application.ExitCode() == 0, "exit_yes_stops_product");
    }
    std::filesystem::path preserved;
    {
        foreground.Activate();
        Child application(executable, config, temporary);
        Require(WaitUntil([&] { return application.Main() != nullptr; }), "gif_product_started");
        foreground.GrantTo(application.Identifier());
        Command(application, config, vispick::AppCommand::Gif); Select(application, pattern.Selection(), true);
        Require(WaitUntil([&] { return Windows(application.Identifier(), L"VisPick.RecordBar").count == 1; }), "duplicate_enter_starts_one_recording");
        const auto started = Clock::now();
        Require(WaitUntil([&] { return Clock::now() - started >= std::chrono::milliseconds(500); }), "recording_interval_elapsed");
        // Public IPC requests only: two stop/start commands may be queued while
        // the first recording drains. No private OverlayResult is fabricated.
        Command(application, config, vispick::AppCommand::Gif);
        Command(application, config, vispick::AppCommand::Gif);
        Require(WaitUntil([&] { return Windows(application.Identifier(), L"VisPick.GifPreview", true).count == 1; }, 10000), "duplicate_gif_commands_produce_one_preview");
        Require(Windows(application.Identifier(), L"VisPick.RecordBar").count == 0 &&
            Windows(application.Identifier(), L"VisPick.Overlay", true).count == 0, "duplicate_commands_leave_no_second_recording_or_selection");
        const auto files = Recordings(temporary, application.Identifier());
        Require(files.size() == 1, "duplicate_commands_produce_one_temporary_recording");
        preserved = files.front();
        EndOwnedSession(application, preserved);
    }
    ValidateKnownRecording(preserved);
    std::filesystem::copy_file(preserved, output / L"session-preserved.gif", std::filesystem::copy_options::overwrite_existing);
    // Model next-day startup with only our file's age changed; do not wait a day
    // or touch unrelated recordings. The artifact above remains reviewable.
    std::filesystem::last_write_time(preserved, std::filesystem::file_time_type::clock::now() - std::chrono::hours(25));
    {
        Child application(executable, config, temporary);
        Require(WaitUntil([&] { return application.Main() != nullptr && !std::filesystem::exists(preserved); }), "next_day_startup_cleans_owned_stale_recording");
        Child exit(executable, config, temporary, L"--exit");
        Require(WaitUntil([&] { return !exit.Running() && !application.Running(); }) && application.ExitCode() == 0,
            "empty_product_exits_without_confirmation");
    }
    {
        foreground.Activate();
        Child application(executable, config, temporary);
        Require(WaitUntil([&] { return application.Main() != nullptr; }), "exit_while_recording_product_started");
        foreground.GrantTo(application.Identifier());
        Command(application, config, vispick::AppCommand::Gif);
        Select(application, pattern.Selection());
        Require(WaitUntil([&] { return Windows(application.Identifier(), L"VisPick.RecordBar", true).count == 1; }), "exit_test_recording_started");
        const auto started = Clock::now();
        Require(WaitUntil([&] { return Clock::now() - started >= std::chrono::milliseconds(500); }), "exit_test_recorded_frames");
        {
            Child exit(executable, config, temporary, L"--exit");
            Require(WaitUntil([&] { return !exit.Running(); }) && exit.ExitCode() == 0, "exit_during_recording_forwarded");
        }
        Answer(application, IDNO);
        const auto files = Recordings(temporary, application.Identifier());
        Require(application.Running() && Windows(application.Identifier(), L"VisPick.GifPreview", true).count == 1 &&
            Windows(application.Identifier(), L"VisPick.RecordBar").count == 0 && files.size() == 1,
            "exit_decline_keeps_completed_recording_and_product");
        EndOwnedSession(application, files.front());
        ValidateKnownRecording(files.front());
    }
}
} // namespace

int wmain(int count, wchar_t** arguments) {
    std::filesystem::path output;
    try {
        auto product = vispick::ExecutablePath().parent_path() / L"VisPick.exe";
        output = std::filesystem::current_path() / L"out/reports/app-lifecycle";
        bool productSpecified = false;
        for (int argument = 1; argument < count; ++argument) {
            const std::wstring_view option(arguments[argument]);
            if (option == L"--output" && argument + 1 < count) { output = arguments[++argument]; }
            else if (!option.starts_with(L"--") && !productSpecified) { product = option; productSpecified = true; }
            else { throw std::invalid_argument("Usage: VisPickAppLifecycleIntegration [PRODUCT_EXE] [--output DIRECTORY]"); }
        }
        product = std::filesystem::absolute(product); output = std::filesystem::absolute(output);
        std::filesystem::create_directories(output);
        if (!std::filesystem::is_regular_file(product)) { throw std::runtime_error("Product executable not found"); }
        if (UnsafeExistingUserState()) {
            std::cout << "SKIP existing VisPick instance/autostart value or unreadable user state\n";
            std::ofstream(output / L"result.txt") << "SKIP: user state untouched\n";
            return 77;
        }
        vispick::InitializeDpiAwareness();
        Run(product, output);
        std::ofstream(output / L"result.txt") << "PASS: real-product pin exit confirmation; duplicate public GIF commands; exit while recording retains preview on No; no-dialog session end; preserved exact-pattern GIF; simulated next-day cleanup\n"
            "Boundary: this does not inject two private OverlayResult objects or test OS-wide logout.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        if (!output.empty()) { std::ofstream(output / L"result.txt") << "FAIL: " << error.what() << '\n'; }
        return 1;
    }
}
