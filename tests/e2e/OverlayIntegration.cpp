#include "overlay/Overlay.h"
#include "overlay/Selection.h"
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

enum class ScreenshotToolbarSlot : LONG { Save = 8, Pin = 9, Copy = 11 };

void ClickScreenshotButton(vispick::Overlay& overlay, const RECT selection, const RECT desktop,
    const vispick::MonitorInfo& monitor, const ScreenshotToolbarSlot button) {
    const auto scale = [&](int logical) { return MulDiv(logical, static_cast<int>(monitor.dpiX), 96); };
    const LONG width = std::min<LONG>(scale(470), monitor.bounds.right - monitor.bounds.left);
    const RECT toolbar = vispick::PlaceToolbar(selection, {width, scale(36)}, monitor.bounds, scale(6));
    const LONG margin = scale(4);
    const LONG slotWidth = (width - margin * 2) / 13;
    const LONG offset = button == ScreenshotToolbarSlot::Copy ? slotWidth : slotWidth / 2;
    const POINT center{toolbar.left + margin + static_cast<LONG>(button) * slotWidth + offset,
        toolbar.top + (margin + scale(32)) / 2};
    const auto client = Position(center, desktop);
    SendMessageW(overlay.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, client);
    SendMessageW(overlay.Handle(), WM_LBUTTONUP, 0, client);
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

void Run(const std::filesystem::path& visualOutput) {
    vispick::InitializeDpiAwareness();
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
            "Screenshot checkmark did not produce copy completion");
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

    if (!visualOutput.empty()) {
        // Only the deterministic full-screen fixture and our overlay are visible.
        begin(vispick::CaptureMode::Screenshot);
        Drag(overlay, {monitor.left + 180, monitor.top + 160, monitor.left + 780, monitor.top + 520}, desktop);
        DwmFlush();
        const RECT visible{monitor.left + 100, monitor.top + 100,
            std::min(monitor.right, monitor.left + 1000), std::min(monitor.bottom, monitor.top + 740)};
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
    const int scale = static_cast<int>(monitors.front().dpiX);
    const LONG magnifierWidth = std::max<LONG>(122, MulDiv(156, scale, 96));
    const LONG left = sample.x + MulDiv(24, scale, 96) + (magnifierWidth - 120) / 2;
    const LONG top = sample.y + MulDiv(24, scale, 96) + 2;
    const auto magnifier = vispick::CaptureScreen({left + 60, top + 60, left + 61, top + 61});
    Require((magnifier->Image().Pixels()[0] & 0xFFFFFFU) == (Pattern(sample.x, sample.y) & 0xFFFFFFU), "Magnifier center RGB mismatch");
    overlay.Cancel();
    config.capture.magnifier = false;

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
    std::puts("Screenshot toolbar: Copy, Pin and Save clicks returned the expected output and pixels");
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

    begin(vispick::CaptureMode::Screenshot);
    Drag(overlay, rectangle, desktop);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'R', 0);
    Drag(overlay, {rectangle.left + 12, rectangle.top + 12, rectangle.right - 12, rectangle.bottom - 12}, desktop);
    SendMessageW(overlay.Handle(), WM_KEYDOWN, 'R', 0);
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
    Require(changed, "Screenshot annotation disappeared after switching to GIF and back");
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
        if (count == 3 && std::wstring_view(arguments[1]) == L"--visual-output") { output = arguments[2]; }
        else if (count != 1) { throw std::invalid_argument("Usage: VisPickOverlayTests [--visual-output image.png]"); }
        Run(output); std::puts("Overlay integration passed"); return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "Overlay integration failed: %s\n", error.what()); return 1; }
}
