// Development-only P07 integration checks. All GUI operations target children
// created by this process; no existing user windows are enumerated for actions.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Reset(); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    HANDLE Get() const noexcept { return value_; }
    bool Valid() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    void Reset() noexcept { if (Valid()) { CloseHandle(value_); } value_ = nullptr; }
private:
    HANDLE value_ = nullptr;
};

struct Child {
    Child(Handle processHandle, Handle threadHandle, DWORD processIdentifier)
        : process(std::move(processHandle)), thread(std::move(threadHandle)), identifier(processIdentifier) {}
    ~Child() {
        if (process.Valid() && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.Get(), 99); // Only our still-running test child.
            WaitForSingleObject(process.Get(), 2000);
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = default;
    Handle process;
    Handle thread;
    DWORD identifier;
};

void Require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::wstring SelfPath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    Require(length != 0 && length < buffer.size(), "GetModuleFileName failed");
    buffer.resize(length);
    return buffer;
}

// Inputs are absolute filesystem paths, with no embedded quote supported.
std::wstring Quote(std::wstring_view value) {
    Require(value.find(L'"') == std::wstring_view::npos, "Unexpected quote in executable path");
    return L"\"" + std::wstring(value) + L"\"";
}

Child Start(const std::wstring& executable, const std::wstring& arguments, DWORD flags,
            bool explicitHandles = false, HANDLE output = nullptr, HANDLE error = nullptr) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (explicitHandles) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = output;
        startup.hStdError = error;
    }
    PROCESS_INFORMATION information{};
    std::wstring command = Quote(executable) + L" " + arguments;
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        explicitHandles ? TRUE : FALSE, flags, nullptr, nullptr, &startup, &information);
    Handle process(information.hProcess);
    Handle thread(information.hThread);
    Require(created != FALSE, "CreateProcess failed");
    return Child(std::move(process), std::move(thread), information.dwProcessId);
}

struct DialogSearch { DWORD process; HWND found = nullptr; };

BOOL CALLBACK FindDialog(HWND window, LPARAM data) noexcept {
    auto& search = *reinterpret_cast<DialogSearch*>(data);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.process) { return TRUE; }
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    if (std::wstring_view(className) == L"#32770") { search.found = window; return FALSE; }
    return TRUE;
}

HWND OwnedDialog(DWORD process) {
    DialogSearch search{process};
    EnumWindows(FindDialog, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

void CloseOwnedDialog(DWORD process) {
    if (const HWND dialog = OwnedDialog(process)) {
        DWORD actualProcess = 0;
        GetWindowThreadProcessId(dialog, &actualProcess);
        if (actualProcess == process) { PostMessageW(dialog, WM_CLOSE, 0, 0); }
    }
}

DWORD Finish(Child& child, DWORD timeout = 5000) {
    if (WaitForSingleObject(child.process.Get(), timeout) != WAIT_OBJECT_0) {
        CloseOwnedDialog(child.identifier);
        WaitForSingleObject(child.process.Get(), 1000);
        throw std::runtime_error("Child timed out (possible unexpected MessageBox)");
    }
    DWORD status = 0;
    Require(GetExitCodeProcess(child.process.Get(), &status) != FALSE, "GetExitCodeProcess failed");
    return status;
}

std::wstring DecodeUtf8(const std::string& bytes) {
    Require(!bytes.empty() && bytes.size() < 65536, "Unexpected output size");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    Require(count > 0, "Output is not valid UTF-8");
    std::wstring decoded(static_cast<std::size_t>(count), L'\0');
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), decoded.data(), count) == count, "UTF-8 decode failed");
    return decoded;
}

bool HasChinese(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t character) {
        return character >= 0x4E00 && character <= 0x9FFF;
    });
}

std::string ReadBytes(HANDLE handle) {
    std::string bytes;
    char buffer[4096]{};
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read != 0) {
        bytes.append(buffer, read);
        Require(bytes.size() < 65536, "Unexpectedly large redirected output");
    }
    return bytes;
}

