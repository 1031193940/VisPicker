#include "core/Log.h"
#include "core/Paths.h"
#include "platform/Console.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace vispick {
namespace {

class ExclusiveLock final {
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& lock_;
};

const char* LevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

class PendingLogFile final {
public:
    explicit PendingLogFile(HANDLE file) noexcept : file_(file) {}
    ~PendingLogFile() { if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); } }
    PendingLogFile(const PendingLogFile&) = delete;
    PendingLogFile& operator=(const PendingLogFile&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return file_; }
    [[nodiscard]] HANDLE Release() noexcept { const auto file = file_; file_ = INVALID_HANDLE_VALUE; return file; }
private:
    HANDLE file_;
};

} // namespace

Log::~Log() { Close(); }

void Log::Close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    status_.enabled = false;
}

LogStatus Log::Configure(bool enabled, std::span<const std::filesystem::path> directories) {
    const ExclusiveLock guard(lock_);
    if (!enabled) {
        Close();
        status_ = {};
        return status_;
    }
    // Hot reload of unrelated settings must not reopen an already enabled log.
    if (status_.enabled && directories.empty()) { return status_; }
    Close();
    status_ = {};
    const auto defaults = directories.empty() ? DiagnosticDirectories() : std::vector<std::filesystem::path>{};
    if (directories.empty()) { directories = defaults; }
    status_.error = ERROR_PATH_NOT_FOUND;
    for (std::size_t directoryIndex = 0; directoryIndex < directories.size(); ++directoryIndex) {
        std::error_code error;
        const auto directory = std::filesystem::absolute(directories[directoryIndex], error);
        if (error) { status_.error = static_cast<DWORD>(error.value()); continue; }
        std::filesystem::create_directories(directory, error);
        if (error) { status_.error = static_cast<DWORD>(error.value()); continue; }
        const auto path = directory / L"VisPick.log";
        PendingLogFile file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE) { status_.error = GetLastError(); continue; }
        // Assign allocating state before taking ownership, so allocation failure
        // cannot leak the newly opened handle.
        status_.path = path;
        file_ = file.Release();
        status_.enabled = true;
        status_.usingFallback = directoryIndex != 0;
        status_.error = ERROR_SUCCESS;
        WriteRecord(LogLevel::Info, status_.usingFallback ?
            "Logging enabled in a fallback directory; see the reported log path." : "Logging enabled.");
        return status_;
    }
    return status_;
}

void Log::WriteRecord(LogLevel level, std::string_view message) {
    if (!status_.enabled) { return; }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[80]{};
    const int length = std::snprintf(prefix, sizeof(prefix),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
        static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds), LevelName(level));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(prefix)) {
        status_.error = ERROR_INVALID_DATA;
        return;
    }
    std::string record(prefix, static_cast<std::size_t>(length));
    // Preserve one physical line per record, including text supplied by errors
    // or configuration files. UTF-8 multibyte sequences are otherwise untouched.
    for (const char character : message) {
        switch (character) {
        case '\r': record += "\\r"; break;
        case '\n': record += "\\n"; break;
        case '\0': record += "\\0"; break;
        default: record += character; break;
        }
    }
    record += "\r\n";
    if (record.size() > std::numeric_limits<DWORD>::max()) { status_.error = ERROR_FILE_TOO_LARGE; return; }
    DWORD written = 0;
    // Buffered by the OS; deliberately no FlushFileBuffers on each record and
    // no timer. Explicit log enablement is the only source of file I/O.
    if (!WriteFile(file_, record.data(), static_cast<DWORD>(record.size()), &written, nullptr)) {
        status_.error = GetLastError();
        Close();
    } else if (written != record.size()) {
        status_.error = ERROR_WRITE_FAULT;
        Close();
    }
}

void Log::Write(LogLevel level, std::wstring_view message) noexcept {
    const ExclusiveLock guard(lock_);
    if (!status_.enabled) { return; }
    try { WriteRecord(level, WideToUtf8(message)); }
    catch (const std::invalid_argument&) { status_.error = ERROR_NO_UNICODE_TRANSLATION; }
    catch (...) { status_.error = ERROR_NOT_ENOUGH_MEMORY; }
}

void Log::WriteUtf8(LogLevel level, std::string_view message) noexcept {
    const ExclusiveLock guard(lock_);
    if (!status_.enabled) { return; }
    if (message.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        status_.error = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!message.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        message.data(), static_cast<int>(message.size()), nullptr, 0) == 0) {
        status_.error = ERROR_NO_UNICODE_TRANSLATION;
        return;
    }
    try { WriteRecord(level, message); }
    catch (...) { status_.error = ERROR_NOT_ENOUGH_MEMORY; }
}

LogStatus Log::Status() const {
    const ExclusiveLock guard(lock_);
    return status_;
}

} // namespace vispick
