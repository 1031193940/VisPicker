#include "App.h"
#include "VisPickVersion.h"

#include "core/Paths.h"
#include "platform/Autostart.h"
#include "platform/Compat.h"
#include "platform/ConfigWatcher.h"
#include "platform/HotkeyRegistration.h"
#include "platform/SingleInstance.h"
#include "platform/Tray.h"
#include "platform/Console.h"
#include "imaging/Clipboard.h"
#include "imaging/ImageCodec.h"
#include "output/ImageSaver.h"
#include "output/SaveDialog.h"
#include "pin/PinManager.h"
#include "recording/Recorder.h"
#include "recording/GifPreview.h"
#include <shellapi.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace vispick {
namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT ConfigurationChanged = WM_APP + 2;
constexpr UINT ProcessQueue = WM_APP + 3;
constexpr UINT WindowFailure = WM_APP + 4;
constexpr UINT SelectionFinished = WM_APP + 5;
constexpr UINT ImageSaved = WM_APP + 6;
constexpr UINT RecordingDone = WM_APP + 7;
constexpr UINT PreviewClosed = WM_APP + 8;
constexpr UINT ClipboardReady = WM_APP + 9;
constexpr UINT_PTR ConfigurationDebounce = 1;
constexpr UINT_PTR ConfigurationRetry = 2;
constexpr UINT_PTR HotkeyRetry = 3;
constexpr UINT HotkeyRetryIntervalMs = 2000;
HWND exceptionOwner = nullptr; // Window callbacks run only on the application's UI thread.

struct ModalScope {
    unsigned& depth;
    HWND owner;
    ModalScope(unsigned& count, HWND window) : depth(count), owner(window) { ++depth; }
    ~ModalScope() {
        if (--depth == 0) { PostMessageW(owner, ProcessQueue, 0, 0); }
    }
};

const wchar_t* CommandName(AppCommand command) noexcept {
    switch (command) {
    case AppCommand::Screenshot:
        return L"screenshot";
    case AppCommand::Gif:
        return L"gif";
    case AppCommand::Pin:
        return L"pin";
    case AppCommand::PinClipboard:
        return L"pin-clipboard";
    case AppCommand::PinFile:
        return L"pin-file";
    case AppCommand::TogglePins:
        return L"toggle-pins";
    case AppCommand::Reload:
        return L"reload";
    case AppCommand::Exit:
        return L"exit";
    default:
        return L"menu";
    }
}

AppCommand TrayCommand(TrayClick click) {
    switch (click) {
    case TrayClick::Screenshot:
        return AppCommand::Screenshot;
    case TrayClick::Gif:
        return AppCommand::Gif;
    case TrayClick::Pin:
        return AppCommand::Pin;
    default:
        return AppCommand::None;
    }
}
} // namespace

App::App() = default;

App::~App() {
    stopping_ = true;
    if (watcher_) { watcher_->Stop(); }
    watcher_.reset();
    if (recorder_) { recorder_->Stop(true); }
    recorder_.reset();
    preview_.reset();
    overlay_.reset();
    pins_.reset();
    imageSaver_.reset();
    imageClipboard_.reset();
    hotkeys_.reset();
    tray_.reset();
    Window::SetExceptionCallback(nullptr);
    exceptionOwner = nullptr;
    Destroy();
    if (icon_) { DestroyIcon(icon_); }
}

