#pragma once

#include "core/Config.h"
#include "imaging/Bitmap.h"
#include <windows.h>
#include <filesystem>
#include <span>
#include <vector>

namespace vispick {

// WIC is created on the calling thread and released before returning. Decoding
// checks dimensions before allocating; Bitmap always stores straight BGRA.
[[nodiscard]] Bitmap ReadImage(const std::filesystem::path& path);
[[nodiscard]] Bitmap DecodeImage(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> EncodeImage(BitmapView image, SaveFormat format, int jpgQuality = 92);
// The caller owns the moveable block until GlobalFree or SetClipboardData.
[[nodiscard]] HGLOBAL EncodeClipboardPng(BitmapView image);
void SaveImage(const std::filesystem::path& path, BitmapView image, SaveFormat format, int jpgQuality = 92);
enum class ImageInterpolation { Nearest, Linear, Cubic, Fant };
[[nodiscard]] Bitmap ScaleImage(BitmapView image, int width, int height, ImageInterpolation interpolation);

} // namespace vispick
