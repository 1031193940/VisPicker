// Developer-only resident-product checks. No SendInput, shell termination, or
// registry mutation; only the copied child process and its own window are used.
#include "core/Paths.h"
#include "platform/Console.h"
#include "ForegroundFixture.h"

#include <Windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT TestModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr wchar_t MainClass[] = L"VisPick.Main";

struct Check { std::string name; bool passed; std::string detail; };
struct Report {
    bool skipped = false;
    std::string skipReason;
    std::vector<Check> checks;
    std::vector<std::pair<std::string, double>> measurements;
    void Require(bool passed, std::string name, std::string detail = {}) {
        checks.push_back({std::move(name), passed, std::move(detail)});
        std::cout << (passed ? "PASS " : "FAIL ") << checks.back().name << '\n';
        if (!passed) { throw std::runtime_error(checks.back().name); }
    }
};

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    void Reset(HANDLE value) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); }
        value_ = value;
    }
private:
    HANDLE value_;
};

bool AutostartValueExists() {
    DWORD bytes = 0;
    // HKCU/Software/Windows CurrentVersion/Run is shared across WOW64 views;
    // the RegGetValue WOW64-subkey flags themselves require newer Windows.
    const auto queried = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"VisPick", RRF_RT_ANY | RRF_NOEXPAND, nullptr, nullptr, &bytes);
    if (queried == ERROR_FILE_NOT_FOUND || queried == ERROR_PATH_NOT_FOUND) { return false; }
    if (queried != ERROR_SUCCESS && queried != ERROR_MORE_DATA) {
        throw std::runtime_error("Cannot safely inspect existing VisPick autostart value");
    }
    return true;
}

std::wstring Quote(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

struct WindowSearch { DWORD process; HWND found = nullptr; const wchar_t* windowClass = MainClass; };
BOOL CALLBACK FindOwnedWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.process) { return TRUE; }
    std::array<wchar_t, 128> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size())) != 0 &&
        std::wstring_view(name.data()) == search.windowClass) {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

class ChildAttributes final {
public:
    explicit ChildAttributes(std::span<const HANDLE> inherited) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0) { throw std::runtime_error("Cannot size child handle list"); }
        storage_.resize(size);
        list_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) { list_ = nullptr; throw std::runtime_error("Cannot initialize child handle list"); }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()), inherited.size_bytes(), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
            throw std::runtime_error("Cannot restrict inherited child handles");
        }
    }
    ~ChildAttributes() { if (list_ != nullptr) { DeleteProcThreadAttributeList(list_); } }
    ChildAttributes(const ChildAttributes&) = delete;
    ChildAttributes& operator=(const ChildAttributes&) = delete;
    [[nodiscard]] PPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_; }
private:
    std::vector<std::byte> storage_;
    PPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

