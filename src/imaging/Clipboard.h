#pragma once
#include "imaging/Bitmap.h"
#include <windows.h>
#include <filesystem>
#include <memory>
#include <optional>

namespace vispick {
// PNG retains alpha; CF_DIB is an opaque, top-down 32-bit fallback.
void CopyImage(HWND owner, BitmapView image);
[[nodiscard]] std::optional<Bitmap> ReadClipboardImage(HWND owner);

struct GifClipboardFormats {
    bool animation;
    bool fileDrop;
};

// Prepare without opening the clipboard, then publish from the owner window's
// thread. The caller retains the GIF file if the file drop was published.
class GifClipboardData final {
public:
    explicit GifClipboardData(const std::filesystem::path& file);
    ~GifClipboardData();
    GifClipboardData(const GifClipboardData&) = delete;
    GifClipboardData& operator=(const GifClipboardData&) = delete;
    [[nodiscard]] GifClipboardFormats Publish(HWND owner);
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

// Checked parser exposed for malformed-input regression tests. Returned pixels
// are straight BGRA; BI_RGB's reserved high byte is never treated as alpha.
[[nodiscard]] Bitmap DecodeDib(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> EncodeDib(BitmapView image);
}
