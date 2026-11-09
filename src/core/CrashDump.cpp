#include "core/CrashDump.h"
#include "core/Paths.h"

#include <DbgHelp.h>
#include <strsafe.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

namespace vispick {
namespace {

constexpr std::size_t PathCapacity = 1024;
constexpr std::size_t DirectoryCapacity = 3;
using DumpFunction = decltype(&MiniDumpWriteDump);

// Static storage outlives every CrashDump owner, including a concurrent filter
// entry during shutdown. No std::string/filesystem/Log/mutex is used by a fault.
struct CrashState {
    std::atomic<bool> installed{false};
    std::atomic<DWORD> faultThread{0};
    std::atomic<unsigned> activeFilters{0};
    HANDLE dumpCompleted = nullptr;
    std::array<std::array<wchar_t, PathCapacity>, DirectoryCapacity> directories{};
    std::size_t directoryCount = 0;
    std::array<wchar_t, PathCapacity> libraryPath{};
    std::array<wchar_t, PathCapacity> dumpPath{};
    std::array<wchar_t, PathCapacity + 256> message{};
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    std::terminate_handler previousTerminate = nullptr;
    _invalid_parameter_handler previousInvalidParameter = nullptr;
    _purecall_handler previousPureCall = nullptr;
    void (*previousAbort)(int) = SIG_DFL;
};

CrashState state;
SRWLOCK installationLock = SRWLOCK_INIT;
CrashDump* owner = nullptr;

class InstallationGuard final {
public:
    InstallationGuard() noexcept { AcquireSRWLockExclusive(&installationLock); }
    ~InstallationGuard() { ReleaseSRWLockExclusive(&installationLock); }
    InstallationGuard(const InstallationGuard&) = delete;
    InstallationGuard& operator=(const InstallationGuard&) = delete;
};

class DumpFile final {
public:
    explicit DumpFile(HANDLE handle) noexcept : handle_(handle) {}
    ~DumpFile() { if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    DumpFile(const DumpFile&) = delete;
    DumpFile& operator=(const DumpFile&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
private:
    HANDLE handle_;
};

class DumpLibrary final {
public:
    explicit DumpLibrary(HMODULE module) noexcept : module_(module) {}
    ~DumpLibrary() { if (module_ != nullptr) { FreeLibrary(module_); } }
    DumpLibrary(const DumpLibrary&) = delete;
    DumpLibrary& operator=(const DumpLibrary&) = delete;
    [[nodiscard]] HMODULE Get() const noexcept { return module_; }
private:
    HMODULE module_;
};

DWORD WriteDump(EXCEPTION_POINTERS* exception, bool& usingFallback) noexcept {
    const DumpLibrary library(LoadLibraryW(state.libraryPath.data()));
    if (library.Get() == nullptr) { return GetLastError(); }
    const auto address = GetProcAddress(library.Get(), "MiniDumpWriteDump");
    if (address == nullptr) { return GetLastError(); }
    DumpFunction writeDump = nullptr;
    static_assert(sizeof(writeDump) == sizeof(address));
    std::memcpy(&writeDump, &address, sizeof(writeDump));
    SYSTEMTIME time{};
    GetLocalTime(&time);
    DWORD error = ERROR_PATH_NOT_FOUND;
    for (std::size_t directoryIndex = 0; directoryIndex < state.directoryCount; ++directoryIndex) {
        const auto* directory = state.directories[directoryIndex].data();
        if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = GetLastError();
            continue;
        }
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            const auto formatted = StringCchPrintfW(state.dumpPath.data(), state.dumpPath.size(),
                L"%s\\VisPick-crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%lu-%u.dmp", directory,
                static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                static_cast<unsigned>(time.wMilliseconds), GetCurrentProcessId(), GetCurrentThreadId(), attempt);
            if (FAILED(formatted)) { error = ERROR_FILENAME_EXCED_RANGE; break; }
            bool success = false;
            {
                const DumpFile file(CreateFileW(state.dumpPath.data(), GENERIC_WRITE, FILE_SHARE_READ,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (file.Get() == INVALID_HANDLE_VALUE) {
                    error = GetLastError();
                    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) { continue; }
                    break;
                }
                MINIDUMP_EXCEPTION_INFORMATION details{GetCurrentThreadId(), exception, FALSE};
                std::size_t pathLength = 0;
                static_cast<void>(StringCchLengthW(state.dumpPath.data(), state.dumpPath.size(), &pathLength));
                MINIDUMP_USER_STREAM pathStream{CommentStreamW,
                    static_cast<ULONG>((pathLength + 1) * sizeof(wchar_t)), state.dumpPath.data()};
                MINIDUMP_USER_STREAM_INFORMATION streams{1, &pathStream};
                success = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file.Get(), MiniDumpNormal,
                    &details, &streams, nullptr) != FALSE;
                error = success ? ERROR_SUCCESS : GetLastError();
                if (!success && error == ERROR_SUCCESS) { error = ERROR_WRITE_FAULT; }
            }
            if (success) { usingFallback = directoryIndex != 0; return ERROR_SUCCESS; }
            // A failed/partial dump is not presented as a usable diagnostic.
            DeleteFileW(state.dumpPath.data());
            break;
        }
    }
    state.dumpPath[0] = L'\0';
    return error;
}

LONG HandleException(EXCEPTION_POINTERS* exception) noexcept {
    bool usingFallback = false;
    const DWORD error = WriteDump(exception, usingFallback);
    if (error == ERROR_SUCCESS) {
        static_cast<void>(StringCchPrintfW(state.message.data(), state.message.size(),
            L"VisPick 遇到未处理的异常，即将退出。\n崩溃转储已保存%s：\n%s",
            usingFallback ? L"到备用目录" : L"", state.dumpPath.data()));
    } else {
        static_cast<void>(StringCchPrintfW(state.message.data(), state.message.size(),
            L"VisPick 遇到未处理的异常，即将退出。\n无法写入崩溃转储，错误代码：0x%08lX", error));
    }
    OutputDebugStringW(state.message.data());
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* exception) noexcept {
    ++state.activeFilters;
    if (!state.installed.load()) { --state.activeFilters; return EXCEPTION_CONTINUE_SEARCH; }
    const auto currentThread = GetCurrentThreadId();
    DWORD expected = 0;
    if (!state.faultThread.compare_exchange_strong(expected, currentThread)) {
        // Do not let a second crashing thread terminate the process mid-write.
        if (expected != currentThread) { WaitForSingleObject(state.dumpCompleted, 5000); }
        --state.activeFilters;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    // SEH is contained separately from RAII functions: the crashing process may
    // have invalid memory/stack and DbgHelp itself may fault. No C++ escape.
    LONG disposition = EXCEPTION_EXECUTE_HANDLER;
    __try { disposition = HandleException(exception); }
    __except (GetExceptionCode() == EXCEPTION_BREAKPOINT || GetExceptionCode() == EXCEPTION_SINGLE_STEP
        ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringW(L"VisPick crash dump handler failed while processing the original exception.");
    }
    SetEvent(state.dumpCompleted);
    --state.activeFilters;
    return disposition;
}

[[noreturn]] void ReportFatal(DWORD code) noexcept {
    __try { RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
    __except (ExceptionFilter(GetExceptionInformation())) {
        OutputDebugStringW(L"VisPick is terminating after a fatal runtime error.\n");
    }
    TerminateProcess(GetCurrentProcess(), code);
    __assume(0);
}

void ReportTerminate() noexcept { ReportFatal(0xE0421002UL); }
void ReportAbort(int) noexcept { ReportFatal(0xE0421003UL); }
void ReportInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) noexcept {
    ReportFatal(0xE0421004UL);
}
void ReportPureCall() noexcept { ReportFatal(0xE0421005UL); }

} // namespace

CrashDump::~CrashDump() { Uninstall(); }

CrashDumpStatus CrashDump::Install(std::span<const std::filesystem::path> directories) noexcept {
    const InstallationGuard guard;
    if (owner == this) { return status_; }
    if (owner != nullptr) { status_ = {false, ERROR_ALREADY_EXISTS}; return status_; }
    try {
        const auto defaults = directories.empty() ? DiagnosticDirectories() : std::vector<std::filesystem::path>{};
        if (directories.empty()) { directories = defaults; }
        if (directories.empty() || directories.size() > DirectoryCapacity) {
            status_ = {false, ERROR_INVALID_PARAMETER};
            return status_;
        }
        const UINT length = GetSystemDirectoryW(state.libraryPath.data(), static_cast<UINT>(state.libraryPath.size()));
        if (length == 0 || length >= state.libraryPath.size() ||
            FAILED(StringCchCatW(state.libraryPath.data(), state.libraryPath.size(), L"\\dbghelp.dll"))) {
            status_ = {false, length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE};
            return status_;
        }
        state.directoryCount = 0;
        for (const auto& candidate : directories) {
            const auto path = std::filesystem::absolute(candidate).native();
            if (path.size() + 100 >= PathCapacity || path.find(L'\0') != std::wstring::npos) {
                status_ = {false, ERROR_FILENAME_EXCED_RANGE};
                return status_;
            }
            std::copy(path.begin(), path.end(), state.directories[state.directoryCount].begin());
            state.directories[state.directoryCount][path.size()] = L'\0';
            ++state.directoryCount;
        }
        state.dumpCompleted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state.dumpCompleted) { status_ = {false, GetLastError()}; return status_; }
        ULONG guaranteedStackBytes = 64 * 1024;
        SetThreadStackGuarantee(&guaranteedStackBytes);
        state.faultThread.store(0);
        state.dumpPath[0] = L'\0';
        state.previous = SetUnhandledExceptionFilter(ExceptionFilter);
        state.previousTerminate = std::set_terminate(ReportTerminate);
        state.previousAbort = std::signal(SIGABRT, ReportAbort);
        state.previousInvalidParameter = _set_invalid_parameter_handler(ReportInvalidParameter);
        state.previousPureCall = _set_purecall_handler(ReportPureCall);
        owner = this;
        state.installed.store(true);
        status_ = {true, ERROR_SUCCESS};
    } catch (const std::filesystem::filesystem_error& error) {
        status_ = {false, static_cast<DWORD>(error.code().value())};
    } catch (...) { status_ = {false, ERROR_NOT_ENOUGH_MEMORY}; }
    return status_;
}

void CrashDump::Uninstall() noexcept {
    const InstallationGuard guard;
    if (owner != this) { return; }
    state.installed.store(false);
    SetUnhandledExceptionFilter(state.previous);
    std::set_terminate(state.previousTerminate);
    if (state.previousAbort != SIG_ERR) { std::signal(SIGABRT, state.previousAbort); }
    _set_invalid_parameter_handler(state.previousInvalidParameter);
    _set_purecall_handler(state.previousPureCall);
    // Lifecycle callers have already joined workers. A late entrant cannot use
    // this event after installed is cleared.
    while (state.activeFilters.load() != 0) { Sleep(1); }
    CloseHandle(state.dumpCompleted);
    state.dumpCompleted = nullptr;
    owner = nullptr;
    status_.installed = false;
}

} // namespace vispick
