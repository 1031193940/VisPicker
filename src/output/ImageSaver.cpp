#include "output/ImageSaver.h"
#include "imaging/ImageCodec.h"
#include "platform/Console.h"
#include <stdexcept>

namespace vispick {
ImageSaver::~ImageSaver() { WaitForCompletion(); }
void ImageSaver::WaitForCompletion() { if (worker_.joinable()) { worker_.join(); } }
void ImageSaver::Start(HWND owner, UINT message, Bitmap image, SaveDestination destination, int jpgQuality) {
    if (Busy()) { throw std::runtime_error("An image is still being saved"); }
    worker_ = std::thread([this, owner, message, image = std::move(image), destination = std::move(destination), jpgQuality] {
        SaveResult completed{std::move(destination.path), {}}; // Move ownership without allocation.
        try { SaveImage(completed.path, image.View(), destination.format, jpgQuality); }
        catch (const std::exception& error) {
            completed.failed = true;
            try { completed.error = ExceptionMessage(error); } catch (...) { /* The UI has a fixed fallback message. */ }
        } catch (...) { completed.failed = true; }
        {
            const std::lock_guard lock(mutex_);
            result_ = std::move(completed);
        }
        PostMessageW(owner, message, 0, 0);
    });
}
std::optional<SaveResult> ImageSaver::TakeResult() {
    std::optional<SaveResult> completed;
    {
        const std::lock_guard lock(mutex_);
        if (!result_) { return std::nullopt; }
        completed.swap(result_);
    }
    if (worker_.joinable()) { worker_.join(); }
    return completed;
}
}
