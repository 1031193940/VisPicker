#include "platform/Autostart.h"

// SDK 10.0.22621 marks RegOpenKeyExW's DWORD ulOptions as _In_opt_.
// Limit suppression of its invalid SAL annotation to the SDK declarations.
#pragma warning(push)
#pragma warning(disable : 6553)
#include <windows.h>
#pragma warning(pop)
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace vispick {
namespace {

class RegistryKey {
public:
    ~RegistryKey() {
        if (handle_) { RegCloseKey(handle_); }
    }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    [[nodiscard]] HKEY Get() const noexcept { return handle_; }
    [[nodiscard]] HKEY* Address() noexcept { return &handle_; }

private:
    HKEY handle_ = nullptr;
};

void CheckRegistry(const LSTATUS status, const char* operation) {
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(status) + ")");
    }
}

bool Open(const std::wstring& keyPath, const REGSAM access, RegistryKey& key) {
    const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, access | KEY_WOW64_64KEY, key.Address());
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) { return false; }
    CheckRegistry(status, "Open autostart registry key");
    return true;
}

std::optional<std::wstring> ReadCommand(const HKEY key, const std::wstring& valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    auto status = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) { return std::nullopt; }
    CheckRegistry(status, "Size autostart command");
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0 || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        throw std::runtime_error("Invalid autostart registry string");
    }
    std::vector<wchar_t> text(bytes / sizeof(wchar_t));
    status = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &bytes);
    if (status == ERROR_FILE_NOT_FOUND) { return std::nullopt; }
    CheckRegistry(status, "Read autostart command");
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0 || bytes % sizeof(wchar_t) != 0 ||
        bytes > text.size() * sizeof(wchar_t)) {
        throw std::runtime_error("Autostart registry string changed while reading");
    }
    const std::size_t characterCount = bytes / sizeof(wchar_t);
    if (text[characterCount - 1] != L'\0' ||
        std::find(text.begin(), text.begin() + characterCount - 1, L'\0') != text.begin() + characterCount - 1) {
        throw std::runtime_error("Autostart registry string is not correctly terminated");
    }
    return std::wstring(text.data(), characterCount - 1);
}

std::wstring QuoteArgument(const std::wstring_view argument) {
    if (argument.find(L'\0') != std::wstring_view::npos) {
        throw std::invalid_argument("Autostart path contains a null character");
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(character);
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

void WriteCommand(const HKEY key, const std::wstring& valueName, const std::wstring& command) {
    // The documented Run/RunOnce limit applies to the command, not just the exe.
    if (command.size() > 260) {
        throw std::length_error("Autostart command exceeds the Windows Run key limit of 260 characters");
    }
    CheckRegistry(RegSetValueExW(key, valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                 static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))),
                  "Write autostart command");
}

} // namespace

Autostart::Autostart(std::wstring keyPath, std::wstring valueName)
    : keyPath_(std::move(keyPath)), valueName_(std::move(valueName)) {
    if (keyPath_.empty() || valueName_.empty() || keyPath_.find(L'\0') != std::wstring::npos ||
        valueName_.find(L'\0') != std::wstring::npos) {
        throw std::invalid_argument("Autostart registry key and value must be nonempty strings");
    }
}

std::optional<std::wstring> Autostart::Command() const {
    RegistryKey key;
    if (!Open(keyPath_, KEY_QUERY_VALUE, key)) { return std::nullopt; }
    return ReadCommand(key.Get(), valueName_);
}

bool Autostart::Enabled() const {
    const auto command = Command();
    return command && !command->empty();
}

std::wstring Autostart::BuildCommand(const std::filesystem::path& executable, const std::filesystem::path& config) {
    if (executable.empty()) { throw std::invalid_argument("Autostart executable path is empty"); }
    // filesystem::absolute delegates to Win32, which would silently truncate an
    // embedded null before QuoteArgument gets a chance to reject it.
    if (executable.native().find(L'\0') != std::wstring::npos || config.native().find(L'\0') != std::wstring::npos) {
        throw std::invalid_argument("Autostart path contains a null character");
    }
    auto command = QuoteArgument(std::filesystem::absolute(executable).lexically_normal().native());
    if (!config.empty()) {
        command += L" --config " + QuoteArgument(std::filesystem::absolute(config).lexically_normal().native());
    }
    return command;
}

void Autostart::SetEnabled(const bool enabled, const std::filesystem::path& executable,
                           const std::filesystem::path& config) const {
    RegistryKey key;
    if (!enabled) {
        if (Open(keyPath_, KEY_SET_VALUE, key)) {
            const auto status = RegDeleteValueW(key.Get(), valueName_.c_str());
            if (status != ERROR_FILE_NOT_FOUND) { CheckRegistry(status, "Delete autostart command"); }
        }
        return;
    }
    const auto command = BuildCommand(executable, config);
    // Reject before creating a key or changing the existing value.
    if (command.size() > 260) {
        throw std::length_error("Autostart command exceeds the Windows Run key limit of 260 characters");
    }
    CheckRegistry(RegCreateKeyExW(HKEY_CURRENT_USER, keyPath_.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, key.Address(), nullptr),
                  "Create autostart registry key");
    WriteCommand(key.Get(), valueName_, command);
}

bool Autostart::RepairIfEnabled(const std::filesystem::path& executable) const {
    RegistryKey key;
    if (!Open(keyPath_, KEY_QUERY_VALUE | KEY_SET_VALUE, key)) { return false; }
    const auto current = ReadCommand(key.Get(), valueName_);
    if (!current || current->empty()) { return false; }
    int count = 0;
    const auto freeArguments = [](wchar_t** arguments) noexcept { LocalFree(arguments); };
    const std::unique_ptr<wchar_t*, decltype(freeArguments)> arguments(CommandLineToArgvW(current->c_str(), &count),
                                                                       freeArguments);
    if (!arguments || (count != 1 && count != 3)) { return false; }
    const std::filesystem::path oldExecutable(arguments.get()[0]);
    std::filesystem::path savedConfig;
    if (count == 3) {
        if (std::wstring_view(arguments.get()[1]) != L"--config") { return false; }
        savedConfig = arguments.get()[2];
        if (!savedConfig.is_absolute()) { return false; }
    }
    if (!oldExecutable.is_absolute() || oldExecutable.native().find(L'%') != std::wstring::npos ||
        _wcsicmp(oldExecutable.filename().c_str(), executable.filename().c_str()) != 0 ||
        *current != BuildCommand(oldExecutable, savedConfig)) {
        return false;
    }
    if (GetFileAttributesW(oldExecutable.c_str()) != INVALID_FILE_ATTRIBUTES) { return false; }
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) { return false; }
    const auto command = BuildCommand(executable, savedConfig);
    if (*current == command) { return false; }
    WriteCommand(key.Get(), valueName_, command);
    return true;
}

} // namespace vispick