bool App::English() const noexcept {
    return config_.general.language == Language::English ||
           (config_.general.language == Language::Auto && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
}

std::wstring App::Text(const wchar_t* chinese, const wchar_t* english) const {
    return English() ? english : chinese;
}

void App::Report(std::wstring_view message, bool warning) {
    log_.Write(warning ? LogLevel::Warning : LogLevel::Info, message);
    if (warning && startupNotificationPending_ && startupProblem_.empty()) { startupProblem_ = message.substr(0, 160); }
    if (tray_ && !startupNotificationPending_ &&
        (config_.general.notify == NotificationMode::All ||
         (warning && config_.general.notify == NotificationMode::Errors))) {
        (void)tray_->Notify(L"VisPick", message, warning);
    }
}

void App::NotifyStartup(const bool repeatedLaunch) {
    if (!startupNotificationPending_ && !repeatedLaunch) { return; }
    auto message =
        repeatedLaunch
            ? Text(
                  L"VisPick " VISPICK_PRODUCT_VERSION_W L" 已在运行。右键系统托盘图标打开菜单。",
                  L"VisPick " VISPICK_PRODUCT_VERSION_W L" is already running. Right-click its tray icon for commands.")
            : Text(L"VisPick " VISPICK_PRODUCT_VERSION_W L" 已启动。右键系统托盘图标打开菜单。",
                   L"VisPick " VISPICK_PRODUCT_VERSION_W L" started. Right-click its system tray icon for commands.");
    const bool warning = config_.general.notify != NotificationMode::None &&
                         (!configurationProblems_.empty() || !hotkeyProblems_.empty() || !startupProblem_.empty());
    if (warning) {
        if (!hotkeyProblems_.empty()) {
            message += L"\n" + hotkeyProblems_.substr(0, hotkeyProblems_.find(L'\n'));
        } else if (!startupProblem_.empty()) {
            message += L"\n" + startupProblem_;
        }
        if (!configurationProblems_.empty() || !hotkeyProblems_.empty()) {
            message += Text(L"\n配置或快捷键存在问题，请在「关于」中查看。",
                            L"\nConfiguration or shortcut problems: see About for details.");
        }
    }
    log_.Write(warning ? LogLevel::Warning : LogLevel::Info, message);
    if (tray_ && tray_->Notify(L"VisPick", message, warning)) {
        startupNotificationPending_ = false;
        startupProblem_.clear();
    }
}

void App::WindowFailed(const WindowException& error) noexcept {
    if (exceptionOwner && error.message != WindowFailure) {
        PostMessageW(exceptionOwner, WindowFailure, error.message, reinterpret_cast<LPARAM>(error.window));
    }
}

int App::Run(const LaunchOptions& options, const std::wstring& instanceWindowTitle) {
    performanceLogging_ = options.performanceLogging;
    executable_ = ExecutablePath();
    const auto selectedPath =
        ResolveConfigPath(options.configPath.empty() ? std::nullopt : std::optional(options.configPath));
    configPath_ = selectedPath.path;
    explicitConfigPath_ = options.configPath;
    config_ = LoadConfig(configPath_).config;
    Create({SingleInstance::MainWindowClass, 0, IDC_ARROW}, instanceWindowTitle.c_str(), WS_OVERLAPPED,
           WS_EX_TOOLWINDOW, {0, 0, 0, 0}, WindowActivation::Interactive);
    Window::SetExceptionCallback(WindowFailed);
    exceptionOwner = Handle();
    icon_ =
        static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!icon_) { throw std::runtime_error("Cannot load the VisPick tray icon"); }
    tray_ = std::make_unique<Tray>(Handle(), TrayMessage, icon_);
    const bool trayAdded = tray_->Restore();
    taskbarCreated_ = Tray::TaskbarCreatedMessage();
    autostart_ = std::make_unique<Autostart>();
    hotkeys_ = std::make_unique<HotkeyRegistration>(Handle());
    overlay_ = std::make_unique<Overlay>(Handle());
    imageSaver_ = std::make_unique<ImageSaver>();
    imageClipboard_ = std::make_unique<ImageClipboard>(Handle(), ClipboardReady);
    pins_ = std::make_unique<PinManager>(
        Handle(), [this](Bitmap image) { (void)SaveBitmap(std::move(image)); },
        [this](std::wstring message, bool warning) { Report(message, warning); });
    watcher_ = std::make_unique<ConfigWatcher>(configPath_, Handle(), ConfigurationChanged);
    watcher_->Start(); // Arm before reading again, including changes during initial startup.
    ReloadConfiguration(false);
    try {
        CleanOldRecordings();
    } catch (const std::exception& error) { log_.Write(LogLevel::Warning, ExceptionMessage(error)); }
    if (!trayAdded) { log_.Write(LogLevel::Warning, L"tray_add_failed; waiting for TaskbarCreated"); }
    try {
        (void)autostart_->RepairIfEnabled(executable_);
    } catch (const std::exception& error) { Report(ExceptionMessage(error), true); }
    if (!selectedPath.notice.empty()) { Report(selectedPath.notice, true); }
    if (trayAdded) { NotifyStartup(); }
    log_.Write(LogLevel::Info, L"ready config=" + configPath_.native());
    if (options.command != AppCommand::None) { Queue(options); }
    MSG message{};
    BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status == -1) { throw std::runtime_error("GetMessageW failed"); }
    return static_cast<int>(message.wParam);
}

