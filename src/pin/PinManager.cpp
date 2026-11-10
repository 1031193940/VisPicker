#include "pin/PinManager.h"

#include "imaging/Clipboard.h"
#include "imaging/ImageCodec.h"
#include "pin/PinGeometry.h"
#include "pin/PinSource.h"
#include "platform/Compat.h"
#include "platform/Gdi.h"
#include "platform/Window.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vispick {
namespace {

constexpr UINT_PTR RefineImageTimer = 1;
constexpr std::uint64_t PinStorageBudget = (sizeof(void*) == 4 ? 384ULL : 1024ULL) * 1024 * 1024;
constexpr std::uint64_t SingleSurfacePixelLimit = (sizeof(void*) == 4 ? 16ULL : 64ULL) * 1024 * 1024;
enum class PinAction { Close, Copy, Cut, Save, Paste, Restore, CloseAll, Menu };

struct PinRecord {
    PinSource image;
    POINT origin{}; // Physical content origin; decoration is always outside it.
    double scale = 1;
    unsigned quarterTurns = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
    int opacity = 100;
    bool topmost = true;
    bool passthrough = false;
    bool compact = false;
    POINT compactAnchor{};
    int compactSide = 50;
};

std::uint64_t DesktopZoomBudget() noexcept {
    const RECT desktop = VirtualDesktopBounds();
    const auto area = static_cast<std::uint64_t>(std::max<LONG>(1, desktop.right - desktop.left)) *
                      static_cast<std::uint64_t>(std::max<LONG>(1, desktop.bottom - desktop.top));
    return area * 2;
}

class Menu {
public:
    Menu() : handle_(CreatePopupMenu()) {
        if (!handle_) { throw std::runtime_error("Create pin menu failed"); }
    }
    ~Menu() {
        if (handle_) { DestroyMenu(handle_); }
    }
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;
    [[nodiscard]] HMENU Get() const noexcept { return handle_; }
    void Add(const UINT identifier, const wchar_t* text, const bool checked = false) const {
        if (!AppendMenuW(handle_, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), identifier, text)) {
            throw std::runtime_error("Append pin menu item failed");
        }
    }
    void Separator() const {
        if (!AppendMenuW(handle_, MF_SEPARATOR, 0, nullptr)) {
            throw std::runtime_error("Append pin menu separator failed");
        }
    }
    void Attach(Menu& submenu, const wchar_t* text) const {
        if (!AppendMenuW(handle_, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu.Get()), text)) {
            throw std::runtime_error("Append pin submenu failed");
        }
        submenu.handle_ = nullptr;
    }

private:
    HMENU handle_;
};

class PinDesktopOwner final : public Window {
public:
    ~PinDesktopOwner() override { Destroy(); }
    void Open() {
        // A hidden ordinary owner keeps the pin off the taskbar without the
        // TOOLWINDOW owner chain that would exclude it from virtual desktops.
        Create({L"VisPick.PinOwner", 0, IDC_ARROW}, L"", WS_OVERLAPPED, 0, {0, 0, 0, 0}, WindowActivation::Interactive);
    }
};

class PinWindow final : public Window {
public:
    PinWindow(PinRecord& record, const Config::Pin& configuration, std::function<void(HWND, PinAction, POINT)> request,
              std::function<std::vector<RECT>(HWND)> snapTargets, std::function<std::uint64_t(HWND)> availableBytes)
        : record_(std::move(record)), config_(configuration), request_(std::move(request)),
          snapTargets_(std::move(snapTargets)), availableBytes_(std::move(availableBytes)) {}

