#pragma once

#include "core/AppCommand.h"
#include <windows.h>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vispick {

enum class ForwardResult { Delivered, ConfigConflict, Rejected, TimedOut, Unavailable };

class SingleInstance {
public:
    static constexpr const wchar_t* MainWindowClass = L"VisPick.Main";
    static constexpr ULONG_PTR CopyDataTag = 0x56535031;
    static constexpr LRESULT Accepted = 1;
    static constexpr LRESULT ConfigConflict = 2;
    static constexpr LRESULT Rejected = 0;
    static constexpr DWORD MaximumPayloadBytes = 64 * 1024;

    explicit SingleInstance(std::wstring identity = L"VisPick");
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    [[nodiscard]] bool IsPrimary() const noexcept { return primary_; }
    [[nodiscard]] const std::wstring& WindowTitle() const noexcept { return windowTitle_; }
    [[nodiscard]] ForwardResult Forward(const LaunchOptions& options, DWORD timeoutMilliseconds = 5000) const;

    [[nodiscard]] static std::vector<std::byte> Encode(const LaunchOptions& options);
    // Copies all validated UTF-16 data before returning. App must enqueue this
    // owned result and PostMessage before returning Accepted from WM_COPYDATA.
    [[nodiscard]] static std::optional<LaunchOptions> Decode(const COPYDATASTRUCT& data);
    [[nodiscard]] static bool ConfigMatches(const std::filesystem::path& requested,
                                            const std::filesystem::path& active);

private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
    std::wstring windowTitle_;
};

} // namespace vispick
