#include "overlay/Overlay.h"
#include "overlay/Selection.h"
#include "core/Paths.h"
#include "platform/Compat.h"
#include "platform/Gdi.h"
#include "capture/ScreenGrab.h"
#include "imaging/ImageCodec.h"
#include "ForegroundFixture.h"

#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void Require(const bool condition, const char* explanation) {
    if (!condition) { throw std::runtime_error(explanation); }
}

bool LockOwnedForeground(const HWND owner) {
    SetLastError(ERROR_SUCCESS);
    if (LockSetForegroundWindow(LSFW_LOCK)) { return true; }
    const DWORD error = GetLastError();
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    DWORD ownerProcess = 0;
    if (foreground) { GetWindowThreadProcessId(foreground, &foregroundProcess); }
    GetWindowThreadProcessId(owner, &ownerProcess);
    std::fprintf(stderr, "LockSetForegroundWindow failed: last_error=%lu foreground_pid=%lu owner_pid=%lu "
        "current_pid=%lu foreground_hwnd=%p owner_hwnd=%p\n", error, foregroundProcess, ownerProcess,
        GetCurrentProcessId(), static_cast<void*>(foreground), static_cast<void*>(owner));
    return false;
}

void PressKey(vispick::Overlay& overlay, const WPARAM key, const bool control = false, const bool shift = false) {
    struct RestoreKeyboard {
        BYTE previous[256]{};
        RestoreKeyboard() { Require(GetKeyboardState(previous) != FALSE, "Get thread keyboard state failed"); }
        ~RestoreKeyboard() { SetKeyboardState(previous); }
    } restore;
    BYTE keys[256]{};
    std::copy(std::begin(restore.previous), std::end(restore.previous), std::begin(keys));
    for (const auto modifier : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
                               VK_MENU, VK_LMENU, VK_RMENU}) { keys[modifier] = 0; }
    keys[VK_CONTROL] = static_cast<BYTE>(control ? 0x80 : 0);
    keys[VK_SHIFT] = static_cast<BYTE>(shift ? 0x80 : 0);
    Require(SetKeyboardState(keys) != FALSE, "Set thread keyboard state failed");
    SendMessageW(overlay.Handle(), WM_KEYDOWN, key, 0);
}

std::uint32_t Pattern(const int screenX, const int screenY) {
    const auto horizontal = static_cast<std::uint32_t>(screenX);
    const auto vertical = static_cast<std::uint32_t>(screenY);
    return 0xFF000000U | (((horizontal * 13U + vertical * 3U) & 255U) << 16U) |
        (((horizontal * 7U + vertical * 11U) & 255U) << 8U) | ((horizontal + vertical * 17U) & 255U);
}

