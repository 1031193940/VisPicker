#pragma once

#include "core/Config.h"
#include "imaging/Bitmap.h"
#include "platform/Window.h"

#include <functional>
#include <memory>

namespace vispick {

enum class CaptureMode { Screenshot, Pin, Gif };
enum class OverlayAction { Copy, Save, Pin, Record, Cancel };

struct OverlayResult {
    CaptureMode mode = CaptureMode::Screenshot;
    OverlayAction action = OverlayAction::Cancel;
    RECT selection{}; // Physical screen coordinates, right/bottom exclusive.
    Bitmap image; // Opaque crop; empty for Record/Cancel or an accepted saveRequested.
    double appearanceMilliseconds = 0;
};

class Overlay final : public Window {
public:
    explicit Overlay(HWND owner);
    ~Overlay() override;
    // Callback receives owned data after the window is hidden and session
    // resources are released. App queues the result and posts its dispatch.
    void Begin(CaptureMode mode, const Config& config, std::function<void(OverlayResult)> completion,
               std::function<bool(Bitmap, RECT)> saveRequested = {});
    void SwitchMode(CaptureMode mode);
    void Cancel();
    [[nodiscard]] bool Active() const noexcept;
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vispick