    void Open() {
        if (record_.image.Empty()) { throw std::invalid_argument("Cannot pin an empty image"); }
        desktopOwner_.Open();
        Create({L"VisPick.Pin", CS_DBLCLKS, IDC_SIZEALL}, L"VisPick reference image", WS_POPUP,
               WS_EX_LAYERED | (config_.acrossDesktops ? WS_EX_TOOLWINDOW : 0UL) |
                   (record_.topmost ? WS_EX_TOPMOST : 0UL) | (record_.passthrough ? WS_EX_TRANSPARENT : 0UL),
               {record_.origin.x, record_.origin.y, record_.origin.x + 1, record_.origin.y + 1},
               WindowActivation::Interactive, desktopOwner_.Handle());
        DisableWindowAnimations(Handle());
        dpi_ = DpiForWindow(Handle());
        Render(false); // Preserve the native pixel size or report insufficient budget.
        ready_ = true;
        if (!SetWindowPos(Handle(), nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            throw std::runtime_error("Show reference image failed");
        }
    }

    ~PinWindow() override {
        ready_ = false;
        KillTimer(Handle(), RefineImageTimer);
        Destroy();
    }
    [[nodiscard]] SIZE OrientedSize() const noexcept {
        return PinImageSize({record_.image.Width(), record_.image.Height()}, record_.quarterTurns);
    }
    [[nodiscard]] SIZE ContentSize() const {
        return record_.compact ? SIZE{record_.compactSide, record_.compactSide}
                               : ScaledPinSize(OrientedSize(), record_.scale);
    }
    [[nodiscard]] RECT ContentBounds() const {
        const SIZE size = ContentSize();
        return {record_.origin.x, record_.origin.y, record_.origin.x + size.cx, record_.origin.y + size.cy};
    }
    [[nodiscard]] PinMemory Memory() const noexcept {
        PinMemory memory;
        memory.windows = 1;
        memory.logicalSourceBytes = record_.image.LogicalBytes();
        memory.sourceBytes = record_.image.StorageBytes();
        if (surface_) {
            memory.renderContentBytes = static_cast<std::uint64_t>(contentSize_.cx) * contentSize_.cy * 4;
            memory.decorationBytes = surface_->Pixels().size_bytes() - memory.renderContentBytes;
        }
        return memory;
    }
    [[nodiscard]] Bitmap Export() const {
        auto decoded = record_.image.Decode();
        if (record_.quarterTurns % 4 == 0 && !record_.flipHorizontal && !record_.flipVertical) { return decoded; }
        return TransformPin(decoded.View(), record_.quarterTurns, record_.flipHorizontal, record_.flipVertical);
    }
    [[nodiscard]] PinRecord TakeRecord() noexcept {
        ready_ = false;
        return std::move(record_);
    }
    [[nodiscard]] bool Visible() const noexcept { return IsWindowVisible(Handle()) != FALSE; }
    [[nodiscard]] bool OnScreen() const noexcept {
        if (!Visible()) { return false; }
        DWORD cloaked = 0;
        // IsWindowVisible remains true when the shell hides a different
        // virtual desktop. Such pins must not become invisible snap targets.
        return FAILED(DwmGetWindowAttribute(Handle(), DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked == 0;
    }
    void Visible(const bool visible) { ShowWindow(Handle(), visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
    void CenterAt(const POINT center) {
        const SIZE size = ContentSize();
        Move({center.x - size.cx / 2, center.y - size.cy / 2});
    }

    void Configure(const Config::Pin& config) {
        if (config.acrossDesktops != config_.acrossDesktops) { AcrossDesktops(config.acrossDesktops); }
        const bool topmostChanged = config.topmost != config_.topmost;
        const bool needsComposition = config.shadow != config_.shadow || config.border != config_.border ||
                                      config.smooth != config_.smooth ||
                                      (record_.compact && (config.compactSize != config_.compactSize ||
                                                           config.compactMode != config_.compactMode ||
                                                           config.compactLineColor != config_.compactLineColor));
        const bool needsAppearance = config.borderColor != config_.borderColor ||
                                     config.inactiveOpacity != config_.inactiveOpacity ||
                                     config.compactOpacity != config_.compactOpacity;
        config_ = config;
        if (topmostChanged) {
            record_.topmost = config.topmost;
            SetWindowPos(Handle(), record_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (record_.compact) {
            const int side = CompactSide();
            record_.origin.x += record_.compactSide / 2 - side / 2;
            record_.origin.y += record_.compactSide / 2 - side / 2;
            record_.compactSide = side;
        }
        if (needsComposition) {
            Render(false);
        } else if (needsAppearance) {
            Render(false, true);
        }
    }

    void Passthrough(const bool enabled) {
        record_.passthrough = enabled;
        auto style = static_cast<DWORD>(GetWindowLongPtrW(Handle(), GWL_EXSTYLE));
        style = enabled ? style | WS_EX_TRANSPARENT : style & ~static_cast<DWORD>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(Handle(), GWL_EXSTYLE, style);
    }

    void DisplayChanged(const std::span<const RECT> monitors) { Move(KeepPinVisible(ContentBounds(), monitors)); }

    UINT ShowMenu(POINT position, const bool english) const {
        const auto text = [english](const wchar_t* chinese, const wchar_t* translated) {
            return english ? translated : chinese;
        };
        Menu menu;
        menu.Add(100, text(L"复制\tCtrl+C", L"Copy\tCtrl+C"));
        menu.Add(101, text(L"另存为…\tCtrl+S", L"Save as…\tCtrl+S"));
        Menu zoom;
        constexpr std::array<const wchar_t*, 6> zoomLabels{L"25%", L"50%", L"75%", L"100%", L"150%", L"200%"};
        constexpr std::array<double, 6> zoomValues{0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
        for (UINT index = 0; index < zoomLabels.size(); ++index) {
            zoom.Add(200 + index, zoomLabels[index], std::abs(record_.scale - zoomValues[index]) < 0.0001);
        }
        zoom.Add(206, text(L"适应屏幕", L"Fit screen"));
        menu.Attach(zoom, text(L"缩放", L"Zoom"));
        Menu opacity;
        constexpr std::array<const wchar_t*, 6> opacityLabels{L"100%", L"90%", L"80%", L"60%", L"40%", L"20%"};
        constexpr std::array<int, 6> opacityValues{100, 90, 80, 60, 40, 20};
        for (UINT index = 0; index < opacityLabels.size(); ++index) {
            opacity.Add(300 + index, opacityLabels[index], record_.opacity == opacityValues[index]);
        }
        menu.Attach(opacity, text(L"不透明度", L"Opacity"));
        menu.Separator();
        menu.Add(400, text(L"旋转 90°\tR", L"Rotate 90°\tR"));
        menu.Add(401, text(L"水平翻转\tAlt+H", L"Flip horizontally\tAlt+H"));
        menu.Add(402, text(L"垂直翻转\tAlt+V", L"Flip vertically\tAlt+V"));
        menu.Add(403, text(record_.compact ? L"还原（双击）" : L"收缩（双击）",
                           record_.compact ? L"Expand (double-click)" : L"Compact (double-click)"));
        menu.Add(404, text(L"置顶", L"Always on top"), record_.topmost);
        menu.Add(405, text(L"鼠标穿透", L"Click-through"), record_.passthrough);
        menu.Separator();
        menu.Add(500, text(L"关闭\tEsc", L"Close\tEsc"));
        menu.Add(501, text(L"关闭全部参考图", L"Close all reference images"));
        menu.Add(502, text(L"恢复最近关闭\tCtrl+Z", L"Restore last closed\tCtrl+Z"));
        if (position.x == -1 && position.y == -1) { GetCursorPos(&position); }
        SetForegroundWindow(Handle());
        const UINT command = TrackPopupMenu(menu.Get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, position.x,
                                            position.y, 0, Handle(), nullptr);
        PostMessageW(Handle(), WM_NULL, 0, 0);
        return command;
    }

    void MenuCommand(const UINT command, const POINT anchor) {
        if (command >= 200 && command <= 205) {
            constexpr std::array<double, 6> values{0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
            Zoom(values[command - 200], anchor, false);
            return;
        }
        if (command == 206) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromWindow(Handle(), MONITOR_DEFAULTTONEAREST), &monitor)) {
                const SIZE size = OrientedSize();
                const double scale =
                    std::min(static_cast<double>(monitor.rcWork.right - monitor.rcWork.left) / size.cx,
                             static_cast<double>(monitor.rcWork.bottom - monitor.rcWork.top) / size.cy);
                Zoom(scale, anchor, false);
                Move({monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - contentSize_.cx) / 2,
                      monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - contentSize_.cy) / 2});
            }
            return;
        }
        if (command >= 300 && command <= 305) {
            constexpr std::array<int, 6> values{100, 90, 80, 60, 40, 20};
            record_.opacity = values[command - 300];
            Present();
            return;
        }
        switch (command) {
        case 400:
            Rotate();
            break;
        case 401:
            record_.flipHorizontal = !record_.flipHorizontal;
            Render(false);
            break;
        case 402:
            record_.flipVertical = !record_.flipVertical;
            Render(false);
            break;
        case 403:
            ToggleCompact({anchor.x - record_.origin.x, anchor.y - record_.origin.y});
            break;
        case 404:
            record_.topmost = !record_.topmost;
            SetWindowPos(Handle(), record_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case 405:
            Passthrough(!record_.passthrough);
            break;
        default:
            break;
        }
    }

protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (!ready_) { return Window::OnMessage(message, parameter, data); }
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            SetForegroundWindow(Handle());
            SetFocus(Handle());
            GetCursorPos(&dragStart_);
            dragOrigin_ = record_.origin;
            dragging_ = true;
            SetCapture(Handle());
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                POINT origin{dragOrigin_.x + cursor.x - dragStart_.x, dragOrigin_.y + cursor.y - dragStart_.y};
                const SIZE size = ContentSize();
                const auto targets = snapTargets_(Handle());
                origin = SnapPinOrigin({origin.x, origin.y, origin.x + size.cx, origin.y + size.cy}, targets,
                                       MulDiv(config_.snapDistance, static_cast<int>(dpi_), 96));
                Move(origin);
            }
            return 0;
        case WM_LBUTTONUP:
            dragging_ = false;
            if (GetCapture() == Handle()) { ReleaseCapture(); }
            return 0;
        case WM_CAPTURECHANGED:
            dragging_ = false;
            return 0;
        case WM_LBUTTONDBLCLK: {
            dragging_ = false;
            if (GetCapture() == Handle()) { ReleaseCapture(); }
            ToggleCompact({GET_X_LPARAM(data) - margin_, GET_Y_LPARAM(data) - margin_});
            return 0;
        }
        case WM_CONTEXTMENU:
            request_(Handle(), PinAction::Menu, {GET_X_LPARAM(data), GET_Y_LPARAM(data)});
            return 0;
        case WM_CLOSE:
            request_(Handle(), PinAction::Close, {});
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (Key(parameter)) { return 0; }
            break;
        case WM_MOUSEWHEEL: {
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(parameter);
            const int steps = wheelRemainder_ / WHEEL_DELTA;
            wheelRemainder_ %= WHEEL_DELTA;
            if (steps == 0) { return 0; }
            const bool control = (GET_KEYSTATE_WPARAM(parameter) & MK_CONTROL) != 0;
            const bool opacity = config_.wheel == PinWheel::Opacity ? !control : control;
            const bool shift = (GET_KEYSTATE_WPARAM(parameter) & MK_SHIFT) != 0;
            if (opacity) {
                record_.opacity = std::clamp(record_.opacity + steps * 10, 10, 100);
                Present();
            } else {
                Zoom(record_.scale + steps * (shift ? 0.01 : config_.zoomStep / 100.0),
                     {GET_X_LPARAM(data), GET_Y_LPARAM(data)}, true);
            }
            return 0;
        }
        case WM_TIMER:
            if (parameter == RefineImageTimer) {
                KillTimer(Handle(), RefineImageTimer);
                Render(false);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (active_ != (LOWORD(parameter) != WA_INACTIVE)) {
                active_ = LOWORD(parameter) != WA_INACTIVE;
                if (!record_.compact) { Render(false, true); }
            }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = LOWORD(parameter);
            if (record_.compact) {
                const int side = CompactSide();
                record_.origin.x += record_.compactSide / 2 - side / 2;
                record_.origin.y += record_.compactSide / 2 - side / 2;
                record_.compactSide = side;
            }
            // Image dimensions and content origin stay in physical pixels. Only
            // logical decoration/compact size changes; ignore the scaled RECT.
            Render(false);
            return 0;
        }
        default:
            break;
        }
        return Window::OnMessage(message, parameter, data);
    }

private:
    void AcrossDesktops(const bool enabled) {
        const auto style = GetWindowLongPtrW(Handle(), GWL_EXSTYLE);
        const auto updatedStyle = enabled ? style | WS_EX_TOOLWINDOW : style & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
        const bool wasVisible = Visible();
        // The shell re-evaluates desktop membership when a hidden window is
        // shown after this style change. Disabling restores Shell-managed
        // membership; the Shell may retain the previous desktop.
        if (wasVisible) { ShowWindow(Handle(), SW_HIDE); }
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(Handle(), GWL_EXSTYLE, updatedStyle) == 0 && GetLastError() != ERROR_SUCCESS) {
            if (wasVisible) { ShowWindow(Handle(), SW_SHOWNOACTIVATE); }
            throw std::runtime_error("Change reference image desktop mode failed");
        }
        const BOOL refreshed = SetWindowPos(Handle(), nullptr, 0, 0, 0, 0,
                                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (wasVisible) { ShowWindow(Handle(), SW_SHOWNOACTIVATE); }
        if (!refreshed) { throw std::runtime_error("Refresh reference image desktop mode failed"); }
    }

    [[nodiscard]] std::uint64_t AvailableSurfacePixels() const {
        const auto available = availableBytes_(Handle());
        const auto sourceBytes = record_.image.StorageBytes();
        return std::min(SingleSurfacePixelLimit, available > sourceBytes ? (available - sourceBytes) / 4 : 0);
    }

    [[nodiscard]] int Margin() const noexcept {
        return config_.shadow ? std::max(2, MulDiv(6, static_cast<int>(dpi_), 96)) : config_.border ? 1 : 0;
    }

    void CheckSurfaceSize(const int width, const int height) const {
        if (width > 32760 || height > 32760 || static_cast<std::uint64_t>(width) * height > AvailableSurfacePixels()) {
            throw std::length_error("Reference image exceeds the rendering budget at this size; close other pins or "
                                    "use a smaller source image");
        }
    }

    [[nodiscard]] int CompactSide() const noexcept {
        return std::clamp(MulDiv(config_.compactSize, static_cast<int>(dpi_), 96), 1, 32760);
    }

    [[nodiscard]] double BoundedScale(const double requested) const {
        const int margin = Margin();
        const SIZE source = OrientedSize();
        const auto nativePixels =
            (static_cast<std::uint64_t>(source.cx) + margin * 2) * (static_cast<std::uint64_t>(source.cy) + margin * 2);
        const auto maximumPixels = std::min(AvailableSurfacePixels(), std::max(DesktopZoomBudget(), nativePixels));
        const double requestedWithinExtent = std::min({requested, static_cast<double>(32760 - margin * 2) / source.cx,
                                                       static_cast<double>(32760 - margin * 2) / source.cy});
        const double scale = LimitPinScale(source, requestedWithinExtent, maximumPixels);
        const SIZE size = ScaledPinSize(OrientedSize(), scale);
        const auto decorationPixels = static_cast<std::uint64_t>(margin * 2) * (size.cx + size.cy) +
                                      static_cast<std::uint64_t>(margin * 2) * (margin * 2);
        if (decorationPixels >= maximumPixels) { throw std::length_error("Pin decoration exceeds rendering budget"); }
        if ((static_cast<std::uint64_t>(size.cx) + margin * 2) * (size.cy + margin * 2) > maximumPixels) {
            return LimitPinScale(source, requestedWithinExtent, maximumPixels - decorationPixels);
        }
        return scale;
    }

    void Move(const POINT origin) {
        const POINT previousOrigin = record_.origin;
        record_.origin = origin; // WM_DPICHANGED can arrive during SetWindowPos.
        // No bitmap composition, upload, timer or scaling while dragging.
        if (!SetWindowPos(Handle(), nullptr, origin.x - margin_, origin.y - margin_, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            record_.origin = previousOrigin;
            throw std::runtime_error("Move reference image failed");
        }
    }

    void Present() {
        if (!surface_) { return; }
        POINT position{record_.origin.x - margin_, record_.origin.y - margin_};
        POINT source{};
        SIZE size{surface_->Width(), surface_->Height()};
        const int opacity = record_.compact ? config_.compactOpacity
                            : active_       ? record_.opacity
                                            : std::min(record_.opacity, config_.inactiveOpacity);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(MulDiv(std::clamp(opacity, 0, 100), 255, 100)),
                            AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(Handle(), nullptr, &position, &size, surface_->DeviceContext(), &source, 0, &blend,
                                 ULW_ALPHA)) {
            throw std::runtime_error("UpdateLayeredWindow failed for reference image");
        }
    }

    void Render(const bool fast, const bool appearanceOnly = false) {
        if (rendering_) {
            if (appearanceOnly) {
                appearanceAgain_ = true;
            } else {
                renderAgain_ = true;
            }
            return;
        }
        rendering_ = true;
        try {
            if (!appearanceOnly) { Compose(fast); }
            if (surface_) {
                DrawDecoration();
                Present();
            }
        } catch (...) {
            rendering_ = false;
            throw;
        }
        rendering_ = false;
        const bool updateAppearance = std::exchange(appearanceAgain_, false);
        if (std::exchange(renderAgain_, false)) {
            Render(false);
        } else if (updateAppearance) {
            Render(false, true);
        }
    }

    void Compose(const bool fast) {
        const SIZE fullSize = ScaledPinSize(OrientedSize(), record_.scale);
        const SIZE contentSize = ContentSize();
        const int margin = record_.compact ? 0 : Margin();
        const int width = contentSize.cx + margin * 2;
        const int height = contentSize.cy + margin * 2;
        CheckSurfaceSize(width, height);

        const SIZE oriented = OrientedSize();
        SIZE desired = fullSize;
        POINT drawOrigin{margin, margin};
        if (record_.compact && config_.compactMode == CompactMode::Thumbnail) {
            const double ratio = std::min(static_cast<double>(record_.compactSide) / oriented.cx,
                                          static_cast<double>(record_.compactSide) / oriented.cy);
            desired = {std::max<LONG>(1, static_cast<LONG>(std::lround(oriented.cx * ratio))),
                       std::max<LONG>(1, static_cast<LONG>(std::lround(oriented.cy * ratio)))};
            drawOrigin = {(record_.compactSide - desired.cx) / 2, (record_.compactSide - desired.cy) / 2};
        } else if (record_.compact) {
            drawOrigin = {record_.compactSide / 2 - record_.compactAnchor.x,
                          record_.compactSide / 2 - record_.compactAnchor.y};
        }
        const bool transformNeeded = record_.quarterTurns % 4 != 0 || record_.flipHorizontal || record_.flipVertical;
        const bool scaleNeeded = oriented.cx != desired.cx || oriented.cy != desired.cy;
        Bitmap decoded;
        Bitmap transformed;
        Bitmap scaled;
        BitmapView view = record_.image.NativeBgra();
        if ((transformNeeded || scaleNeeded) && view.Empty()) {
            decoded = record_.image.Decode();
            view = decoded.View();
        }
        if (transformNeeded) {
            transformed = TransformPin(view, record_.quarterTurns, record_.flipHorizontal, record_.flipVertical);
            view = transformed.View();
        }
        if (scaleNeeded) {
            const auto interpolation = !config_.smooth ? ImageInterpolation::Nearest
                                       : fast          ? ImageInterpolation::Linear
                                       : desired.cx < view.Width() || desired.cy < view.Height()
                                           ? ImageInterpolation::Fant
                                           : ImageInterpolation::Cubic;
            scaled = ScaleImage(view, desired.cx, desired.cy, interpolation);
            view = scaled.View();
        }
        if (!surface_ || surface_->Width() != width || surface_->Height() != height) {
            surface_ = std::make_unique<DibSection>(width, height);
        }
        auto destination = surface_->Pixels();
        contentSize_ = contentSize;
        margin_ = margin;
        if (record_.compact) { std::fill(destination.begin(), destination.end(), 0U); }
        // Native opaque display reads BGR24 directly, without expanding a full
        // temporary bitmap. Transform/scaling scratch never survives this call.
        for (int row = 0; row < contentSize_.cy; ++row) {
            const int sourceY = row + margin - drawOrigin.y;
            if (sourceY < 0 || sourceY >= desired.cy) { continue; }
            for (int column = 0; column < contentSize_.cx; ++column) {
                const int sourceX = column + margin - drawOrigin.x;
                if (sourceX < 0 || sourceX >= desired.cx) { continue; }
                const auto pixel = view.Empty() ? record_.image.Pixel(sourceX, sourceY) : view.Row(sourceY)[sourceX];
                destination[static_cast<std::size_t>(row + margin) * width + column + margin] =
                    PremultiplyPinPixel(pixel);
            }
        }
    }

    void DrawDecoration() {
        const int width = surface_->Width();
        const int height = surface_->Height();
        auto destination = surface_->Pixels();
        const int thickness = record_.compact ? 2 : margin_;
        if (!thickness) { return; }
        for (int row = 0; row < height; ++row) {
            const auto paint = [&](int column) {
                auto& pixel = destination[static_cast<std::size_t>(row) * width + column];
                if (record_.compact) {
                    pixel = ((row + column) / 4) % 2 == 0 ? 0xFFFFFFFFU : 0xFF000000U | config_.compactLineColor;
                    return;
                }
                const int horizontal = column < margin_                      ? margin_ - column
                                       : column >= margin_ + contentSize_.cx ? column - margin_ - contentSize_.cx + 1
                                                                             : 0;
                const int vertical = row < margin_                      ? margin_ - row
                                     : row >= margin_ + contentSize_.cy ? row - margin_ - contentSize_.cy + 1
                                                                        : 0;
                const int distance = std::max(horizontal, vertical);
                pixel = 0;
                if (config_.shadow && distance <= margin_) {
                    const auto alpha = static_cast<std::uint32_t>((margin_ - distance + 1) * 48 / margin_);
                    pixel = alpha << 24U;
                }
                if (config_.border && active_ && distance == 1) { pixel = 0xFF000000U | config_.borderColor; }
            };
            const bool fullRow = row < thickness || row >= height - thickness || width <= thickness * 2;
            if (fullRow) {
                for (int column = 0; column < width; ++column) { paint(column); }
            } else {
                for (int column = 0; column < thickness; ++column) { paint(column); }
                for (int column = width - thickness; column < width; ++column) { paint(column); }
            }
        }
    }

    void Expand(const double expandedScale) {
        if (!record_.compact) { return; }
        const SIZE size = ScaledPinSize(OrientedSize(), expandedScale);
        const int margin = Margin();
        CheckSurfaceSize(size.cx + margin * 2, size.cy + margin * 2);
        record_.origin = RestorePinOrigin(record_.origin, record_.compactAnchor, record_.compactSide);
        record_.compact = false;
    }

    void ToggleCompact(const POINT anchor) {
        if (record_.compact) {
            Expand(record_.scale);
        } else {
            const int side = CompactSide();
            CheckSurfaceSize(side, side);
            record_.compactAnchor = anchor;
            record_.compactSide = side;
            const RECT bounds = CompactPinBounds(record_.origin, anchor, record_.compactSide);
            record_.origin = {bounds.left, bounds.top};
            record_.compact = true;
        }
        Render(false);
    }

    void Zoom(const double scale, const POINT anchor, const bool fast) {
        const double next = BoundedScale(scale);
        Expand(next);
        record_.origin = ZoomPinOrigin(record_.origin, anchor, record_.scale, next);
        record_.scale = next;
        Render(fast);
        if (fast && config_.smooth) {
            if (!SetTimer(Handle(), RefineImageTimer, 150, nullptr)) { Render(false); }
        } else {
            KillTimer(Handle(), RefineImageTimer);
        }
    }

    void Rotate() {
        Expand(record_.scale);
        const SIZE before = ContentSize();
        record_.quarterTurns = (record_.quarterTurns + 1) % 4;
        // Flips are relative to visible axes; swap them when those axes rotate.
        std::swap(record_.flipHorizontal, record_.flipVertical);
        const SIZE after = ContentSize();
        record_.origin.x += (before.cx - after.cx) / 2;
        record_.origin.y += (before.cy - after.cy) / 2;
        Render(false);
    }

    bool Key(const WPARAM key) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        POINT cursor{};
        GetCursorPos(&cursor);
        if (control) {
            switch (key) {
            case 'C':
                request_(Handle(), PinAction::Copy, {});
                return true;
            case 'X':
                request_(Handle(), PinAction::Cut, {});
                return true;
            case 'V':
                request_(Handle(), PinAction::Paste, cursor);
                return true;
            case 'S':
                request_(Handle(), PinAction::Save, {});
                return true;
            case 'Z':
                request_(Handle(), PinAction::Restore, {});
                return true;
            default:
                return false;
            }
        }
        if (record_.compact && (key == VK_ESCAPE || key == VK_RETURN)) {
            Expand(record_.scale);
            Render(false);
            return true;
        }
        const RECT bounds = ContentBounds();
        const POINT center{bounds.left + (bounds.right - bounds.left) / 2,
                           bounds.top + (bounds.bottom - bounds.top) / 2};
        if (alt) {
            if (key == VK_UP || key == VK_DOWN) {
                Zoom(record_.scale + (key == VK_UP ? 1 : -1) * (shift ? 0.01 : 0.1), center, false);
                return true;
            }
            if (key == VK_LEFT || key == VK_RIGHT) {
                record_.opacity = std::clamp(record_.opacity + (key == VK_RIGHT ? 1 : -1) * (shift ? 1 : 10), 10, 100);
                Present();
                return true;
            }
            if (key == 'H') {
                record_.flipHorizontal = !record_.flipHorizontal;
                Render(false);
                return true;
            }
            if (key == 'V') {
                record_.flipVertical = !record_.flipVertical;
                Render(false);
                return true;
            }
            return false;
        }
        if (key >= VK_LEFT && key <= VK_DOWN) {
            const int step = shift ? 50 : 1;
            Move({record_.origin.x + (key == VK_LEFT    ? -step
                                      : key == VK_RIGHT ? step
                                                        : 0),
                  record_.origin.y + (key == VK_UP     ? -step
                                      : key == VK_DOWN ? step
                                                       : 0)});
            return true;
        }
        if (key >= '5' && key <= '9') {
            Zoom(static_cast<double>(key - '0') / 10, center, false);
            return true;
        }
        if (key == '0') {
            Zoom(1, center, false);
            return true;
        }
        if (key == 'R') {
            Rotate();
            return true;
        }
        if (key == VK_ESCAPE || key == 'Q') {
            request_(Handle(), PinAction::Close, {});
            return true;
        }
        return false;
    }

    PinRecord record_;
    PinDesktopOwner desktopOwner_;
    Config::Pin config_;
    std::function<void(HWND, PinAction, POINT)> request_;
    std::function<std::vector<RECT>(HWND)> snapTargets_;
    std::function<std::uint64_t(HWND)> availableBytes_;
    std::unique_ptr<DibSection> surface_;
    SIZE contentSize_{};
    UINT dpi_ = 96;
    int margin_ = 0;
    int wheelRemainder_ = 0;
    bool ready_ = false;
    bool rendering_ = false;
    bool renderAgain_ = false;
    bool appearanceAgain_ = false;
    bool active_ = false;
    bool dragging_ = false;
    POINT dragStart_{};
    POINT dragOrigin_{};
};

} // namespace