void App::ReloadConfiguration(bool announce, bool retry, bool retryHotkeys) {
    KillTimer(Handle(), ConfigurationRetry);
    const auto loaded = LoadConfig(configPath_, &config_);
    if (loaded.retryable && !retry && SetTimer(Handle(), ConfigurationRetry, 300, nullptr) != 0) { return; }
    if (loaded.accepted || !appliedHotkeys_) {
        config_ = loaded.config;
        ApplyConfiguration(retryHotkeys);
    }
    configurationProblems_.clear();
    for (const auto& diagnostic : loaded.diagnostics) {
        const auto line =
            Text(L"配置第 ", L"Configuration line ") + std::to_wstring(diagnostic.line) + L": " + diagnostic.message;
        log_.Write(LogLevel::Warning, line);
        configurationProblems_ += line + L"\n";
    }
    if (!loaded.diagnostics.empty()) {
        if (!startupNotificationPending_) {
            Report(configurationProblems_.substr(0, 180) +
                       Text(L"\n无效项沿用上次有效值。关于窗口可查看全部错误。",
                            L"\nInvalid values retain their last valid settings. About shows all errors."),
                   true);
        }
    } else if (announce && hotkeyProblems_.empty()) {
        Report(Text(L"配置已重新加载。", L"Configuration reloaded."));
    }
    log_.Write(LogLevel::Info, L"config_reload accepted=" + std::to_wstring(loaded.accepted) + L" diagnostics=" +
                                   std::to_wstring(loaded.diagnostics.size()));
}

void App::ApplyConfiguration(bool retryHotkeys) {
    const auto logging = log_.Configure(config_.general.log || performanceLogging_);
    if (logging.error != 0) {
        Report(Text(L"无法打开日志文件，错误 ", L"Cannot open log file, error ") + std::to_wstring(logging.error),
               true);
    } else if (logging.enabled && logging.usingFallback) {
        Report(Text(L"日志写入备用位置：", L"Log uses fallback location: ") + logging.path.native());
    }
    tray_->SetLanguage(English());
    tray_->SetLeftClickCommand(TrayCommand(config_.general.trayClick));
    tray_->SetHotkeys(config_.hotkeys);
    if (pins_) {
        try {
            pins_->Configure(config_.pin, English());
        } catch (const std::exception& error) { Report(ExceptionMessage(error), true); }
    }
    if (!retryHotkeys && appliedHotkeys_ == config_.hotkeys) { return; }
    RebindHotkeys(retryHotkeys);
    appliedHotkeys_ = config_.hotkeys;
}

