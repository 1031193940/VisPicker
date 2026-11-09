#pragma once

#include <Windows.h>
#include <filesystem>
#include <span>

namespace vispick {

struct CrashDumpStatus {
    bool installed = false;
    DWORD error = ERROR_SUCCESS;
};

// Owns the process-wide top-level exception filter. Install/uninstall on the
// application lifecycle thread, before starting/after stopping worker threads.
// Only one instance may be installed. Installation creates no files and does
// not load dbghelp; the in-process dump is best effort after an actual fault.
class CrashDump final {
public:
    CrashDump() noexcept = default;
    ~CrashDump();
    CrashDump(const CrashDump&) = delete;
    CrashDump& operator=(const CrashDump&) = delete;
    [[nodiscard]] CrashDumpStatus Install(std::span<const std::filesystem::path> directories = {}) noexcept;
    void Uninstall() noexcept;
    [[nodiscard]] CrashDumpStatus Status() const noexcept { return status_; }
private:
    CrashDumpStatus status_;
};

} // namespace vispick
