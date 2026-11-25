#pragma once
#include "core/Config.h"
#include <windows.h>
#include <optional>
#include <string_view>

namespace vispick {
struct SaveDestination {
    std::filesystem::path path;
    SaveFormat format = SaveFormat::Png;
};
// Resolves the final path and format from a save dialog selection, appending the
// active filter extension when the user's filename has no supported extension.
[[nodiscard]] SaveDestination ResolveSaveDestination(std::filesystem::path selectedPath, UINT fileTypeIndex,
                                                     bool gif = false);
// fixedExtension="gif" restricts the dialog to GIF. Otherwise PNG/JPG/BMP
// filters are offered and the returned format follows the final extension.
[[nodiscard]] std::optional<SaveDestination> ChooseSavePath(HWND owner, const Config::Save& settings, int width,
                                                            int height, std::wstring_view fixedExtension = {},
                                                            std::filesystem::path* lastFolder = nullptr,
                                                            bool english = false);
void ShowSavedFile(HWND owner, const std::filesystem::path& file);
} // namespace vispick