void App::RebindHotkeys(bool announceFailure) {
    const std::array bindings{
        HotkeyBinding{AppCommand::Screenshot, config_.hotkeys.screenshot, Text(L"截图", L"Screenshot")},
        HotkeyBinding{AppCommand::Gif, config_.hotkeys.gif, L"GIF"},
        HotkeyBinding{AppCommand::PinClipboard, config_.hotkeys.pinClipboard, Text(L"剪贴板贴图", L"Pin clipboard")},
        HotkeyBinding{AppCommand::TogglePins, config_.hotkeys.togglePins, Text(L"隐藏/显示参考图", L"Toggle pins")}};
    const auto failures = hotkeys_->Rebind(bindings);
    bool retry = false;
    std::wstring problems;
    for (const auto& failure : failures) {
        problems += failure.label + L" (" + FormatHotkey(failure.hotkey) + L")";
        if (failure.error == ERROR_HOTKEY_ALREADY_REGISTERED) {
            problems += Text(L" 已被占用。\n", L" is already registered.\n");
            retry = true;
        } else {
            problems +=
                Text(L" 注册失败，错误 ", L" failed to register, error ") + std::to_wstring(failure.error) + L"\n";
        }
    }
    if (retry) {
        SetTimer(Handle(), HotkeyRetry, HotkeyRetryIntervalMs, nullptr);
    } else {
        KillTimer(Handle(), HotkeyRetry);
    }
    const bool changed = hotkeyProblems_ != problems;
    hotkeyProblems_ = std::move(problems);
    if (!hotkeyProblems_.empty() && (changed || announceFailure)) {
        const auto message = Text(L"以下热键无法注册。请在托盘的「关于」中查看。\n",
                                  L"These shortcuts could not be registered. See About in the tray.\n") +
                             hotkeyProblems_;
        if (startupNotificationPending_) {
            log_.Write(LogLevel::Warning, message);
        } else {
            Report(message, true);
        }
    }
}

void App::Queue(LaunchOptions options) {
    if (commands_.size() >= 64) { throw std::runtime_error("Pending command limit reached"); }
    commands_.push_back(std::move(options));
    if (!PostMessageW(Handle(), ProcessQueue, 0, 0)) {
        commands_.pop_back();
        throw std::runtime_error("Cannot schedule application command");
    }
}

void App::ProcessCommands() {
    if (stopping_ || modalDepth_ || (preview_ && preview_->Busy())) { return; }
    if (preview_ && preview_->Closed()) { preview_.reset(); }
    if (pinsPending_ && pins_) {
        pinsPending_ = false;
        pins_->ProcessPending();
    }
    if (commands_.empty()) { return; }
    auto request = std::move(commands_.front());
    commands_.pop_front();
    if (request.performanceLogging && !performanceLogging_) {
        performanceLogging_ = true;
        (void)log_.Configure(true);
    }
    try {
        Dispatch(request.command, request.pinFile);
    } catch (const std::exception& error) { Report(ExceptionMessage(error), true); }
    if (!commands_.empty() && !stopping_) { PostMessageW(Handle(), ProcessQueue, 0, 0); }
}

void App::BeginSelection(CaptureMode mode) {
    if (!selections_.empty()) { return; } // A completed selection still owns the next action.
    if (recorder_) {
        Report(Text(L"请先停止当前 GIF 录制。", L"Please stop the current GIF recording first."), true);
        return;
    }
    if (mode == CaptureMode::Gif && preview_ && !preview_->Closed()) {
        SetForegroundWindow(preview_->Handle());
        Report(Text(L"请先保存或放弃当前 GIF 预览。", L"Please save or discard the current GIF preview first."), true);
        return;
    }
    if (overlay_->Active()) {
        overlay_->SwitchMode(mode);
        return;
    }
    overlay_->Begin(
        mode, config_,
        [this](OverlayResult result) {
            selections_.push_back(std::move(result));
            PostMessageW(Handle(), SelectionFinished, 0, 0);
        },
        [this](Bitmap image, RECT) { return SaveBitmap(std::move(image), overlay_->Handle()); });
}

