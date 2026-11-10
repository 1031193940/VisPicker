#pragma once

#include <Windows.h>
#include <filesystem>
#include <span>
#include <string_view>

namespace vispick {

enum class LogLevel { Info, Warning, Error };

struct LogStatus {
    bool enabled = false;
    bool usingFallback = false;
    DWORD error = ERROR_SUCCESS;
    std::filesystem::path path;
};

// No file, worker thread or periodic flush exists while disabled (the default).
// Configure, Write and Status may be called concurrently. Object destruction
// requires its callers to have stopped, like other synchronization-owning objects.
class Log final {
public:
    Log() noexcept = default;
    ~Log();
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // Empty directories selects exe, APPDATA/VisPick, then TEMP/VisPick.
    // Explicit candidates provide the same ordered fallback policy for tests.
    [[nodiscard]] LogStatus Configure(bool enabled, std::span<const std::filesystem::path> directories = {});
    void Write(LogLevel level, std::wstring_view message) noexcept;
    void WriteUtf8(LogLevel level, std::string_view message) noexcept;
    [[nodiscard]] LogStatus Status() const;

private:
    void Close() noexcept;
    void WriteRecord(LogLevel level, std::string_view message);
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    LogStatus status_;
};

} // namespace vispick