class Child final {
public:
    Child(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
          const std::filesystem::path& diagnosticsDirectory) {
        std::wstring command = Quote(executable.native());
        for (const auto& argument : arguments) { command += L" " + Quote(argument); }
        static std::atomic<unsigned> sequence{0};
        const auto redirectedPath = diagnosticsDirectory / (L"child-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(sequence.fetch_add(1)) + L".txt");
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        const Handle redirected(CreateFileW(redirectedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        const Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (redirected.Get() == INVALID_HANDLE_VALUE || input.Get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot redirect product diagnostics");
        }
        const std::array inherited{redirected.Get(), input.Get()};
        const ChildAttributes attributes(inherited);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = redirected.Get();
        startup.StartupInfo.hStdError = redirected.Get();
        startup.StartupInfo.hStdInput = input.Get();
        startup.lpAttributeList = attributes.Get();
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &created)) {
            throw std::runtime_error("Create isolated product failed: " + std::to_string(GetLastError()));
        }
        const Handle thread(created.hThread);
        process_.Reset(created.hProcess);
        identifier_ = created.dwProcessId;
    }
    ~Child() {
        if (Running()) {
            const auto window = Window();
            if (window != nullptr) { PostMessageW(window, WM_CLOSE, 0, 0); }
            if (WaitForSingleObject(process_.Get(), 3000) == WAIT_TIMEOUT) {
                // Only this process handle, returned by our CreateProcess call.
                TerminateProcess(process_.Get(), 1);
                WaitForSingleObject(process_.Get(), 3000);
            }
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    [[nodiscard]] HANDLE Process() const noexcept { return process_.Get(); }
    [[nodiscard]] bool Running() const noexcept { return WaitForSingleObject(Process(), 0) == WAIT_TIMEOUT; }
    [[nodiscard]] HWND Window() const noexcept {
        WindowSearch search{identifier_};
        EnumWindows(FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    [[nodiscard]] DWORD Wait(DWORD milliseconds) const {
        if (WaitForSingleObject(Process(), milliseconds) != WAIT_OBJECT_0) { throw std::runtime_error("Child process timeout"); }
        DWORD code = 0;
        if (!GetExitCodeProcess(Process(), &code)) { throw std::runtime_error("Cannot read child exit code"); }
        return code;
    }
private:
    Handle process_;
    DWORD identifier_ = 0;
};

class Scratch final {
public:
    explicit Scratch(std::filesystem::path output) : output_(std::move(output)) {
        path = std::filesystem::temp_directory_path() / (L"VisPick-app-e2e-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error("Isolated directory already exists"); }
    }
    ~Scratch() {
        std::error_code error;
        for (const auto* name : {L"VisPick.log", L"isolated.ini"}) {
            if (std::filesystem::exists(path / name, error)) {
                std::filesystem::copy_file(path / name, output_ / name,
                    std::filesystem::copy_options::overwrite_existing, error);
            }
        }
        std::filesystem::remove_all(path, error); // Only our newly created directory.
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    std::filesystem::path path;
private:
    std::filesystem::path output_;
};

class HotkeyLease final {
public:
    explicit HotkeyLease(UINT key) : registered_(RegisterHotKey(nullptr, 0x1250,
        TestModifiers | MOD_NOREPEAT, key) != FALSE) {}
    ~HotkeyLease() { if (registered_) { UnregisterHotKey(nullptr, 0x1250); } }
    HotkeyLease(const HotkeyLease&) = delete;
    HotkeyLease& operator=(const HotkeyLease&) = delete;
    [[nodiscard]] bool Registered() const noexcept { return registered_; }
private:
    bool registered_;
};

bool HotkeyAvailable(UINT key) {
    if (!RegisterHotKey(nullptr, 0x1240, TestModifiers | MOD_NOREPEAT, key)) {
        if (GetLastError() != ERROR_HOTKEY_ALREADY_REGISTERED) { throw std::runtime_error("Unexpected hotkey probe failure"); }
        return false;
    }
    if (!UnregisterHotKey(nullptr, 0x1240)) { throw std::runtime_error("Cannot release test hotkey probe"); }
    return true;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, std::string_view bytes, bool atomicReplace = false) {
    const auto destination = atomicReplace ? std::filesystem::path(path.native() + L".saving") : path;
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) { throw std::runtime_error("Cannot write isolated configuration"); }
    }
    if (atomicReplace && !MoveFileExW(destination.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Atomic configuration replacement failed");
    }
}

std::size_t Occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t start = 0; (start = text.find(needle, start)) != std::string_view::npos; start += needle.size()) { ++count; }
    return count;
}

bool WaitUntil(const std::function<bool()>& predicate, DWORD timeout = 5000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    do {
        if (predicate()) { return true; }
        Sleep(10);
    } while (Clock::now() < deadline);
    return predicate();
}

void PostOwned(const Child& child, UINT message, WPARAM parameter = 0, LPARAM data = 0) {
    const auto window = child.Window();
    if (window == nullptr || !PostMessageW(window, message, parameter, data)) {
        throw std::runtime_error("Cannot post to the isolated product window");
    }
}

void CancelOwnedOverlay(const Child& child) {
    WindowSearch search{GetProcessId(child.Process()), nullptr, L"VisPick.Overlay"};
    EnumWindows(FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.found) { return; } // P1 binaries have no selection window.
    if (!WaitUntil([&] { return IsWindowVisible(search.found) != FALSE; })) {
        throw std::runtime_error("Capture command did not show its selection window");
    }
    PostMessageW(search.found, WM_KEYDOWN, VK_ESCAPE, 0);
    if (!WaitUntil([&] { return IsWindowVisible(search.found) == FALSE; })) {
        throw std::runtime_error("Capture cancellation did not hide its selection window");
    }
}

std::string Configuration(std::string_view screenshot, unsigned gif, unsigned togglePins, bool logging = true) {
    return "; VisPick E2E 隔离配置\r\n[Hotkeys]\r\nScreenshot=" + std::string(screenshot) +
        "\r\nGif=Ctrl+Alt+Shift+F" + std::to_string(gif) + "\r\nPinClipboard=\r\nTogglePins=Ctrl+Alt+Shift+F" +
        std::to_string(togglePins) + "\r\n[General]\r\nLanguage=en\r\nLog=" +
        (logging ? "true" : "false") + "\r\n";
}

void Reload(Report& report, const std::filesystem::path& config, const std::filesystem::path& log,
            const std::string& contents, std::string name, bool atomicReplace = false,
            std::string_view expected = "config_reload accepted=1 diagnostics=0") {
    const auto previous = Occurrences(ReadFile(log), expected);
    const auto start = Clock::now();
    WriteFile(config, contents, atomicReplace);
    const bool observed = WaitUntil([&] { return Occurrences(ReadFile(log), expected) > previous; }, 1000);
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    report.measurements.emplace_back(name + "_milliseconds", elapsed);
    report.Require(observed && elapsed < 1000.0, std::move(name), "Measured from save start to product reload record; limit <1000ms.");
}

void DispatchHotkey(Report& report, const Child& child, vispick::test::ForegroundFixture& foreground, const std::filesystem::path& log,
                    unsigned identifier, UINT key, std::string_view command) {
    const std::string record = "command=" + std::string(command);
    const auto previous = Occurrences(ReadFile(log), record);
    foreground.GrantTo(GetProcessId(child.Process()));
    PostOwned(child, WM_HOTKEY, identifier, MAKELPARAM(TestModifiers, key));
    report.Require(WaitUntil([&] { return Occurrences(ReadFile(log), record) > previous; }),
        "wm_hotkey_dispatch_" + std::string(command), "Synthetic WM_HOTKEY; actual registration is probed independently, without SendInput.");
    if (command == "screenshot" || command == "gif") { CancelOwnedOverlay(child); }
}

std::uint64_t ProcessCpu100ns(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) { throw std::runtime_error("GetProcessTimes failed"); }
    const auto ticks = [](FILETIME value) { return static_cast<std::uint64_t>(value.dwHighDateTime) << 32 | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}

std::uint64_t PrivateWorkingSet(HANDLE process) {
    // QueryWorkingSet's Shared bit describes resident pages. PrivateUsage from
    // PROCESS_MEMORY_COUNTERS is committed private bytes and is not substituted.
    std::vector<ULONG_PTR> storage(4097);
    while (!QueryWorkingSet(process, storage.data(), static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR)))) {
        if (GetLastError() != ERROR_BAD_LENGTH || storage.size() >= 1048576) { throw std::runtime_error("QueryWorkingSet failed"); }
        storage.resize(storage.size() * 2);
    }
    const auto* information = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(storage.data());
    if (information->NumberOfEntries >= storage.size()) { throw std::runtime_error("Invalid working-set entry count"); }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::uint64_t privatePages = 0;
    for (ULONG_PTR page = 0; page < information->NumberOfEntries; ++page) {
        if (!information->WorkingSetInfo[page].Shared) { ++privatePages; }
    }
    return privatePages * system.dwPageSize;
}

void RunChecks(Report& report, const std::filesystem::path& product,
               const std::filesystem::path& output, unsigned idleSeconds) {
    if (FindWindowW(MainClass, nullptr) != nullptr) {
        report.skipped = true; report.skipReason = "An existing VisPick.Main window is present; user instance untouched."; return;
    }
    try {
        if (AutostartValueExists()) {
            report.skipped = true; report.skipReason = "HKCU Run already contains VisPick; startup repair could modify it, so product checks were skipped."; return;
        }
    } catch (const std::exception& error) { report.skipped = true; report.skipReason = error.what(); return; }
    for (UINT key = VK_F13; key <= VK_F20; ++key) {
        if (!HotkeyAvailable(key)) { report.skipped = true; report.skipReason = "An isolated Ctrl+Alt+Shift+F13..F20 test key is already owned; no application was launched."; return; }
    }
    Scratch scratch(output);
    const auto executable = scratch.path / L"VisPick.exe";
    const auto config = scratch.path / L"isolated.ini";
    const auto log = scratch.path / L"VisPick.log";
    std::filesystem::copy_file(product, executable);
    for (const auto* library : {L"dwmapi.dll", L"gdiplus.dll", L"winmm.dll", L"wtsapi32.dll"}) {
        WriteFile(scratch.path / library, "Invalid DLL fixture. The product must resolve system libraries from Windows.");
    }
    const auto blockedParent = scratch.path / L"blocked-config-parent";
    WriteFile(blockedParent, "A regular file, never a directory.");
    {
        Child invalidStart(executable, {L"--config", (blockedParent / L"bad.ini").native()}, output);
        report.Require(invalidStart.Wait(5000) != 0, "startup_path_error_is_reported_without_dialog");
    }
    vispick::test::ForegroundFixture foreground;
    foreground.Activate();
    Child application(executable, {L"--config", config.native()}, output);
    report.Require(WaitUntil([&] { return application.Running() && application.Window() != nullptr && std::filesystem::exists(config); }),
        "isolated_product_started");
    const auto initialConfig = ReadFile(config);
    const auto initialWide = vispick::Utf8ToWide(initialConfig);
    report.Require(initialConfig.find("[Hotkeys]") != std::string::npos && initialConfig.find("Ctrl+1") != std::string::npos &&
        std::any_of(initialWide.begin(), initialWide.end(), [](wchar_t character) { return character >= 0x4E00 && character <= 0x9FFF; }),
        "create_new_default_chinese_template");
    report.Require(!std::filesystem::exists(log), "default_logging_creates_no_file");

    Reload(report, config, log, Configuration("Ctrl+Alt+Shift+F13", 14, 15), "initial_isolated_hotkeys");
    report.Require(!HotkeyAvailable(VK_F13) && !HotkeyAvailable(VK_F14) && !HotkeyAvailable(VK_F15), "three_hotkeys_actually_registered");
    Reload(report, config, log, Configuration("Ctrl+Alt+Shift+F16", 14, 15), "hotkey_change_below_one_second");
    report.Require(HotkeyAvailable(VK_F13) && !HotkeyAvailable(VK_F16), "old_binding_released_new_binding_registered");
    DispatchHotkey(report, application, foreground, log, 1, VK_F16, "screenshot");
    DispatchHotkey(report, application, foreground, log, 2, VK_F14, "gif");
    DispatchHotkey(report, application, foreground, log, 4, VK_F15, "toggle-pins");
    const auto occupiedConfiguration = Configuration("Ctrl+Alt+Shift+F16", 17, 15);
    std::size_t reloadsWhileOccupied = 0;
    {
        const HotkeyLease conflict(VK_F17);
        report.Require(conflict.Registered(), "external_conflict_probe_registered");
        Reload(report, config, log, occupiedConfiguration, "conflict_reload");
        report.Require(ReadFile(log).find("These shortcuts could not be registered") != std::string::npos &&
            HotkeyAvailable(VK_F14) && !HotkeyAvailable(VK_F16) && !HotkeyAvailable(VK_F15), "conflict_reported_other_hotkeys_preserved");
        DispatchHotkey(report, application, foreground, log, 1, VK_F16, "screenshot");
        reloadsWhileOccupied = Occurrences(ReadFile(log), "config_reload ");
    }
    const auto releasedAt = Clock::now();
    report.Require(WaitUntil([&] { return !HotkeyAvailable(VK_F17); }) &&
        ReadFile(config) == occupiedConfiguration && Occurrences(ReadFile(log), "config_reload ") == reloadsWhileOccupied,
        "occupied_hotkey_recovers_without_configuration_reload", "Waits for real registration after releasing the test lease. No reload command or file edit.");
    report.measurements.emplace_back("automatic_hotkey_recovery_milliseconds",
        std::chrono::duration<double, std::milli>(Clock::now() - releasedAt).count());
    DispatchHotkey(report, application, foreground, log, 2, VK_F17, "gif");
    Reload(report, config, log, Configuration("Ctrl+Alt+Shift+F16", 18, 15), "hotkey_change_after_automatic_recovery");
    report.Require(HotkeyAvailable(VK_F17) && !HotkeyAvailable(VK_F18), "recovered_hotkey_releases_when_changed");
    Reload(report, config, log, Configuration("Ctrl+", 18, 15), "invalid_hotkey_reload", false, "config_reload accepted=1 diagnostics=1");
    report.Require(!HotkeyAvailable(VK_F16) && !HotkeyAvailable(VK_F18) && !HotkeyAvailable(VK_F15) &&
        ReadFile(log).find("Configuration line ") != std::string::npos, "invalid_value_retains_previous_bindings_with_line_diagnostic");
    const std::string invalidUtf8("\xFF\xFE\x80", 3);
    Reload(report, config, log, invalidUtf8, "invalid_encoding_reload", false, "config_reload accepted=0");
    report.Require(!HotkeyAvailable(VK_F16) && !HotkeyAvailable(VK_F18) && !HotkeyAvailable(VK_F15), "invalid_encoding_retains_previous_snapshot");
    Reload(report, config, log, "", "truncated_configuration_retry", false, "config_reload accepted=0");
    report.Require(!HotkeyAvailable(VK_F16) && !HotkeyAvailable(VK_F18) && !HotkeyAvailable(VK_F15), "truncated_file_retains_active_bindings");
    Reload(report, config, log, "[Capture]\nMagnifier=false\n", "partial_configuration_preserves_bindings");
    report.Require(!HotkeyAvailable(VK_F16) && !HotkeyAvailable(VK_F18) && !HotkeyAvailable(VK_F15), "partial_file_retains_unspecified_bindings");
    const auto finalConfiguration = Configuration("Ctrl+Alt+Shift+F19", 20, 15);
    Reload(report, config, log, finalConfiguration, "atomic_replace_reload", true);
    report.Require(HotkeyAvailable(VK_F16) && HotkeyAvailable(VK_F18) && !HotkeyAvailable(VK_F19) && !HotkeyAvailable(VK_F20),
        "atomic_replace_updates_bindings");
    const auto forwardedBefore = Occurrences(ReadFile(log), "command=screenshot");
    foreground.GrantTo(GetProcessId(application.Process()));
    {
        Child forwarded(executable, {L"--screenshot", L"--config", config.native()}, output);
        report.Require(forwarded.Wait(5000) == 0 && application.Running(), "secondary_process_exits_original_survives");
    }
    report.Require(WaitUntil([&] { return Occurrences(ReadFile(log), "command=screenshot") > forwardedBefore; }), "secondary_command_forwarded_to_original");
    CancelOwnedOverlay(application);
    report.Require(ReadFile(config) == finalConfiguration, "secondary_start_does_not_replace_existing_config");
    {
        const auto otherConfig = scratch.path / L"other-config.ini";
        Child conflicting(executable, {L"--screenshot", L"--config", otherConfig.native()}, output);
        report.Require(conflicting.Wait(5000) == 4 && application.Running() && !std::filesystem::exists(otherConfig),
            "secondary_config_conflict_rejected_without_touching_original");
    }
    const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    report.Require(taskbarCreated != 0, "taskbar_message_registered");
    const auto restoredBefore = Occurrences(ReadFile(log), "tray_restored");
    PostOwned(application, taskbarCreated);
    report.Require(WaitUntil([&] { return Occurrences(ReadFile(log), "tray_restored") > restoredBefore; }),
        "owned_window_handles_taskbar_created", "Only the owned HWND receives the message; Explorer is not restarted.");

    WriteFile(config, Configuration("Ctrl+Alt+Shift+F19", 20, 15, false));
    report.Require(WaitUntil([&] {
        const Handle exclusive(CreateFileW(log.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        return exclusive.Get() != INVALID_HANDLE_VALUE;
    }, 1000), "logging_disabled_closes_file");
    const auto logSize = std::filesystem::file_size(log);
    foreground.GrantTo(GetProcessId(application.Process()));
    PostOwned(application, WM_HOTKEY, 1, MAKELPARAM(TestModifiers, VK_F19));
    CancelOwnedOverlay(application);
    Sleep(250);
    report.Require(std::filesystem::file_size(log) == logSize, "disabled_logging_stops_record_writes");
    if (idleSeconds != 0) {
        std::cout << "Measuring resident idle for " << idleSeconds << " seconds" << std::endl;
        const auto cpuBefore = ProcessCpu100ns(application.Process());
        const auto started = Clock::now();
        auto maximumPrivate = PrivateWorkingSet(application.Process());
        while (Clock::now() - started < std::chrono::seconds(idleSeconds)) {
            if (WaitForSingleObject(application.Process(), 1000) != WAIT_TIMEOUT) {
                report.Require(false, "idle_product_alive");
            }
            maximumPrivate = std::max(maximumPrivate, PrivateWorkingSet(application.Process()));
        }
        const auto cpuAfter = ProcessCpu100ns(application.Process());
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        const double cpuPercent = static_cast<double>(cpuAfter - cpuBefore) / 100000.0 / seconds;
        report.measurements.emplace_back("idle_wall_seconds", seconds);
        report.measurements.emplace_back("idle_single_core_cpu_percent", cpuPercent);
        report.measurements.emplace_back("idle_max_private_working_set_bytes", static_cast<double>(maximumPrivate));
        report.Require(true, "idle_product_alive");
        report.Require(cpuPercent < 0.1, "idle_cpu_below_0_1_percent");
        report.Require(maximumPrivate <= 5ULL * 1024 * 1024, "idle_private_working_set_at_most_5_mib");
        report.Require(std::filesystem::file_size(log) == logSize, "idle_logging_remains_disabled");
    }
    {
        Child exitCommand(executable, {L"--exit", L"--config", config.native()}, output);
        report.Require(exitCommand.Wait(5000) == 0 && application.Wait(5000) == 0, "forwarded_exit_cleanly_stops_product");
    }
    report.Require(HotkeyAvailable(VK_F19) && HotkeyAvailable(VK_F20) && HotkeyAvailable(VK_F15), "exit_releases_all_test_hotkeys");
    report.Require(!AutostartValueExists(), "autostart_value_remains_absent");
}

std::string Json(std::string_view text) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') { result += '\\'; result += static_cast<char>(character); }
        else if (character < 32) { result += "\\u00"; result += digits[character >> 4]; result += digits[character & 15]; }
        else { result += static_cast<char>(character); }
    }
    return result + '"';
}

void SaveReport(const Report& report, const std::filesystem::path& output, unsigned idleSeconds) {
    std::ofstream stream(output / L"app-integration.json", std::ios::binary);
    stream << std::setprecision(17);
    const bool passed = !report.skipped && std::all_of(report.checks.begin(), report.checks.end(), [](const Check& check) { return check.passed; });
    const bool measuredTenMinutes = std::any_of(report.measurements.begin(), report.measurements.end(),
        [](const auto& measurement) { return measurement.first == "idle_wall_seconds" && measurement.second >= 600.0; });
    stream << "{\n  \"passed\": " << (passed ? "true" : "false") << ",\n  \"skipped\": " << (report.skipped ? "true" : "false")
        << ",\n  \"skip_reason\": " << Json(report.skipReason) << ",\n  \"idle_requested_seconds\": " << idleSeconds
        << ",\n  \"ten_minute_idle_measured\": " << (measuredTenMinutes ? "true" : "false") << ",\n  \"checks\": [";
    for (std::size_t index = 0; index < report.checks.size(); ++index) {
        const auto& check = report.checks[index];
        stream << (index == 0 ? "\n    " : ",\n    ") << "{\"name\":" << Json(check.name) << ",\"passed\":"
            << (check.passed ? "true" : "false") << ",\"detail\":" << Json(check.detail) << '}';
    }
    stream << "\n  ],\n  \"measurements\": {";
    for (std::size_t index = 0; index < report.measurements.size(); ++index) {
        stream << (index == 0 ? "\n    " : ",\n    ") << Json(report.measurements[index].first) << ':' << report.measurements[index].second;
    }
    stream << "\n  }\n}\n";
    if (!stream) { throw std::runtime_error("Cannot save product integration report"); }
}

} // namespace

int wmain() {
    Report report;
    std::filesystem::path output;
    unsigned idleSeconds = 0;
    try {
        const auto arguments = vispick::ProcessArguments();
        auto product = vispick::ExecutablePath().parent_path() / L"VisPick.exe";
        output = std::filesystem::current_path() / L"out/reports/app-integration";
        bool productSpecified = false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (argument == L"--output" || argument == L"--idle-seconds") {
                if (++index >= arguments.size()) { throw std::invalid_argument("Missing option value"); }
                if (argument == L"--output") { output = arguments[index]; }
                else {
                    std::size_t consumed = 0;
                    const auto value = std::stoul(arguments[index], &consumed);
                    if (consumed != arguments[index].size() || value > 86400) { throw std::invalid_argument("Invalid idle duration"); }
                    idleSeconds = static_cast<unsigned>(value);
                }
            } else if (!argument.starts_with(L"--") && !productSpecified) { product = argument; productSpecified = true; }
            else { throw std::invalid_argument("Usage: VisPickAppIntegration [PRODUCT_EXE] [--output DIRECTORY] [--idle-seconds 600]"); }
        }
        output = std::filesystem::absolute(output);
        std::filesystem::create_directories(output);
        product = std::filesystem::absolute(product);
        if (!std::filesystem::is_regular_file(product)) { throw std::runtime_error("Product executable not found"); }
        RunChecks(report, product, output, idleSeconds);
        SaveReport(report, output, idleSeconds);
        if (report.skipped) { std::cout << "SKIP " << report.skipReason << '\n'; return 77; }
        return 0;
    } catch (const std::exception& error) {
        report.checks.push_back({"integration_exception", false, error.what()});
        std::cerr << "FAIL " << error.what() << '\n';
        if (!output.empty()) {
            try { SaveReport(report, output, idleSeconds); } catch (...) { }
        }
        return 1;
    }
}