bool App::SaveBitmap(Bitmap image, HWND dialogOwner) {
    if (stopping_) { return false; }
    if (imageSaver_->Busy()) {
        Report(Text(L"正在保存上一张图片，请稍后重试。", L"The previous image is still being saved. Please try again."),
               true);
        return false;
    }
    const ModalScope modal(modalDepth_, Handle());
    const auto settings = config_.save; // Config may reload inside the native dialog's message loop.
    const auto destination = ChooseSavePath(dialogOwner ? dialogOwner : Handle(), settings, image.Width(),
                                            image.Height(), {}, &lastSaveFolder_);
    if (!destination || stopping_) { return false; }
    imageSaver_->Start(Handle(), ImageSaved, std::move(image), *destination, settings.jpgQuality);
    return true;
}

void App::PinAtCursor(Bitmap image) {
    POINT cursor{};
    GetCursorPos(&cursor);
    pins_->AddCentered(std::move(image), cursor, config_.pin);
}

void App::ProcessSelection() {
    if (selections_.empty()) { return; }
    auto result = std::move(selections_.front());
    selections_.pop_front();
    if (stopping_) { return; }
    log_.Write(LogLevel::Info, L"overlay_appearance_ms=" + std::to_wstring(result.appearanceMilliseconds));
    try {
        switch (result.action) {
        case OverlayAction::Copy: {
            const auto width = result.image.Width();
            const auto height = result.image.Height();
            imageClipboard_->Copy(std::move(result.image));
            log_.Write(LogLevel::Info,
                       L"image_copied width=" + std::to_wstring(width) + L" height=" + std::to_wstring(height));
        } break;
        case OverlayAction::Save: // saveRequested already accepted ownership.
        case OverlayAction::Cancel:
            break;
        case OverlayAction::Pin:
            pins_->Add(std::move(result.image), {result.selection.left, result.selection.top}, config_.pin);
            break;
        case OverlayAction::Record: {
            if (recorder_ || (preview_ && !preview_->Closed())) {
                Report(Text(L"请先结束当前录制或预览。", L"Please finish the current recording or preview first."),
                       true);
                break;
            }
            auto settings = config_.gif;
            settings.fps = result.gifFramesPerSecond;
            auto recording = std::make_unique<Recorder>(result.selection, settings, Handle(), RecordingDone, English());
            recording->Start();
            recorder_ = std::move(recording);
        } break;
        }
    } catch (const std::exception& error) { Report(ExceptionMessage(error), true); }
}

void App::RecordingFinished() {
    if (!recorder_) { return; }
    auto result = recorder_->TakeResult();
    if (!result) { return; }
    recorder_.reset();
    if (stopping_) {
        FinishExitIfReady();
        return;
    }
    const bool exitRequested = std::exchange(exitAfterRecording_, false);
    log_.Write(LogLevel::Info,
               L"recording_frames=" + std::to_wstring(result->frameCount) + L" captured=" +
                   std::to_wstring(result->capturedFrames) + L" dropped=" + std::to_wstring(result->droppedTicks) +
                   L" width=" + std::to_wstring(result->width) + L" height=" + std::to_wstring(result->height) +
                   L" duration_cs=" + std::to_wstring(result->durationCentiseconds) + L" pipeline_bound_bytes=" +
                   std::to_wstring(result->peakPipelineBytes) + L" capture_layered=" +
                   std::to_wstring(result->captureLayeredWindows) + L" capture_failures=" +
                   std::to_wstring(result->captureFailures) + L" cursor_failures=" +
                   std::to_wstring(result->cursorFailures) + L" cancelled=" + std::to_wstring(result->cancelled));
    if (result->captureInterrupted) {
        Report(Text(L"屏幕采集中断，已保留之前录到的画面。",
                    L"Capture was interrupted. The recorded frames were preserved."),
               true);
    }
    if (!result->error.empty()) {
        Report(result->error, true);
    } else if (!result->cancelled && result->path.empty()) {
        Report(Text(L"没有捕获到可保存的 GIF 帧。", L"No GIF frames were captured."), true);
    } else if (!result->cancelled) {
        auto preview = std::make_unique<GifPreview>(
            std::move(*result), config_.save, Handle(), PreviewClosed,
            [this](HWND owner, int width, int height) -> std::optional<std::filesystem::path> {
                const ModalScope modal(modalDepth_, Handle());
                const auto settings = config_.save;
                const auto selected = ChooseSavePath(owner, settings, width, height, L"gif", &lastSaveFolder_);
                return selected && !stopping_ ? std::optional(selected->path) : std::nullopt;
            },
            English());
        preview->Show();
        log_.Write(LogLevel::Info,
                   L"preview_created visible=" + std::to_wstring(IsWindowVisible(preview->Handle()) != FALSE));
        preview_ = std::move(preview);
    }
    if (exitRequested) { RequestExit(); }
}