class DiagnosticLog {
public:
    DiagnosticLog() {
        wchar_t directory[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        Require(length != 0 && length < MAX_PATH, "Temporary log directory is unavailable");
        path_ = std::wstring(directory) + L"VisPick-console-log-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".txt";
        file_ = Handle(CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        Require(file_.Valid(), "Could not create private helper diagnostic log");
    }
    const std::wstring& Path() const noexcept { return path_; }
    std::wstring Read() {
        LARGE_INTEGER start{};
        Require(SetFilePointerEx(file_.Get(), start, nullptr, FILE_BEGIN) != FALSE, "Diagnostic rewind failed");
        const auto bytes = ReadBytes(file_.Get());
        return bytes.empty() ? L"no helper diagnostic" : DecodeUtf8(bytes);
    }
private:
    std::wstring path_;
    Handle file_;
};

void WriteDiagnostic(const wchar_t* path, const char* text) noexcept {
    // A private side channel, never stdout/stderr: preserves the output state
    // under test, including detached helpers with no console or standard handles.
    Handle file(CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.Valid()) {
        DWORD written = 0;
        const auto length = static_cast<DWORD>(std::char_traits<char>::length(text));
        (void)WriteFile(file.Get(), text, length, &written, nullptr);
    }
}

void ConsoleCase(const std::wstring& product) {
    Handle console(CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    Require(console.Valid(), "Helper does not have a real console");
    CONSOLE_SCREEN_BUFFER_INFO information{};
    Require(GetConsoleScreenBufferInfo(console.Get(), &information) != FALSE, "Console buffer query failed");
    DWORD written = 0;
    FillConsoleOutputCharacterW(console.Get(), L' ',
        static_cast<DWORD>(information.dwSize.X) * static_cast<DWORD>(information.dwSize.Y), {0, 0}, &written);
    SetConsoleCursorPosition(console.Get(), {0, 0});
    // The GUI process starts with NULL standard handles but an actual console parent.
    auto child = Start(product, L"--help", 0, true);
    Require(Finish(child) == 0, "Help child returned an error");
    std::wstring text(16384, L'\0');
    DWORD read = 0;
    Require(ReadConsoleOutputCharacterW(console.Get(), text.data(), static_cast<DWORD>(text.size()),
        {0, 0}, &read) != FALSE, "Console buffer read failed");
    text.resize(read);
    Require(text.find(L"VisPick") != std::wstring::npos && HasChinese(text), "Help was not written as Unicode console text");
}

void FileCase(const std::wstring& product) {
    wchar_t temporaryDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporaryDirectory);
    Require(length != 0 && length < MAX_PATH, "Temporary directory is unavailable");
    const auto path = std::wstring(temporaryDirectory) + L"VisPick-console-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".txt";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    Require(file.Valid(), "Could not create redirected output file");
    auto child = Start(product, L"--help", 0, true, file.Get(), file.Get());
    Require(Finish(child) == 0, "Redirected help returned an error");
    LARGE_INTEGER start{};
    Require(SetFilePointerEx(file.Get(), start, nullptr, FILE_BEGIN) != FALSE, "File rewind failed");
    const auto text = DecodeUtf8(ReadBytes(file.Get()));
    Require(text.find(L"VisPick") != std::wstring::npos && HasChinese(text), "Redirected help lost its text/encoding");
}

void PipeCase(const std::wstring& product) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    Require(CreatePipe(&readHandle, &writeHandle, &security, 65536) != FALSE, "CreatePipe failed");
    Handle reader(readHandle);
    Handle writer(writeHandle);
    Require(SetHandleInformation(reader.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE, "Pipe inheritance setup failed");
    auto child = Start(product, L"--未知参数", 0, true, writer.Get(), writer.Get());
    writer.Reset();
    Require(Finish(child) == 2, "Unknown argument did not return exit code 2");
    const auto text = DecodeUtf8(ReadBytes(reader.Get()));
    Require(text.find(L"--未知参数") != std::wstring::npos && text.find(L"--help") != std::wstring::npos,
        "UTF-8 pipe error omitted/corrupted the Chinese argument");
}

struct DialogText { DWORD process; std::wstring text; bool failed = false; };
BOOL CALLBACK ReadDialogChild(HWND window, LPARAM data) noexcept {
    auto& result = *reinterpret_cast<DialogText*>(data);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != result.process) { return TRUE; }
    try {
        wchar_t text[4096]{};
        DWORD_PTR characters = 0;
        if (SendMessageTimeoutW(window, WM_GETTEXT, 4096, reinterpret_cast<LPARAM>(text),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &characters)) {
            result.text += text;
        }
    } catch (...) { result.failed = true; return FALSE; }
    return TRUE;
}

void DetachedCase(const std::wstring& product) {
    // A detached helper is essential: AttachConsole must not find our main runner's console.
    auto child = Start(product, L"--help", DETACHED_PROCESS, true);
    // EnumWindows can find the dialog before MessageBox initializes its STATIC
    // text. Wait for its input loop, then still poll the content with a deadline.
    (void)WaitForInputIdle(child.process.Get(), 2000);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    HWND dialog = nullptr;
    DialogText content{child.identifier, {}, false};
    bool contentReady = false;
    while (GetTickCount64() < deadline && WaitForSingleObject(child.process.Get(), 0) == WAIT_TIMEOUT) {
        dialog = OwnedDialog(child.identifier);
        if (dialog != nullptr) {
            content.text.clear();
            content.failed = false;
            EnumChildWindows(dialog, ReadDialogChild, reinterpret_cast<LPARAM>(&content));
            contentReady = !content.failed && content.text.find(L"VisPick") != std::wstring::npos && HasChinese(content.text);
            if (contentReady) { break; }
        }
        Sleep(10);
    }
    CloseOwnedDialog(child.identifier);
    const DWORD status = Finish(child);
    Require(dialog != nullptr, "No-console help did not show its own MessageBox");
    Require(status == 0, "Detached help did not exit cleanly after closing its own dialog");
    if (!contentReady) {
        throw std::runtime_error("No-console dialog text did not become ready: length=" +
            std::to_string(content.text.size()) + ", contains VisPick=" +
            std::to_string(content.text.find(L"VisPick") != std::wstring::npos) +
            ", contains Chinese=" + std::to_string(HasChinese(content.text)));
    }
}

} // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    try {
        if (argumentCount == 5 && std::wstring_view(arguments[1]) == L"--helper") {
            const std::wstring mode = arguments[2];
            const std::wstring product = arguments[3];
            if (mode == L"console") { ConsoleCase(product); }
            else if (mode == L"file") { FileCase(product); }
            else if (mode == L"pipe") { PipeCase(product); }
            else if (mode == L"detached") { DetachedCase(product); }
            else { throw std::runtime_error("Unknown console test helper mode"); }
            return 0;
        }
        Require(argumentCount <= 2, "Usage: VisPickConsoleTests.exe [path-to-VisPick.exe]");
        const std::wstring self = SelfPath();
        const std::wstring product = argumentCount == 2 ? std::filesystem::absolute(arguments[1]).wstring() :
            (std::filesystem::path(self).parent_path() / L"VisPick.exe").wstring();
        bool passed = true;
        for (const auto* mode : {L"console", L"file", L"pipe", L"detached"}) {
            // Hidden real consoles exercise AttachConsole and redirection preservation.
            // The detached helper instead models a double click from Explorer.
            DiagnosticLog diagnostic;
            auto helper = Start(self, L"--helper " + std::wstring(mode) + L" " + Quote(product) + L" " + Quote(diagnostic.Path()),
                std::wstring_view(mode) == L"detached" ? DETACHED_PROCESS : CREATE_NEW_CONSOLE);
            const DWORD status = Finish(helper, 15000);
            std::wprintf(L"[%ls] %ls (exit %lu)\n", status == 0 ? L"PASS" : L"FAIL", mode, status);
            if (status != 0) { std::wprintf(L"  %ls\n", diagnostic.Read().c_str()); }
            passed = passed && status == 0;
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        if (argumentCount == 5 && std::wstring_view(arguments[1]) == L"--helper") {
            WriteDiagnostic(arguments[4], error.what());
        }
        std::fprintf(stderr, "Console integration failure: %s\n", error.what());
        return 10;
    }
}
