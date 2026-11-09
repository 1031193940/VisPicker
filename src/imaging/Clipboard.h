#pragma once
#include "imaging/Bitmap.h"
#include <windows.h>
#include <optional>

namespace vispick {
// PNG retains alpha; CF_DIB is an opaque, top-down 32-bit fallback.
void CopyImage(HWND owner, BitmapView image);
[[nodiscard]] std::optional<Bitmap> ReadClipboardImage(HWND owner);
// Checked parser exposed for malformed-input regression tests. Returned pixels
// are straight BGRA; BI_RGB's reserved high byte is never treated as alpha.
[[nodiscard]] Bitmap DecodeDib(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> EncodeDib(BitmapView image);
}