struct PinManager::State {
    HWND owner;
    std::function<void(Bitmap)> save;
    std::function<void(std::wstring, bool)> notify;
    Config::Pin config;
    bool english = false;
    bool processing = false;
    struct Request {
        HWND window;
        PinAction action;
        POINT point;
    };
    std::deque<Request> pending;
    std::vector<std::unique_ptr<PinWindow>> pins;
    std::deque<PinRecord> recycled;
    std::vector<RECT> monitorTargets;
    std::vector<RECT> workAreas;

    void RefreshMonitors() {
        std::vector<RECT> targets, areas;
        for (const auto& monitor : EnumerateMonitors()) {
            targets.push_back(monitor.workArea);
            targets.push_back(monitor.bounds);
            areas.push_back(monitor.workArea);
        }
        if (areas.empty()) { throw std::runtime_error("No display work area available for reference images"); }
        monitorTargets.swap(targets);
        workAreas.swap(areas);
    }

    void Queue(const HWND window, const PinAction action, const POINT point = {}) {
        pending.push_back({window, action, point});
        if (!PostMessageW(owner, PinManager::PendingMessage, 0, 0)) {
            pending.pop_back();
            throw std::runtime_error("Post pin command failed");
        }
    }

    std::vector<RECT> SnapTargets(const HWND excluded) const {
        std::vector<RECT> targets = monitorTargets;
        targets.reserve(targets.size() + pins.size());
        for (const auto& pin : pins) {
            if (pin->Handle() != excluded && pin->OnScreen()) { targets.push_back(pin->ContentBounds()); }
        }
        return targets;
    }

