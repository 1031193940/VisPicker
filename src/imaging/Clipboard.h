#pragma once
#include "imaging/Bitmap.h"
#include <windows.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vispick {
// PNG and CF_DIBV5 retain alpha; CF_DIB is an opaque, top-down 32-bit fallback.
void CopyImage(HWND owner, BitmapView image);
void CopyText(HWND owner, const std::wstring& text);
[[nodiscard]] std::optional<Bitmap> ReadClipboardImage(HWND owner);

// UI-thread owner: publish raw pixels immediately, then append PNG when ready.
// One worker keeps only the latest pending image. The owner must remain alive
// until destruction, which joins any encoding already in progress.
class ImageClipboard final {
public:
    ImageClipboard(HWND owner, UINT readyMessage);
    ~ImageClipboard();
    ImageClipboard(const ImageClipboard&) = delete;
    ImageClipboard& operator=(const ImageClipboard&) = delete;
    void Copy(Bitmap image);
    // Call on readyMessage. Stale results are discarded; encoding/publication
    // errors propagate while the already-published DIB formats remain usable.
    void PublishReady();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

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
} // namespace vispick
