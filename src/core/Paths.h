#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vispick {

[[nodiscard]] std::filesystem::path ExecutablePath();
[[nodiscard]] std::filesystem::path AppDataDirectory(); // Includes the VisPick subdirectory.
[[nodiscard]] std::vector<std::filesystem::path> DiagnosticDirectories();
[[nodiscard]] std::vector<std::uint8_t> LoadDefaultConfigTemplate(HINSTANCE module = nullptr);

struct ConfigPathResult {
    std::filesystem::path path;
    bool created = false;
    bool usedFallback = false;
    std::wstring notice;
};

// CREATE_NEW only: never modifies an existing config. Explicit paths never fall back.
[[nodiscard]] ConfigPathResult ResolveConfigPath(
    const std::optional<std::filesystem::path>& explicitPath = std::nullopt,
    HINSTANCE module = nullptr);
// Same policy with injected directories/template, for tests and portable installations.
[[nodiscard]] ConfigPathResult ResolveConfigPathAt(const std::filesystem::path& executableDirectory,
    const std::filesystem::path& fallbackDirectory, std::span<const std::uint8_t> defaultTemplate,
    const std::optional<std::filesystem::path>& explicitPath = std::nullopt);

struct FolderTemplateContext {
    std::filesystem::path pictures;
    std::filesystem::path desktop;
    std::filesystem::path executableDirectory;
};

[[nodiscard]] FolderTemplateContext DefaultFolderTemplateContext();
// Unknown placeholders/environment variables and relative paths are rejected.
[[nodiscard]] std::filesystem::path ExpandSaveFolder(std::wstring_view pattern,
    const FolderTemplateContext& context);
// Returns a stem without extension. Invalid Windows filename characters, reserved names,
// unknown placeholders, trailing dots/spaces and a result longer than 240 UTF-16 units fail.
[[nodiscard]] std::wstring ExpandFileName(std::wstring_view pattern, const SYSTEMTIME& localTime,
    int width, int height);

} // namespace vispick
