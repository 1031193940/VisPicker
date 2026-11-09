#include "core/Paths.h"

#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace vispick {
namespace {

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~FileHandle() { if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[noreturn]] void PathError(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

std::filesystem::path KnownDirectory(int identifier) {
    std::array<wchar_t, MAX_PATH> buffer{};
    const HRESULT result = SHGetFolderPathW(nullptr, identifier, nullptr, SHGFP_TYPE_CURRENT, buffer.data());
    if (FAILED(result)) { PathError("SHGetFolderPathW", static_cast<DWORD>(result)); }
    return buffer.data();
}

bool EnsureConfig(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    const FileHandle output(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.Get() == INVALID_HANDLE_VALUE) {
        const DWORD creationError = GetLastError();
        if (creationError != ERROR_FILE_EXISTS && creationError != ERROR_ALREADY_EXISTS) { PathError("Create config", creationError); }
        const FileHandle input(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (input.Get() == INVALID_HANDLE_VALUE) { PathError("Read existing config"); }
        return false;
    }
    if (bytes.size() > std::numeric_limits<DWORD>::max()) { throw std::length_error("Default configuration is too large"); }
    DWORD written = 0;
    if (!WriteFile(output.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
        const DWORD error = GetLastError();
        // Mark only the newly created file for deletion, while the exclusive handle owns it.
        FILE_DISPOSITION_INFO disposition{TRUE};
        static_cast<void>(SetFileInformationByHandle(output.Get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        PathError("Write default config", error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error);
    }
    return true;
}

std::wstring EnvironmentValue(std::wstring_view name) {
    if (name.empty()) { throw std::invalid_argument("Empty environment variable"); }
    const std::wstring terminated(name);
    const DWORD required = GetEnvironmentVariableW(terminated.c_str(), nullptr, 0);
    if (required == 0) { throw std::invalid_argument("Unknown or empty environment variable"); }
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(terminated.c_str(), value.data(), required);
    if (length == 0 || length >= required) { throw std::invalid_argument("Environment variable changed during expansion"); }
    value.resize(length);
    return value;
}

template<typename Resolve>
std::wstring ExpandBraces(std::wstring_view pattern, Resolve resolve) {
    std::wstring result;
    for (std::size_t offset = 0; offset < pattern.size();) {
        if (offset + 1 < pattern.size() && (pattern[offset] == L'{' || pattern[offset] == L'}') &&
            pattern[offset] == pattern[offset + 1]) {
            result += pattern[offset];
            offset += 2;
            continue;
        }
        if (pattern[offset] == L'}') { throw std::invalid_argument("Unmatched closing placeholder brace"); }
        if (pattern[offset] != L'{') { result += pattern[offset++]; continue; }
        const auto end = pattern.find(L'}', offset + 1);
        if (end == std::wstring_view::npos) { throw std::invalid_argument("Unclosed placeholder"); }
        result += resolve(pattern.substr(offset + 1, end - offset - 1));
        offset = end + 1;
    }
    return result;
}

std::wstring PadNumber(unsigned value, std::size_t digits) {
    auto text = std::to_wstring(value);
    if (text.size() < digits) { text.insert(0, digits - text.size(), L'0'); }
    return text;
}

} // namespace

std::filesystem::path ExecutablePath() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) { PathError("GetModuleFileNameW"); }
        if (length < buffer.size()) { buffer.resize(length); return buffer; }
        if (buffer.size() >= 32768) { throw std::length_error("Executable path exceeds the Windows path limit"); }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::filesystem::path AppDataDirectory() { return KnownDirectory(CSIDL_APPDATA) / L"VisPick"; }

std::vector<std::filesystem::path> DiagnosticDirectories() {
    std::vector<std::filesystem::path> directories;
    try { directories.push_back(ExecutablePath().parent_path()); } catch (const std::exception&) {}
    try { directories.push_back(AppDataDirectory()); } catch (const std::exception&) {}
    try { directories.push_back(std::filesystem::temp_directory_path() / L"VisPick"); } catch (const std::exception&) {}
    return directories;
}

std::vector<std::uint8_t> LoadDefaultConfigTemplate(HINSTANCE module) {
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(201), RT_RCDATA);
    if (resource == nullptr) { PathError("Find default config resource 201"); }
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const auto* data = loaded == nullptr ? nullptr : static_cast<const std::uint8_t*>(LockResource(loaded));
    if (data == nullptr || size == 0) { PathError("Load default config resource 201"); }
    return {data, data + size};
}

ConfigPathResult ResolveConfigPathAt(const std::filesystem::path& executableDirectory,
    const std::filesystem::path& fallbackDirectory, std::span<const std::uint8_t> defaultTemplate,
    const std::optional<std::filesystem::path>& explicitPath) {
    ConfigPathResult result;
    if (explicitPath) {
        if (explicitPath->empty()) { throw std::invalid_argument("Explicit config path is empty"); }
        result.path = std::filesystem::absolute(*explicitPath).lexically_normal();
        result.created = EnsureConfig(result.path, defaultTemplate);
        return result;
    }
    result.path = executableDirectory / L"VisPick.ini";
    try {
        result.created = EnsureConfig(result.path, defaultTemplate);
        return result;
    } catch (const std::system_error&) {
        // Only the implicit default path can fall back. Existing files are never rewritten.
        std::filesystem::create_directories(fallbackDirectory);
        result.path = fallbackDirectory / L"VisPick.ini";
        result.created = EnsureConfig(result.path, defaultTemplate);
        result.usedFallback = true;
        result.notice = L"程序目录不可写或配置文件不可读；改用：" + result.path.native();
        return result;
    }
}

ConfigPathResult ResolveConfigPath(const std::optional<std::filesystem::path>& explicitPath, HINSTANCE module) {
    const auto defaultTemplate = LoadDefaultConfigTemplate(module);
    if (explicitPath) { return ResolveConfigPathAt({}, {}, defaultTemplate, explicitPath); }
    // Resolve APPDATA only when needed: a broken profile must not block a portable installation.
    const auto directory = ExecutablePath().parent_path();
    const auto path = directory / L"VisPick.ini";
    try { return {path, EnsureConfig(path, defaultTemplate), false, {}}; }
    catch (const std::system_error&) { return ResolveConfigPathAt(directory, AppDataDirectory(), defaultTemplate); }
}

FolderTemplateContext DefaultFolderTemplateContext() {
    return {KnownDirectory(CSIDL_MYPICTURES), KnownDirectory(CSIDL_DESKTOPDIRECTORY), ExecutablePath().parent_path()};
}

std::filesystem::path ExpandSaveFolder(std::wstring_view pattern, const FolderTemplateContext& context) {
    if (pattern.empty()) { throw std::invalid_argument("Save folder cannot be empty"); }
    const auto expandedTokens = ExpandBraces(pattern, [&](std::wstring_view token) -> std::wstring {
        if (token == L"pictures") { return context.pictures.native(); }
        if (token == L"desktop") { return context.desktop.native(); }
        if (token == L"exe_dir") { return context.executableDirectory.native(); }
        throw std::invalid_argument("Unknown folder placeholder");
    });
    std::wstring expanded;
    for (std::size_t offset = 0; offset < expandedTokens.size();) {
        if (expandedTokens[offset] != L'%') { expanded += expandedTokens[offset++]; continue; }
        const auto end = expandedTokens.find(L'%', offset + 1);
        if (end == std::wstring::npos) { throw std::invalid_argument("Unclosed environment variable"); }
        expanded += EnvironmentValue(std::wstring_view(expandedTokens).substr(offset + 1, end - offset - 1));
        offset = end + 1;
    }
    if (expanded.find_first_of(L"<>\"|?*") != std::wstring::npos ||
        std::any_of(expanded.begin(), expanded.end(), [](wchar_t character) { return character < 32; })) {
        throw std::invalid_argument("Invalid folder characters");
    }
    const std::filesystem::path path(expanded);
    if (!path.is_absolute() || expanded.find(L':', 2) != std::wstring::npos) {
        throw std::invalid_argument("Save folder must be an absolute Windows path");
    }
    return path.lexically_normal();
}

std::wstring ExpandFileName(std::wstring_view pattern, const SYSTEMTIME& localTime, int width, int height) {
    if (width <= 0 || height <= 0) { throw std::invalid_argument("Image dimensions must be positive"); }
    auto result = ExpandBraces(pattern, [&](std::wstring_view token) -> std::wstring {
        if (token == L"date") { return PadNumber(localTime.wYear, 4) + PadNumber(localTime.wMonth, 2) + PadNumber(localTime.wDay, 2); }
        if (token == L"time") { return PadNumber(localTime.wHour, 2) + PadNumber(localTime.wMinute, 2) + PadNumber(localTime.wSecond, 2); }
        if (token == L"ms") { return PadNumber(localTime.wMilliseconds, 3); }
        if (token == L"w") { return std::to_wstring(width); }
        if (token == L"h") { return std::to_wstring(height); }
        throw std::invalid_argument("Unknown filename placeholder");
    });
    if (result.empty() || result.size() > 240 || result.back() == L'.' || result.back() == L' ' ||
        result.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
        std::any_of(result.begin(), result.end(), [](wchar_t character) { return character < 32; })) {
        throw std::invalid_argument("Invalid Windows filename");
    }
    auto base = result.substr(0, result.find(L'.'));
    while (!base.empty() && base.back() == L' ') { base.pop_back(); }
    for (auto& character : base) { if (character >= L'a' && character <= L'z') { character -= L'a' - L'A'; } }
    if (base == L"CON" || base == L"CONIN$" || base == L"CONOUT$" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CLOCK$" ||
        (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
            ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'¹' || base[3] == L'²' || base[3] == L'³'))) {
        throw std::invalid_argument("Reserved Windows filename");
    }
    return result;
}

} // namespace vispick
