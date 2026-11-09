#pragma once

#include "core/AppCommand.h"
#include "core/Config.h"
#include "core/Log.h"
#include "platform/Window.h"
#include "overlay/Overlay.h"
#include <deque>
#include <filesystem>
#include <memory>

namespace vispick {

class ConfigWatcher;
class HotkeyRegistration;
class Tray;
class Autostart;
class ImageSaver;
class CopyToast;
class PinManager;
class Recorder;
class GifPreview;

class App final : public Window {
public:
    App();
    ~App() override;
    int Run(const LaunchOptions& options, const std::wstring& instanceWindowTitle);
    void Dispatch(AppCommand command, const std::filesystem::path& file = {});
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override;
private:
    void ReloadConfiguration(bool announce, bool retry = false, bool retryHotkeys = false);
    void ApplyConfiguration(bool retryHotkeys);
    void RebindHotkeys(bool announceFailure = false);
    void Notify(std::wstring_view message, bool warning = false);
    void Queue(LaunchOptions options);
    void ProcessCommands();
    void BeginSelection(CaptureMode mode);
    void ProcessSelection();
    bool SaveBitmap(Bitmap image, HWND dialogOwner = nullptr);
    void PinAtCursor(Bitmap image);
    void RecordingFinished();
    void RequestExit();
    void EndSession();
    void FinishExitIfReady();
    [[nodiscard]] bool English() const noexcept;
    [[nodiscard]] std::wstring Text(const wchar_t* chinese, const wchar_t* english) const;
    static void WindowFailed(const WindowException& error) noexcept;

    Config config_;
    std::filesystem::path executable_;
    std::filesystem::path configPath_;
    std::filesystem::path explicitConfigPath_;
    std::optional<Config::Hotkeys> appliedHotkeys_;
    Log log_;
    HICON icon_ = nullptr;
    std::unique_ptr<Tray> tray_;
    std::unique_ptr<Autostart> autostart_;
    std::unique_ptr<HotkeyRegistration> hotkeys_;
    std::unique_ptr<ConfigWatcher> watcher_;
    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<ImageSaver> imageSaver_;
    std::unique_ptr<CopyToast> copyToast_;
    std::unique_ptr<PinManager> pins_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<GifPreview> preview_;
    std::deque<LaunchOptions> commands_;
    std::deque<OverlayResult> selections_;
    std::filesystem::path lastSaveFolder_;
    std::filesystem::path notificationFile_;
    std::wstring configurationProblems_;
    std::wstring hotkeyProblems_;
    bool performanceLogging_ = false;
    bool stopping_ = false;
    unsigned modalDepth_ = 0;
    bool exitAfterRecording_ = false;
    bool pinsPending_ = false;
    UINT taskbarCreated_ = 0;
};

} // namespace vispick
