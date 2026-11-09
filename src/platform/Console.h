#pragma once

#include <windows.h>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace vispick {

[[nodiscard]] std::string WideToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring Utf8ToWide(std::string_view text);
// MSVC/system exceptions can contain text in the current Windows ANSI code page.
[[nodiscard]] std::wstring ExceptionMessage(const std::exception& error);
[[nodiscard]] std::vector<std::wstring> ProcessArguments();

class ConsoleOutput {
public:
    ConsoleOutput() noexcept;
    ~ConsoleOutput();
    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;
    // Console handles receive UTF-16; redirected files and pipes receive UTF-8.
    [[nodiscard]] bool Write(std::wstring_view text, bool error = false) const;
    void Show(std::wstring_view text, bool error = false) const;
private:
    HANDLE originalOutput_ = nullptr;
    HANDLE originalError_ = nullptr;
    HANDLE ownedConsole_ = INVALID_HANDLE_VALUE;
};

} // namespace vispick