class PatternWindow final : public vispick::Window {
public:
    explicit PatternWindow(const RECT bounds)
        : bounds_(bounds), surface_(bounds.right - bounds.left, bounds.bottom - bounds.top) {
        auto pixels = surface_.Pixels();
        for (int row = 0; row < surface_.Height(); ++row) {
            for (int column = 0; column < surface_.Width(); ++column) {
                pixels[static_cast<std::size_t>(row) * surface_.Width() + column] = Pattern(bounds.left + column, bounds.top + row);
            }
        }
        Create({L"VisPick.Tests.OverlayPattern", 0, IDC_ARROW}, L"VisPick owned capture test pattern", WS_POPUP,
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, bounds, vispick::WindowActivation::Passive);
        vispick::DisableWindowAnimations(Handle());
        if (!SetWindowPos(Handle(), HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            Destroy();
            throw std::runtime_error("Cannot show overlay test pattern");
        }
        UpdateWindow(Handle());
        GdiFlush();
        DwmFlush();
    }
    ~PatternWindow() override { Destroy(); }
protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(Handle(), &paint);
            BitBlt(context, 0, 0, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, surface_.DeviceContext(), 0, 0, SRCCOPY);
            EndPaint(Handle(), &paint);
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
private:
    RECT bounds_;
    vispick::DibSection surface_;
};

class FocusWindow final : public vispick::Window {
public:
    explicit FocusWindow(const POINT origin) {
        Create({L"VisPick.Tests.OverlayFocus", 0, IDC_ARROW}, L"VisPick overlay focus fixture", WS_POPUP,
            WS_EX_TOOLWINDOW, {origin.x, origin.y, origin.x + 32, origin.y + 32}, vispick::WindowActivation::Interactive);
    }
    ~FocusWindow() override { Destroy(); }
};

constexpr wchar_t FocusLockClass[] = L"VisPick.Tests.OverlayFocusLock";
constexpr UINT LockFocus = WM_APP + 1;
constexpr UINT UnlockFocus = WM_APP + 2;
constexpr UINT ChangeFocus = WM_APP + 3;

class ForegroundLockWindow final : public vispick::Window {
public:
    explicit ForegroundLockWindow(HWND alternative) : alternative_(alternative) {
        MONITORINFO primary{sizeof(primary)};
        Require(GetMonitorInfoW(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY), &primary) != FALSE,
            "Cannot locate the foreground lock fixture monitor");
        const auto bounds = primary.rcMonitor;
        Create({FocusLockClass, 0, IDC_ARROW}, std::to_wstring(GetCurrentProcessId()).c_str(), WS_POPUP,
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, {bounds.left, bounds.top, bounds.left + 32, bounds.top + 32},
            vispick::WindowActivation::Interactive);
    }
    ~ForegroundLockWindow() override {
        if (GetForegroundWindow() == Handle()) { LockSetForegroundWindow(LSFW_UNLOCK); }
        Destroy();
    }

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == LockFocus) {
            if (GetForegroundWindow() == alternative_) { SetForegroundWindow(Handle()); }
            return GetForegroundWindow() == Handle() && LockOwnedForeground(Handle());
        }
        if (message == UnlockFocus || message == ChangeFocus) {
            return GetForegroundWindow() == Handle() && LockSetForegroundWindow(LSFW_UNLOCK) &&
                (message == UnlockFocus || SetForegroundWindow(alternative_));
        }
        return Window::OnMessage(message, parameter, data);
    }

private:
    HWND alternative_;
};

template <class Predicate>
bool WaitUntil(Predicate ready, DWORD timeoutMs = 5000) {
    const auto deadline = GetTickCount64() + timeoutMs;
    do {
        vispick::PumpWindowMessages();
        if (ready()) { return true; }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

class ForegroundProbeProcess final {
public:
    ForegroundProbeProcess() {
        const auto executable = vispick::ExecutablePath();
        auto command = L"\"" + executable.native() + L"\" --foreground-lock-child " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0,
            nullptr, nullptr, &startup, &child) != FALSE, "Cannot start the foreground test child");
        process_ = child.hProcess;
        CloseHandle(child.hThread);
    }
    ~ForegroundProbeProcess() {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1); // Restricted to this fixture's process handle.
            WaitForSingleObject(process_, 1000);
        }
        CloseHandle(process_);
    }
    ForegroundProbeProcess(const ForegroundProbeProcess&) = delete;
    ForegroundProbeProcess& operator=(const ForegroundProbeProcess&) = delete;

    void WaitForSuccess() const {
        Require(WaitUntil([&] { return WaitForSingleObject(process_, 0) == WAIT_OBJECT_0; }, 15000),
            "Foreground test child did not finish");
        DWORD exitCode = 1;
        Require(GetExitCodeProcess(process_, &exitCode) != FALSE && exitCode == 0, "Foreground denial regression failed");
    }

private:
    HANDLE process_ = nullptr;
};

LPARAM Position(const POINT point, const RECT desktop) {
    return MAKELPARAM(static_cast<WORD>(point.x - desktop.left), static_cast<WORD>(point.y - desktop.top));
}

void Drag(vispick::Overlay& overlay, const RECT selection, const RECT desktop) {
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, Position({selection.left, selection.top}, desktop));
    SendMessageW(overlay.Handle(), WM_MOUSEMOVE, MK_LBUTTON, Position({selection.right, selection.bottom}, desktop));
    SendMessageW(overlay.Handle(), WM_LBUTTONUP, 0, Position({selection.right, selection.bottom}, desktop));
    UpdateWindow(overlay.Handle());
    GdiFlush();
}