    [[nodiscard]] std::uint64_t AvailableBytes(const HWND excluded) const noexcept {
        std::uint64_t used = 0;
        for (const auto& pin : pins) {
            if (pin->Handle() == excluded) { continue; }
            const auto memory = pin->Memory();
            used += memory.sourceBytes + memory.renderContentBytes + memory.decorationBytes;
        }
        for (const auto& record : recycled) { used += record.image.StorageBytes(); }
        return used < PinStorageBudget ? PinStorageBudget - used : 0;
    }

    PinWindow& Add(PinRecord& record, const Config::Pin& preferences) {
        auto pin = std::make_unique<PinWindow>(
            record, preferences,
            [this](const HWND window, const PinAction action, const POINT point) { Queue(window, action, point); },
            [this](const HWND window) { return SnapTargets(window); },
            [this](const HWND window) { return AvailableBytes(window); });
        auto& created = *pin;
        try {
            pin->Open();
            pins.push_back(std::move(pin));
        } catch (...) {
            record = pin->TakeRecord();
            throw;
        }
        return created;
    }

    void Close(const HWND window) {
        const auto found =
            std::find_if(pins.begin(), pins.end(), [window](const auto& pin) { return pin->Handle() == window; });
        if (found == pins.end()) { return; }
        if (config.dustBox > 0) {
            recycled.emplace_back();
            recycled.back() = (*found)->TakeRecord();
            while (recycled.size() > static_cast<std::size_t>(config.dustBox)) { recycled.pop_front(); }
        }
        pins.erase(found);
    }

