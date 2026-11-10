#pragma once
#include "output/SaveDialog.h"
#include "imaging/Bitmap.h"
#include <mutex>
#include <optional>
#include <thread>

namespace vispick {
struct SaveResult {
    std::filesystem::path path;
    std::wstring error;
    bool failed = false;
};
// One in-flight job bounds memory. Completion messages carry no heap pointers;
// the UI takes the owned result only after the worker has signalled completion.
class ImageSaver {
public:
    ~ImageSaver();
    [[nodiscard]] bool Busy() const noexcept { return worker_.joinable(); }
    void Start(HWND owner, UINT message, Bitmap image, SaveDestination destination, int jpgQuality);
    void WaitForCompletion(); // Session shutdown must finish file writes before returning to Windows.
    [[nodiscard]] std::optional<SaveResult> TakeResult();

private:
    std::thread worker_;
    std::mutex mutex_;
    std::optional<SaveResult> result_;
};
} // namespace vispick