void Click(vispick::Overlay& overlay, const POINT point, const RECT desktop) {
    const auto client = Position(point, desktop);
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, client);
    SendMessageW(overlay.Handle(), WM_LBUTTONUP, 0, client);
}

enum class ScreenshotToolbarSlot : LONG { Save = 6, Pin = 7, Cancel = 8, Copy = 9 };

RECT ScreenshotToolbarBounds(const RECT selection, const vispick::MonitorInfo& monitor) {
    const auto scale = [&](int logical) { return MulDiv(logical, static_cast<int>(monitor.dpiX), 96); };
    const LONG width = std::min<LONG>(scale(400), monitor.bounds.right - monitor.bounds.left);
    return vispick::PlaceToolbar(selection, {width, scale(36)}, monitor.bounds, scale(6));
}

void ClickScreenshotButton(vispick::Overlay& overlay, const RECT selection, const RECT desktop,
    const vispick::MonitorInfo& monitor, const ScreenshotToolbarSlot button) {
    const auto scale = [&](int logical) { return MulDiv(logical, static_cast<int>(monitor.dpiX), 96); };
    const RECT toolbar = ScreenshotToolbarBounds(selection, monitor);
    const LONG margin = scale(4);
    const LONG slotWidth = (toolbar.right - toolbar.left - margin * 2) / 11;
    const LONG offset = button == ScreenshotToolbarSlot::Copy ? slotWidth : slotWidth / 2;
    const POINT center{toolbar.left + margin + static_cast<LONG>(button) * slotWidth + offset,
        toolbar.top + (margin + scale(32)) / 2};
    Click(overlay, center, desktop);
}

void ChooseGifFrameRate(vispick::Overlay& overlay, const RECT selection, const RECT desktop,
    const vispick::MonitorInfo& monitor, const LONG choice, const bool above) {
    const auto scale = [&](int logical) { return MulDiv(logical, static_cast<int>(monitor.dpiX), 96); };
    const LONG width = std::min<LONG>(scale(320), monitor.bounds.right - monitor.bounds.left);
    const RECT toolbar = vispick::PlaceToolbar(selection, {width, scale(36)}, monitor.bounds, scale(6));
    Click(overlay, {toolbar.right - width / 5, toolbar.top + scale(18)}, desktop);
    Require(overlay.Active(), "Opening the GIF frame-rate picker ended the overlay");
    const LONG margin = scale(4);
    const LONG optionCount = 10; // Nine presets and the fixture's custom 17 fps.
    const LONG choiceX = toolbar.left + margin + (width - margin * 2) * (choice * 2 + 1) / (optionCount * 2);
    Click(overlay, {choiceX, above ? toolbar.top - scale(16) : toolbar.bottom + scale(16)}, desktop);
}

void CheckPixels(const vispick::BitmapView image, const RECT rectangle) {
    Require(image.Width() == rectangle.right - rectangle.left && image.Height() == rectangle.bottom - rectangle.top,
        "Overlay output dimensions mismatch");
    for (int row = 0; row < image.Height(); ++row) {
        for (int column = 0; column < image.Width(); ++column) {
            Require(image.Row(row)[column] == Pattern(rectangle.left + column, rectangle.top + row),
                "Overlay output differs from owned frozen RGB pattern");
        }
    }
}

void CheckPixels(const vispick::OverlayResult& result, const RECT rectangle) {
    Require(EqualRect(&result.selection, &rectangle) != FALSE, "Overlay output rectangle mismatch");
    CheckPixels(result.image.View(), rectangle);
}