    void Restore() {
        if (recycled.empty()) { return; }
        auto& record = recycled.back();
        const bool previousPassthrough = record.passthrough;
        record.passthrough = false;
        try {
            Add(record, config);
        } catch (...) {
            record.passthrough = previousPassthrough;
            throw;
        }
        recycled.pop_back();
    }

    void NotifyFailure(const char* message) const {
        if (!notify) { return; }
        const int count = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
        std::wstring detail(static_cast<std::size_t>(std::max(1, count)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, message, -1, detail.data(), static_cast<int>(detail.size()));
        if (!detail.empty() && detail.back() == L'\0') { detail.pop_back(); }
        notify((english ? L"Reference image operation failed: " : L"参考图操作失败：") + detail, true);
    }

    template <typename Action> void UpdateAll(Action action) {
        std::exception_ptr firstFailure;
        for (const auto& pin : pins) {
            try {
                action(*pin);
            } catch (...) {
                if (!firstFailure) { firstFailure = std::current_exception(); }
            }
        }
        if (firstFailure) {
            try {
                std::rethrow_exception(firstFailure);
            } catch (const std::exception& error) { NotifyFailure(error.what()); } catch (...) {
                NotifyFailure("Unknown reference image error");
            }
        }
    }

    void Execute(Request request) {
        if (request.action == PinAction::Restore) {
            Restore();
            return;
        }
        if (request.action == PinAction::CloseAll) {
            while (!pins.empty()) { Close(pins.back()->Handle()); }
            return;
        }
        const auto found =
            std::find_if(pins.begin(), pins.end(), [&](const auto& pin) { return pin->Handle() == request.window; });
        if (found == pins.end()) { return; }
        PinWindow& pin = **found;
        if (request.action == PinAction::Menu) {
            if (request.point.x == -1 && request.point.y == -1) { GetCursorPos(&request.point); }
            const UINT command = pin.ShowMenu(request.point, english);
            if (command == 0) { return; }
            if (command == 100) {
                request.action = PinAction::Copy;
            } else if (command == 101) {
                request.action = PinAction::Save;
            } else if (command == 500) {
                request.action = PinAction::Close;
            } else if (command == 501) {
                Queue(request.window, PinAction::CloseAll);
                return;
            } else if (command == 502) {
                Queue(request.window, PinAction::Restore);
                return;
            } else {
                pin.MenuCommand(command, request.point);
                return;
            }
        }
        switch (request.action) {
        case PinAction::Close:
            Close(request.window);
            break;
        case PinAction::Copy:
        case PinAction::Cut: {
            const auto image = pin.Export();
            CopyImage(owner, image.View());
            if (request.action == PinAction::Cut) { Close(request.window); }
            break;
        }
        case PinAction::Save:
            if (save) { save(pin.Export()); }
            break;
        case PinAction::Paste: {
            auto image = ReadClipboardImage(owner);
            if (!image) {
                if (notify) {
                    notify(english ? L"The clipboard does not contain an image." : L"剪贴板中没有图片。", true);
                }
                return;
            }
            PinRecord record;
            record.origin = request.point;
            record.image = PinSource(std::move(*image));
            record.opacity = config.opacity;
            record.topmost = config.topmost;
            Add(record, config).CenterAt(request.point);
            break;
        }
        default:
            break;
        }
    }
};

PinManager::PinManager(const HWND owner, std::function<void(Bitmap)> save,
                       std::function<void(std::wstring, bool)> notify)
    : state_(std::make_unique<State>()) {
    if (!owner) { throw std::invalid_argument("Pin manager requires an owner window"); }
    state_->owner = owner;
    state_->save = std::move(save);
    state_->notify = std::move(notify);
    try {
        state_->RefreshMonitors();
    } catch (const std::exception& error) { state_->NotifyFailure(error.what()); }
}
PinManager::~PinManager() = default;

void PinManager::Add(Bitmap image, const POINT screenOrigin, const Config::Pin& config) {
    state_->config = config;
    PinRecord record;
    record.image = PinSource(std::move(image));
    record.origin = screenOrigin;
    record.opacity = config.opacity;
    record.topmost = config.topmost;
    state_->Add(record, config);
}

void PinManager::AddCentered(Bitmap image, const POINT screenCenter, const Config::Pin& config) {
    state_->config = config;
    PinRecord record;
    record.image = PinSource(std::move(image));
    record.origin = screenCenter;
    record.opacity = config.opacity;
    record.topmost = config.topmost;
    state_->Add(record, config).CenterAt(screenCenter);
}

void PinManager::Configure(const Config::Pin& config, const bool english) {
    state_->config = config;
    state_->english = english;
    while (state_->recycled.size() > static_cast<std::size_t>(std::max(0, config.dustBox))) {
        state_->recycled.pop_front();
    }
    state_->UpdateAll([&](PinWindow& pin) { pin.Configure(config); });
}

void PinManager::ShowAll() {
    for (const auto& pin : state_->pins) { pin->Visible(true); }
}
void PinManager::HideAll() {
    for (const auto& pin : state_->pins) { pin->Visible(false); }
}
void PinManager::ToggleAll() {
    const bool anyVisible =
        std::any_of(state_->pins.begin(), state_->pins.end(), [](const auto& pin) { return pin->Visible(); });
    if (anyVisible) {
        HideAll();
    } else {
        ShowAll();
    }
}
void PinManager::CloseAll() {
    state_->Queue(nullptr, PinAction::CloseAll);
}
void PinManager::ResetPassthrough() {
    for (const auto& pin : state_->pins) { pin->Passthrough(false); }
}
void PinManager::RestoreLast() {
    state_->Queue(nullptr, PinAction::Restore);
}
void PinManager::DisplayChanged() {
    try {
        state_->RefreshMonitors();
    } catch (const std::exception& error) {
        state_->NotifyFailure(error.what());
        return;
    }
    state_->UpdateAll([&](PinWindow& pin) { pin.DisplayChanged(state_->workAreas); });
}

void PinManager::ProcessPending() {
    if (state_->processing) { return; }
    state_->processing = true;
    struct ResetFlag {
        bool& flag;
        ~ResetFlag() { flag = false; }
    } reset{state_->processing};
    // Nested popup/dialog loops may append requests, but cannot delete a window
    // until the outer Execute call (including its popup) has fully returned.
    while (!state_->pending.empty()) {
        const auto request = state_->pending.front();
        state_->pending.pop_front();
        try {
            state_->Execute(request);
        } catch (const std::exception& error) { state_->NotifyFailure(error.what()); }
    }
}

PinMemory PinManager::Memory() const noexcept {
    PinMemory total;
    for (const auto& pin : state_->pins) {
        const auto memory = pin->Memory();
        total.windows += memory.windows;
        total.logicalSourceBytes += memory.logicalSourceBytes;
        total.sourceBytes += memory.sourceBytes;
        total.renderContentBytes += memory.renderContentBytes;
        total.decorationBytes += memory.decorationBytes;
    }
    for (const auto& record : state_->recycled) { total.recycledBytes += record.image.StorageBytes(); }
    return total;
}

} // namespace vispick