void App::RequestExit() {
    if (stopping_) { return; }
    if (modalDepth_ || (preview_ && preview_->Busy())) {
        LaunchOptions request;
        request.command = AppCommand::Exit;
        Queue(std::move(request));
        return;
    }
    if (recorder_) {
        exitAfterRecording_ = true;
        recorder_->Stop();
        return;
    }
    const bool hasPins = pins_ && pins_->Memory().windows != 0;
    if (preview_ && !preview_->Closed()) {
        const ModalScope modal(modalDepth_, Handle());
        preview_->RequestDiscard(hasPins);
        if (!preview_->Closed()) { return; }
    } else if (hasPins) {
        const ModalScope modal(modalDepth_, Handle());
        if (MessageBoxW(
                Handle(),
                Text(L"关闭所有参考图并退出 VisPick？", L"Close all reference images and exit VisPick?").c_str(),
                L"VisPick", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }
    stopping_ = true;
    KillTimer(Handle(), HotkeyRetry);
    if (overlay_) { overlay_->Cancel(); }
    if (watcher_) { watcher_->Stop(); }
    if (hotkeys_) { hotkeys_->Clear(); }
    if (recorder_) { recorder_->Stop(true); }
    FinishExitIfReady();
}

void App::EndSession() {
    // Windows has already committed to shutdown. Never enter another dialog;
    // preserve an unsaved completed GIF and synchronously finish owned workers.
    stopping_ = true;
    KillTimer(Handle(), HotkeyRetry);
    if (hotkeys_) { hotkeys_->Clear(); }
    if (tray_) { tray_->Remove(); }
    if (watcher_) { watcher_->Stop(); }
    if (preview_) { preview_->PreserveTemporaryForSessionEnd(); }
    if (overlay_) { overlay_->Cancel(); }
    if (recorder_) {
        if (recorder_->Finished()) {
            (void)recorder_->TakeResult();
        } else {
            recorder_->Stop(true);
        }
        recorder_.reset();
    }
    if (imageSaver_) { imageSaver_->WaitForCompletion(); }
    PostQuitMessage(0);
}

void App::FinishExitIfReady() {
    if (!stopping_ || recorder_ || (imageSaver_ && imageSaver_->Busy())) { return; }
    if (tray_) { tray_->Remove(); }
    PostQuitMessage(0);
}

void App::Dispatch(AppCommand command, const std::filesystem::path& file) {
    if (stopping_) { return; }
    log_.Write(LogLevel::Info, std::wstring(L"command=") + CommandName(command));
    switch (command) {
    case AppCommand::None:
        NotifyStartup(true);
        break;
    case AppCommand::Screenshot:
        BeginSelection(CaptureMode::Screenshot);
        break;
    case AppCommand::Gif:
        if (recorder_) {
            recorder_->Stop();
        } else {
            BeginSelection(CaptureMode::Gif);
        }
        break;
    case AppCommand::Pin:
        BeginSelection(CaptureMode::Pin);
        break;
    case AppCommand::PinClipboard:
        if (auto image = ReadClipboardImage(Handle())) {
            PinAtCursor(std::move(*image));
        } else {
            Report(Text(L"剪贴板中没有可读取的图片。", L"The clipboard contains no readable image."), true);
        }
        break;
    case AppCommand::PinFile:
        PinAtCursor(ReadImage(file));
        break;
    case AppCommand::TogglePins:
        pins_->ToggleAll();
        break;
    case AppCommand::ShowPins:
        pins_->ShowAll();
        break;
    case AppCommand::HidePins:
        pins_->HideAll();
        break;
    case AppCommand::ClosePins:
        pins_->CloseAll();
        break;
    case AppCommand::ResetPinPassthrough:
        pins_->ResetPassthrough();
        break;
    case AppCommand::RestorePin:
        pins_->RestoreLast();
        break;
    case AppCommand::TogglePinsAcrossDesktops:
        SavePinAcrossDesktops(configPath_, !config_.pin.acrossDesktops);
        ReloadConfiguration(false);
        break;
    case AppCommand::OpenConfig: {
        const auto opened = reinterpret_cast<INT_PTR>(
            ShellExecuteW(Handle(), L"open", configPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (opened <= 32) { throw std::runtime_error("Cannot open configuration file in its associated editor"); }
        break;
    }
    case AppCommand::Reload:
        ReloadConfiguration(true, false, true);
        break;
    case AppCommand::ToggleAutostart: {
        bool enabled = false;
        try {
            enabled = autostart_->Enabled();
        } catch (const std::exception&) {
            autostart_->SetEnabled(false, executable_);
            Report(Text(L"已移除损坏的自启设置。", L"Removed the invalid start-at-sign-in entry."));
            break;
        }
        const bool enable = !enabled;
        autostart_->SetEnabled(enable, executable_, explicitConfigPath_);
        Report(enable ? Text(L"已开启当前用户的开机自启。", L"Start at sign-in is enabled for the current user.")
                      : Text(L"已关闭开机自启。", L"Start at sign-in is disabled."));
        break;
    }
    case AppCommand::About: {
        // Own the complete text across this modal call: configuration may reload
        // while MessageBox runs a nested message loop.
        const auto details = Text(L"VisPick " VISPICK_PRODUCT_VERSION_W L"\n截图、标注、参考图与 GIF 录制。\n\n配置：",
                                  L"VisPick"
                                  L" " VISPICK_PRODUCT_VERSION_W L"\nScreenshots, annotations, reference images and "
                                  L"GIF recording.\n\nConfiguration: ") +
                             configPath_.native() + L"\n\n" + configurationProblems_ + hotkeyProblems_;
        const ModalScope modal(modalDepth_, Handle());
        MessageBoxW(Handle(), details.c_str(), L"VisPick", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case AppCommand::Exit:
        RequestExit();
        break;
    }
}

LRESULT App::OnMessage(UINT message, WPARAM parameter, LPARAM data) {
    if (taskbarCreated_ && message == taskbarCreated_) {
        if (stopping_) { return 0; }
        const bool restored = tray_->Restore();
        log_.Write(restored ? LogLevel::Info : LogLevel::Warning, restored ? L"tray_restored" : L"tray_restore_failed");
        if (restored) { NotifyStartup(); }
        return 0;
    }
    switch (message) {
    case WM_COPYDATA: {
        if (stopping_ || !data) { return SingleInstance::Rejected; }
        auto request = SingleInstance::Decode(*reinterpret_cast<const COPYDATASTRUCT*>(data));
        if (!request) { return SingleInstance::Rejected; }
        if (!request->pinFile.empty() && !request->pinFile.is_absolute()) { return SingleInstance::Rejected; }
        if (!SingleInstance::ConfigMatches(request->configPath, configPath_)) { return SingleInstance::ConfigConflict; }
        Queue(std::move(*request));
        return SingleInstance::Accepted;
    }
    case WM_HOTKEY: {
        const auto command = hotkeys_ ? hotkeys_->CommandFor(static_cast<int>(parameter)) : AppCommand::None;
        if (command != AppCommand::None) {
            const bool beginCapture =
                commands_.empty() && (command == AppCommand::Screenshot || command == AppCommand::Gif);
            LaunchOptions request;
            request.command = command;
            Queue(std::move(request));
            // Honor the hotkey's foreground eligibility before returning to the message loop.
            if (beginCapture) { ProcessCommands(); }
        }
        return 0;
    }
    case TrayMessage: {
        if (!tray_ || !autostart_) { return 0; }
        bool autostartEnabled = false;
        try {
            autostartEnabled = autostart_->Enabled();
        } catch (const std::exception& error) { log_.Write(LogLevel::Warning, ExceptionMessage(error)); }
        const auto command = tray_->HandleCallback(parameter, data, autostartEnabled, config_.pin.acrossDesktops);
        if (command != AppCommand::None) {
            LaunchOptions request;
            request.command = command;
            Queue(std::move(request));
        }
        return 0;
    }
    case ConfigurationChanged:
        if (parameter != 0) {
            Report(Text(L"配置监控已停止，错误 ", L"Configuration watching stopped, error ") +
                       std::to_wstring(parameter),
                   true);
        } else if (!stopping_) {
            SetTimer(Handle(), ConfigurationDebounce, 150, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (parameter == ConfigurationRetry) {
            KillTimer(Handle(), ConfigurationRetry);
            if (!stopping_) { ReloadConfiguration(true, true); }
            return 0;
        }
        if (parameter == ConfigurationDebounce) {
            KillTimer(Handle(), ConfigurationDebounce);
            if (!stopping_) { ReloadConfiguration(true); }
            return 0;
        }
        if (parameter == HotkeyRetry) {
            if (!stopping_) { RebindHotkeys(); }
            return 0;
        }
        break;
    case ProcessQueue:
        ProcessCommands();
        return 0;
    case SelectionFinished:
        ProcessSelection();
        return 0;
    case ImageSaved:
        if (const auto saved = imageSaver_->TakeResult()) {
            if (stopping_) {
                FinishExitIfReady();
                return 0;
            }
            if (saved->failed) {
                Report(saved->error.empty() ? Text(L"图片保存失败。", L"Image save failed.") : saved->error, true);
            } else {
                Report(Text(L"已保存：", L"Saved: ") + saved->path.native());
            }
        }
        return 0;
    case RecordingDone:
        RecordingFinished();
        return 0;
    case ClipboardReady:
        try {
            imageClipboard_->PublishReady();
        } catch (const std::exception& error) {
            Report(Text(L"图片已复制，PNG 格式补充失败：", L"Image copied; adding the PNG format failed: ") +
                       ExceptionMessage(error),
                   true);
        }
        return 0;
    case PreviewClosed:
        if (preview_ && preview_->Closed() && !preview_->Busy() && !modalDepth_) { preview_.reset(); }
        PostMessageW(Handle(), ProcessQueue, 0, 0);
        return 0;
    case PinManager::PendingMessage:
        if (modalDepth_) {
            pinsPending_ = true;
        } else if (pins_) {
            pins_->ProcessPending();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (overlay_) { overlay_->Cancel(); }
        if (pins_) { pins_->DisplayChanged(); }
        return 0;
    case WindowFailure:
        if (overlay_ && overlay_->Active() &&
            (reinterpret_cast<HWND>(data) == overlay_->Handle() ||
             IsChild(overlay_->Handle(), reinterpret_cast<HWND>(data)))) {
            // Release the frozen full-screen window before reporting the error.
            overlay_->Cancel();
        }
        Report(Text(L"窗口操作失败，已保留错误计数。", L"A window operation failed; the error was recorded."), true);
        return 0;
    case WM_CLOSE:
        Dispatch(AppCommand::Exit);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (parameter) { EndSession(); }
        return 0;
    }
    return Window::OnMessage(message, parameter, data);
}

} // namespace vispick