void CheckDeniedForeground(DWORD parentProcess) {
    const HWND lockedWindow = FindWindowW(FocusLockClass, std::to_wstring(parentProcess).c_str());
    DWORD ownerProcess = 0;
    if (lockedWindow) { GetWindowThreadProcessId(lockedWindow, &ownerProcess); }
    Require(lockedWindow && ownerProcess == parentProcess && GetForegroundWindow() == lockedWindow,
        "The foreground test parent lost its owned lock window before the child started");
    const auto sendToParent = [&](UINT message) {
        DWORD_PTR response = 0;
        Require(SendMessageTimeoutW(lockedWindow, message, 0, 0, SMTO_ABORTIFHUNG, 1000, &response) && response != 0,
            "The foreground test parent could not change its owned focus state");
    };
    vispick::InitializeDpiAwareness();
    const RECT desktop = vispick::VirtualDesktopBounds();
    const auto monitors = vispick::EnumerateMonitors();
    Require(!monitors.empty(), "No monitor available for foreground denial test");
    const RECT monitor = monitors.front().bounds;
    const RECT rectangle{monitor.left + 40, monitor.top + 40, monitor.left + 104, monitor.top + 88};
    PatternWindow pattern(desktop);
    vispick::Overlay overlay(nullptr);
    vispick::Config config;
    config.capture.includeCursor = false;
    config.capture.magnifier = false;
    config.capture.windowSnap = false;
    std::optional<vispick::OverlayResult> result;
    const auto completed = [&](vispick::OverlayResult value) { result = std::move(value); };
    const auto beginDenied = [&] {
        result.reset();
        Require(GetForegroundWindow() == lockedWindow, "Foreground lock fixture unexpectedly lost activation");
        overlay.Begin(vispick::CaptureMode::Screenshot, config, completed);
        vispick::PumpWindowMessages();
        Require(overlay.Active() && IsWindowVisible(overlay.Handle()) && GetForegroundWindow() == lockedWindow,
            "Denied foreground activation did not leave the overlay visible without taking focus");
    };

    beginDenied();
    const auto point = Position({rectangle.left, rectangle.top}, desktop);
    SendMessageW(overlay.Handle(), WM_RBUTTONUP, 0, point);
    Require(result && result->action == vispick::OverlayAction::Cancel && !overlay.Active() &&
        !IsWindowVisible(overlay.Handle()) && GetForegroundWindow() == lockedWindow,
        "Right-click did not dismiss the waiting overlay without changing foreground");

    beginDenied();
    sendToParent(ChangeFocus);
    Require(WaitUntil([&] { return !overlay.Active(); }) && result && result->action == vispick::OverlayAction::Cancel &&
        !IsWindowVisible(overlay.Handle()) && GetForegroundWindow() != lockedWindow,
        "Waiting overlay did not dismiss when foreground changed to another window");

    sendToParent(LockFocus);
    beginDenied();
    Require(SendMessageW(overlay.Handle(), WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(overlay.Handle()),
        MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_ACTIVATE,
        "Waiting overlay did not allow normal mouse activation and delivery of the initial click");
    // The fixture simulates the OS activation step without injecting a real click.
    sendToParent(UnlockFocus);
    Require(GetForegroundWindow() == lockedWindow, "Foreground lock fixture was interrupted before the test click");
    vispick::test::ActivateOwnedWindow(overlay.Handle());
    Drag(overlay, rectangle, desktop);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Require(result && result->action == vispick::OverlayAction::Copy && !overlay.Active(),
        "Activated overlay did not accept drag and keyboard confirmation after foreground denial");
    CheckPixels(*result, rectangle);

    std::puts("Foreground denial: pending overlay, right-click, activation and external focus change passed");
}

void RunForegroundDenialTest() {
    FocusWindow alternate({0, 0});
    ForegroundLockWindow foreground(alternate.Handle());
    constexpr UINT showWithoutActivation = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    Require(SetWindowPos(alternate.Handle(), HWND_TOPMOST, 0, 0, 0, 0, showWithoutActivation) &&
        SetWindowPos(foreground.Handle(), HWND_TOPMOST, 0, 0, 0, 0, showWithoutActivation),
        "Cannot show the owned foreground lock fixture");
    vispick::test::ActivateOwnedWindow(foreground.Handle());
    Require(LockOwnedForeground(foreground.Handle()), "Could not lock the owned test foreground window");
    ForegroundProbeProcess child;
    child.WaitForSuccess();
}

void Run(const std::filesystem::path& visualOutput, bool focusOnly, bool selectionOnly) {
    vispick::InitializeDpiAwareness();
    if (selectionOnly) { std::puts("Selection-only run: foreground-denial fixture is not executed."); }
    else { RunForegroundDenialTest(); }
    if (focusOnly) { return; }
    const RECT desktop = vispick::VirtualDesktopBounds();
    const auto monitors = vispick::EnumerateMonitors();
    Require(!monitors.empty(), "No monitor available for overlay integration");
    PatternWindow pattern(desktop);
    vispick::Overlay overlay(nullptr);
    vispick::Config config;
    config.capture.includeCursor = false;
    config.capture.magnifier = false;
    config.capture.windowSnap = false;
    std::optional<vispick::OverlayResult> result;
    const auto completed = [&](vispick::OverlayResult value) { result = std::move(value); };
    const auto begin = [&](vispick::CaptureMode mode, std::function<bool(vispick::Bitmap, RECT)> save = {}) {
        vispick::test::ActivateOwnedWindow(pattern.Handle());
        overlay.Begin(mode, config, completed, std::move(save));
    };
    for (const auto& monitor : monitors) {
        Require(monitor.bounds.right - monitor.bounds.left > 120 && monitor.bounds.bottom - monitor.bounds.top > 120,
            "Test requires monitor dimensions above 120 physical pixels");
        const RECT rectangle{monitor.bounds.left + 40, monitor.bounds.top + 40, monitor.bounds.left + 104, monitor.bounds.top + 88};
        result.reset();
        begin(vispick::CaptureMode::Screenshot);
        Drag(overlay, rectangle, desktop);
        ClickScreenshotButton(overlay, rectangle, desktop, monitor, ScreenshotToolbarSlot::Copy);
        Require(result && result->mode == vispick::CaptureMode::Screenshot && result->action == vispick::OverlayAction::Copy,
            "Screenshot Copy button did not produce copy completion");
        CheckPixels(*result, rectangle);
        Require(!overlay.Active() && !IsWindowVisible(overlay.Handle()), "Completed overlay remained visible");
        std::printf("monitor origin=(%ld,%ld) dpi=%u output=64x48 exact; appearance=%.3f ms\n",
            monitor.bounds.left, monitor.bounds.top, monitor.dpiX, result->appearanceMilliseconds);
    }
    const RECT monitor = monitors.front().bounds;
    const RECT rectangle{monitor.left + 40, monitor.top + 40, monitor.left + 104, monitor.top + 88};

    result.reset();
    begin(vispick::CaptureMode::Screenshot);
    Drag(overlay, rectangle, desktop);
    ClickScreenshotButton(overlay, rectangle, desktop, monitors.front(), ScreenshotToolbarSlot::Pin);
    Require(result && result->mode == vispick::CaptureMode::Screenshot && result->action == vispick::OverlayAction::Pin,
        "Screenshot Pin button did not produce reference completion");
    CheckPixels(*result, rectangle);

    result.reset();
    begin(vispick::CaptureMode::Screenshot);
    Drag(overlay, rectangle, desktop);
    ClickScreenshotButton(overlay, rectangle, desktop, monitors.front(), ScreenshotToolbarSlot::Cancel);
    Require(result && result->mode == vispick::CaptureMode::Screenshot && result->action == vispick::OverlayAction::Cancel &&
        result->image.Empty() && !overlay.Active() && !IsWindowVisible(overlay.Handle()),
        "Screenshot Cancel button did not dismiss the overlay without output");

    if (!visualOutput.empty()) {
        // Only the deterministic full-screen fixture and our overlay are visible.
        begin(vispick::CaptureMode::Screenshot);
        const RECT visualSelection{monitor.left + std::min(180L, (monitor.right - monitor.left) / 4),
            monitor.top + std::min(160L, (monitor.bottom - monitor.top) / 4),
            std::min(monitor.right - 1, monitor.left + 780), std::min(monitor.bottom - 1, monitor.top + 520)};
        Drag(overlay, visualSelection, desktop);
        DwmFlush();
        const RECT toolbar = ScreenshotToolbarBounds(visualSelection, monitors.front());
        constexpr LONG contextMargin = 20;
        const RECT visible{std::max(monitor.left, toolbar.left - contextMargin), std::max(monitor.top, toolbar.top - contextMargin),
            std::min(monitor.right, toolbar.right + contextMargin), std::min(monitor.bottom, toolbar.bottom + contextMargin)};
        const auto screenshot = vispick::CaptureScreen(visible);
        vispick::SaveImage(visualOutput, screenshot->Image(), vispick::SaveFormat::Png);
        overlay.Cancel();
    }

    // Mouse hover magnification samples the same frozen physical pixel. This
    // reads only our overlay showing our deterministic pattern; nothing is saved.
    config.capture.magnifier = true;
    begin(vispick::CaptureMode::Screenshot);
    const POINT sample{monitor.left + 60, monitor.top + 60};
    SendMessageW(overlay.Handle(), WM_MOUSEMOVE, 0, Position(sample, desktop));
    UpdateWindow(overlay.Handle()); GdiFlush(); DwmFlush();
    const auto layout = vispick::LayoutMagnifier(sample, monitor, monitors.front().dpiX);
    const LONG centerX = (layout.center.left + layout.center.right) / 2;
    const LONG centerY = (layout.center.top + layout.center.bottom) / 2;
    const auto magnifier = vispick::CaptureScreen({centerX, centerY, centerX + 1, centerY + 1});
    Require((magnifier->Image().Pixels()[0] & 0xFFFFFFU) == (Pattern(sample.x, sample.y) & 0xFFFFFFU), "Magnifier center RGB mismatch");
    overlay.Cancel();
    config.capture.magnifier = false;

    // Ctrl+A selects the display under the pointer; modifiers adjust physical
    // selection pixels rather than DPI-scaled UI pixels. GIF avoids flattening.
    result.reset();
    begin(vispick::CaptureMode::Gif);
    SendMessageW(overlay.Handle(), WM_MOUSEMOVE, 0, Position(sample, desktop));
    PressKey(overlay, 'A', true);
    PressKey(overlay, VK_LEFT, true);
    PressKey(overlay, VK_UP, true, true);
    PressKey(overlay, VK_RETURN);
    const RECT keyboardSelection{monitor.left, monitor.top, monitor.right - 1, monitor.bottom - 10};
    Require(result && result->action == vispick::OverlayAction::Record && EqualRect(&result->selection, &keyboardSelection),
        "Ctrl+A / Ctrl+arrows did not select the pointer display and resize its right/bottom edges in physical pixels");
    result.reset();

    // Save cancellation keeps the complete editor session; accepted save hands
    // ownership of the image to the callback and then completes with no duplicate.
    bool chooseSave = false;
    int saveRequests = 0;
    std::optional<vispick::Bitmap> saved;
    result.reset();
    begin(vispick::CaptureMode::Screenshot,
        [&](vispick::Bitmap image, RECT bounds) {
            Require(EqualRect(&bounds, &rectangle) != FALSE, "Save lost selection geometry");
            ++saveRequests;
            if (chooseSave) { saved = std::move(image); }
            return chooseSave;
        });
    Drag(overlay, rectangle, desktop);
    BYTE originalKeys[256]{};
    Require(GetKeyboardState(originalKeys) != FALSE, "GetKeyboardState failed");
    BYTE keys[256]{};
    std::copy(std::begin(originalKeys), std::end(originalKeys), std::begin(keys));
    keys[VK_CONTROL] = 0x80;
    Require(SetKeyboardState(keys) != FALSE, "Set thread keyboard state failed");
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'S', 0);
    Require(SetKeyboardState(originalKeys) != FALSE, "Restore thread keyboard state failed");
    Require(saveRequests == 1 && overlay.Active() && !result, "Cancelled save discarded the selection session");
    chooseSave = true;
    ClickScreenshotButton(overlay, rectangle, desktop, monitors.front(), ScreenshotToolbarSlot::Save);
    Require(saveRequests == 2 && saved && result && result->mode == vispick::CaptureMode::Screenshot &&
        result->action == vispick::OverlayAction::Save && result->image.Empty(), "Screenshot Save button did not hand off the image exactly once");
    CheckPixels(saved->View(), rectangle);
    std::puts("Screenshot toolbar: Copy, Pin, Cancel and Save clicks returned the expected actions");
    saved.reset(); result.reset();

    // A monitor change or session shutdown can cancel while the save dialog pumps messages.
    begin(vispick::CaptureMode::Screenshot,
        [&](vispick::Bitmap, RECT) { overlay.Cancel(); return false; });
    Drag(overlay, rectangle, desktop);
    Require(SetKeyboardState(keys) != FALSE, "Set thread keyboard state failed");
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'S', 0);
    Require(SetKeyboardState(originalKeys) != FALSE, "Restore thread keyboard state failed");
    Require(!overlay.Active() && !IsWindowVisible(overlay.Handle()) && result && result->action == vispick::OverlayAction::Cancel,
        "Cancelled save session resurrected its overlay after the nested dialog returned");
    result.reset();

    std::optional<vispick::Bitmap> undoSnapshot;
    begin(vispick::CaptureMode::Screenshot, [&](vispick::Bitmap image, RECT) {
        undoSnapshot = std::move(image);
        return false;
    });
    Drag(overlay, rectangle, desktop);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'R', 0);
    Drag(overlay, {rectangle.left + 12, rectangle.top + 12, rectangle.right - 12, rectangle.bottom - 12}, desktop);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'R', 0);
    Require(SetKeyboardState(keys) != FALSE, "Set thread keyboard state failed");
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'Z', 0);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'S', 0);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'Y', 0);
    Require(SetKeyboardState(originalKeys) != FALSE, "Restore thread keyboard state failed");
    Require(undoSnapshot && overlay.Active(), "Ctrl+Z did not allow the unannotated selection to be inspected");
    CheckPixels(undoSnapshot->View(), rectangle);
    const POINT outside{rectangle.right + 12, rectangle.top + 12};
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, Position(outside, desktop));
    SendMessageW(overlay.Handle(), WM_LBUTTONUP, 0, Position(outside, desktop));
    overlay.SwitchMode(vispick::CaptureMode::Gif);
    overlay.SwitchMode(vispick::CaptureMode::Pin);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Require(result && result->action == vispick::OverlayAction::Pin && EqualRect(&result->selection, &rectangle) &&
        result->image.Width() == rectangle.right - rectangle.left, "Mode switch or outside click lost the current selection");
    bool changed = false;
    for (int row = 0; row < result->image.Height(); ++row) {
        for (int column = 0; column < result->image.Width(); ++column) {
            changed = changed || result->image.Row(row)[column] != Pattern(rectangle.left + column, rectangle.top + row);
        }
    }
    Require(changed, "Ctrl+Y did not restore the annotation through the GIF mode switch");
    result.reset();

    config.gif.fps = 17;
    const auto gifDpi = static_cast<int>(monitors.front().dpiX);
    const LONG gifBottom = monitor.bottom - MulDiv(36, gifDpi, 96) - MulDiv(6, gifDpi, 96) - 1;
    const RECT gifSelection{rectangle.left, gifBottom - 48, rectangle.right, gifBottom};
    begin(vispick::CaptureMode::Gif);
    Drag(overlay, gifSelection, desktop);
    ChooseGifFrameRate(overlay, gifSelection, desktop, monitors.front(), 5, true); // 24 fps.
    overlay.SwitchMode(vispick::CaptureMode::Screenshot);
    overlay.SwitchMode(vispick::CaptureMode::Gif);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Require(result && result->action == vispick::OverlayAction::Record && result->gifFramesPerSecond == 24 &&
        EqualRect(&result->selection, &gifSelection), "GIF picker did not return the selected frame rate after switching modes");
    result.reset();

    begin(vispick::CaptureMode::Gif);
    Drag(overlay, rectangle, desktop);
    ChooseGifFrameRate(overlay, rectangle, desktop, monitors.front(), 6, false); // 25 fps.
    ChooseGifFrameRate(overlay, rectangle, desktop, monitors.front(), 3, false); // Configured 17 fps.
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Require(result && result->action == vispick::OverlayAction::Record && result->gifFramesPerSecond == 17 &&
        EqualRect(&result->selection, &rectangle), "GIF picker did not offer the configured custom frame rate");
    std::puts("GIF frame-rate picker: presets, configured custom rate and mode switches passed");
    result.reset();

    begin(vispick::CaptureMode::Gif);
    Drag(overlay, rectangle, desktop);
    overlay.SwitchMode(vispick::CaptureMode::Screenshot);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Require(result && result->action == vispick::OverlayAction::Copy, "Switching from GIF failed to enable screenshot output");
    CheckPixels(*result, rectangle);
    result.reset();

    begin(vispick::CaptureMode::Pin);
    const auto dragStart = Position({rectangle.left, rectangle.top}, desktop);
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, dragStart);
    SendMessageW(overlay.Handle(), WM_MOUSEMOVE, MK_LBUTTON, Position({rectangle.right, rectangle.bottom}, desktop));
    Require(GetCapture() == overlay.Handle(), "Selection gesture did not capture the pointer");
    SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_ESCAPE, 0);
    Require(!overlay.Active() && GetCapture() != overlay.Handle() && result && result->action == vispick::OverlayAction::Cancel,
        "Esc during a drag did not release capture and cancel safely");
    result.reset();

    // Cancellation releases the full-screen window and gesture before invoking
    // App completion, even if downstream error handling itself throws.
    vispick::test::ActivateOwnedWindow(pattern.Handle());
    overlay.Begin(vispick::CaptureMode::Screenshot, config, [](vispick::OverlayResult) {
        throw std::runtime_error("Injected overlay completion failure");
    });
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, dragStart);
    bool completionFailed = false;
    try { overlay.Cancel(); }
    catch (const std::runtime_error&) { completionFailed = true; }
    Require(completionFailed && !overlay.Active() && !IsWindowVisible(overlay.Handle()) && GetCapture() != overlay.Handle(),
        "Completion failure left the overlay visible or kept pointer capture");

    begin(vispick::CaptureMode::Screenshot);
    {
        FocusWindow focus({monitor.left, monitor.top});
        ShowWindow(focus.Handle(), SW_SHOW);
        SetForegroundWindow(focus.Handle());
        SetFocus(focus.Handle());
        vispick::PumpWindowMessages();
        Require(!overlay.Active() && !IsWindowVisible(overlay.Handle()) && result && result->action == vispick::OverlayAction::Cancel &&
            GetForegroundWindow() == focus.Handle(), "Overlay losing activation did not hide or stole foreground back");
    }
    result.reset();

    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int cycle = 0; cycle < 100; ++cycle) {
        begin(vispick::CaptureMode::Screenshot);
        SendMessageW(overlay.Handle(), WM_KEYDOWN, VK_ESCAPE, 0);
        Require(!overlay.Active() && result && result->action == vispick::OverlayAction::Cancel, "Esc did not cancel overlay");
        result.reset();
    }
    const auto gdiDelta = static_cast<long long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - gdiBefore;
    const auto userDelta = static_cast<long long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - userBefore;
    Require(std::abs(gdiDelta) <= 5 && std::abs(userDelta) <= 5, "100 capture/cancel cycles leaked GUI resources");
    std::printf("100 capture/cancel cycles: GDI delta=%lld USER delta=%lld\n", gdiDelta, userDelta);
    Require(vispick::Window::ExceptionCount() == 0, "A window callback exception occurred");
}

} // namespace

int wmain(int count, wchar_t** arguments) {
    try {
        std::filesystem::path output;
        const bool focusOnly = count == 2 && std::wstring_view(arguments[1]) == L"--focus-only";
        const bool selectionOnly = count == 2 && std::wstring_view(arguments[1]) == L"--selection-only";
        if (count == 3 && std::wstring_view(arguments[1]) == L"--foreground-lock-child") {
            CheckDeniedForeground(std::stoul(arguments[2]));
            return 0;
        }
        if (count == 3 && std::wstring_view(arguments[1]) == L"--visual-output") { output = arguments[2]; }
        else if (count != 1 && !focusOnly && !selectionOnly) {
            throw std::invalid_argument("Usage: VisPickOverlayTests [--focus-only | --selection-only | --visual-output image.png]");
        }
        Run(output, focusOnly, selectionOnly);
        std::puts(selectionOnly ? "Overlay selection integration passed; foreground-denial fixture was not run" : "Overlay integration passed");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "Overlay integration failed: %s\n", error.what()); return 1; }
}
