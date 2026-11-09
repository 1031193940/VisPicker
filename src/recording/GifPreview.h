#pragma once

#include "recording/Recorder.h"
#include <functional>

namespace vispick {

using GifSavePathCallback = std::function<std::optional<std::filesystem::path>(HWND, int, int)>;

// Owns the successful recording's temporary file until save/discard. Window
// operations belong to the UI thread. The owner message also signals the end
// of a modal operation; destroy only when Closed() && !Busy(), after dispatch.
class GifPreview final {
public:
    GifPreview(RecordingResult result, Config::Save saveOptions, HWND owner,
               UINT closedMessage, GifSavePathCallback chooseSavePath, bool english = false);
    ~GifPreview();
    GifPreview(const GifPreview&) = delete;
    GifPreview& operator=(const GifPreview&) = delete;
    void Show();
    void RequestDiscard(bool closePins = false);
    void PreserveTemporaryForSessionEnd() noexcept;
    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] bool Closed() const noexcept;
    [[nodiscard]] bool Busy() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace vispick
