#include "platform/Console.h"

#include <shellapi.h>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace vispick {
namespace {

int TextLength(std::size_t length) {
    if (length > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("Text exceeds Windows conversion limit");
    }
    return static_cast<int>(length);
}

std::wstring Decode(std::string_view text, UINT codePage, DWORD flags) {
    if (text.empty()) { return {}; }
    const int length = TextLength(text.size());
    const int count = MultiByteToWideChar(codePage, flags, text.data(), length, nullptr, 0);
    if (count == 0) { throw std::invalid_argument("Invalid text encoding"); }
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(codePage, flags, text.data(), length, converted.data(), count) != count) {
        throw std::runtime_error("Text conversion failed");
    }
    return converted;
}

bool ValidHandle(HANDLE handle) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) { return false; }
    SetLastError(ERROR_SUCCESS);
    return GetFileType(handle) != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
}

struct LocalArgumentsDeleter {
    void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
};

} // namespace

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) { return {}; }
    const int length = TextLength(text.size());
    const int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (count == 0) { throw std::invalid_argument("Invalid UTF-16 text"); }
    std::string converted(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, converted.data(), count, nullptr,
                            nullptr) != count) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return converted;
}

std::wstring Utf8ToWide(std::string_view text) {
    return Decode(text, CP_UTF8, MB_ERR_INVALID_CHARS);
}
std::wstring ExceptionMessage(const std::exception& error) {
    // Recover filesystem paths from their native UTF-16 representation rather
    // than guessing the narrow encoding selected by filesystem_error::what().
    if (const auto* fileError = dynamic_cast<const std::filesystem::filesystem_error*>(&error)) {
        std::wstring message = Decode(fileError->code().message(), CP_ACP, 0);
        if (!fileError->path1().empty()) { message += L"\n" + fileError->path1().native(); }
        if (!fileError->path2().empty()) { message += L"\n" + fileError->path2().native(); }
        return message;
    }
    return Decode(error.what(), CP_ACP, 0);
}

std::vector<std::wstring> ProcessArguments() {
    int count = 0;
    std::unique_ptr<wchar_t*, LocalArgumentsDeleter> arguments(CommandLineToArgvW(GetCommandLineW(), &count));
    if (!arguments) { throw std::runtime_error("CommandLineToArgvW failed"); }
    std::vector<std::wstring> result;
    for (int argumentIndex = 1; argumentIndex < count; ++argumentIndex) {
        result.emplace_back(arguments.get()[argumentIndex]);
    }
    return result;
}

ConsoleOutput::ConsoleOutput() noexcept
    : originalOutput_(GetStdHandle(STD_OUTPUT_HANDLE)), originalError_(GetStdHandle(STD_ERROR_HANDLE)) {
    const bool preserveOutput = ValidHandle(originalOutput_);
    const bool preserveError = ValidHandle(originalError_);
    AttachConsole(ATTACH_PARENT_PROCESS);
    // AttachConsole may replace standard handles. Never lose caller redirection.
    if (preserveOutput) { SetStdHandle(STD_OUTPUT_HANDLE, originalOutput_); }
    if (preserveError) { SetStdHandle(STD_ERROR_HANDLE, originalError_); }
    if (!ValidHandle(GetStdHandle(STD_OUTPUT_HANDLE)) || !ValidHandle(GetStdHandle(STD_ERROR_HANDLE))) {
        ownedConsole_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (ownedConsole_ != INVALID_HANDLE_VALUE) {
            if (!ValidHandle(GetStdHandle(STD_OUTPUT_HANDLE))) { SetStdHandle(STD_OUTPUT_HANDLE, ownedConsole_); }
            if (!ValidHandle(GetStdHandle(STD_ERROR_HANDLE))) { SetStdHandle(STD_ERROR_HANDLE, ownedConsole_); }
        }
    }
}

ConsoleOutput::~ConsoleOutput() {
    if (ownedConsole_ != INVALID_HANDLE_VALUE) {
        if (GetStdHandle(STD_OUTPUT_HANDLE) == ownedConsole_) { SetStdHandle(STD_OUTPUT_HANDLE, originalOutput_); }
        if (GetStdHandle(STD_ERROR_HANDLE) == ownedConsole_) { SetStdHandle(STD_ERROR_HANDLE, originalError_); }
        CloseHandle(ownedConsole_);
    }
}

bool ConsoleOutput::Write(std::wstring_view text, bool error) const {
    const HANDLE output = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!ValidHandle(output)) { return false; }
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        while (!text.empty()) {
            auto length = static_cast<DWORD>(std::min<std::size_t>(16384, text.size()));
            if (length < text.size() && text[length - 1] >= 0xD800 && text[length - 1] <= 0xDBFF) { --length; }
            DWORD written = 0;
            if (!WriteConsoleW(output, text.data(), length, &written, nullptr) || written == 0) { return false; }
            text.remove_prefix(written);
        }
        return true;
    }
    const auto encoded = WideToUtf8(text);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const auto length = static_cast<DWORD>(std::min<std::size_t>(65536, encoded.size() - offset));
        DWORD written = 0;
        if (!WriteFile(output, encoded.data() + offset, length, &written, nullptr) || written == 0) { return false; }
        offset += written;
    }
    return true;
}

void ConsoleOutput::Show(std::wstring_view text, bool error) const {
    if (Write(text, error)) { return; }
    const std::wstring terminated(text);
    MessageBoxW(nullptr, terminated.c_str(), L"VisPick", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

} // namespace vispick
