#include "overlay/Overlay.h"

#include "capture/ScreenGrab.h"
#include "editor/Annotation.h"
#include "editor/Renderer.h"
#include "editor/TextInput.h"
#include "imaging/Clipboard.h"
#include "overlay/Selection.h"
#include "platform/Compat.h"
#include "platform/Gdi.h"
#include "platform/ToolbarIcon.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vispick {
namespace {

constexpr UINT CommitTextMessage = WM_APP + 51;
constexpr UINT CheckFocusMessage = WM_APP + 52;
constexpr UINT_PTR ActivationWatch = 1;
constexpr std::array<std::uint32_t, 8> PresetColors{0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759,
                                                    0x00C7BE, 0x007AFF, 0xAF52DE, 0xFFFFFF};
constexpr std::array<int, 3> StrokeWidths{2, 4, 6};
constexpr std::array<int, 3> FontSizes{14, 20, 28};
constexpr std::array<int, 3> MosaicSizes{6, 10, 16};

class PaintScope {
public:
    explicit PaintScope(const HWND window) : window_(window), context_(BeginPaint(window, &paint_)) {}
    ~PaintScope() { EndPaint(window_, &paint_); }
    [[nodiscard]] HDC Context() const noexcept { return context_; }
    [[nodiscard]] RECT Bounds() const noexcept { return paint_.rcPaint; }

private:
    HWND window_;
    PAINTSTRUCT paint_{};
    HDC context_;
};

class SavedDc {
public:
    explicit SavedDc(const HDC context) : context_(context), saved_(SaveDC(context)) {
        if (!saved_) { throw std::runtime_error("SaveDC failed for overlay"); }
    }
    ~SavedDc() { RestoreDC(context_, saved_); }

private:
    HDC context_;
    int saved_;
};

COLORREF Rgb(const std::uint32_t color) noexcept {
    return RGB((color >> 16U) & 255U, (color >> 8U) & 255U, color & 255U);
}

bool EqualRect(const RECT first, const RECT second) noexcept {
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}

void Fill(const HDC context, const RECT rectangle, const COLORREF color) {
    SetDCBrushColor(context, color);
    FillRect(context, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

struct WindowSnapshot {
    HWND excluded;
    RECT desktop;
    std::vector<RECT> rectangles;
    std::exception_ptr error;
};

BOOL CALLBACK SnapshotWindow(const HWND window, const LPARAM parameter) noexcept {
    auto& snapshot = *reinterpret_cast<WindowSnapshot*>(parameter);
    try {
        if (window == snapshot.excluded || !IsWindowVisible(window) || IsIconic(window) || window == GetShellWindow()) {
            return TRUE;
        }
        const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_EX_LAYERED) != 0) {
            if ((style & WS_EX_TRANSPARENT) != 0) { return TRUE; }
            BYTE opacity = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(window, nullptr, &opacity, &flags) && (flags & LWA_ALPHA) != 0 &&
                opacity == 0) {
                return TRUE;
            }
        }
        wchar_t name[64]{};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        if (std::wcscmp(name, L"Progman") == 0 || std::wcscmp(name, L"WorkerW") == 0 ||
            std::wcscmp(name, L"tooltips_class32") == 0) {
            return TRUE;
        }
        DWORD cloaked = 0;
        // Attribute 14 is unavailable on Win7; failure simply leaves it visible.
        if (SUCCEEDED(DwmGetWindowAttribute(window, 14, &cloaked, sizeof(cloaked))) && cloaked) { return TRUE; }
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
            !GetWindowRect(window, &bounds)) {
            return TRUE;
        }
        RECT visible{};
        if (IntersectRect(&visible, &bounds, &snapshot.desktop)) { snapshot.rectangles.push_back(visible); }
        return TRUE;
    } catch (...) {
        snapshot.error = std::current_exception();
        return FALSE;
    }
}

enum class ButtonKind {
    Tool,
    Save,
    Copy,
    Pin,
    Cancel,
    Confirm,
    Color,
    Width,
    Font,
    MosaicSize,
    GifFrameRatePicker,
    GifFrameRate
};
struct Button {
    RECT bounds{};
    ButtonKind kind;
    int value = 0;
    const wchar_t* hint = L"";
    ToolbarIcon icon = ToolbarIcon::None;
};

} // namespace

struct Overlay::State {
    explicit State(Overlay& overlay) : window(overlay) {}

    Overlay& window;
    Config config;
    CaptureMode mode = CaptureMode::Screenshot;
    bool active = false;
    bool saving = false;
    bool drawing = false;
    bool english = false;
    bool choosingGifFrameRate = false;
    int gifFramesPerSecond = 30;
    UINT dpi = 96;
    RECT desktop{};
    RECT hover{};
    RECT toolbar{};
    POINT pointer{};
    HWND previousForeground = nullptr;
    std::optional<std::chrono::steady_clock::time_point> activationDeadline;
    RECT activationHint{};
    Selection selection;
    std::vector<RECT> windows;
    std::vector<MonitorInfo> monitors;
    std::vector<RECT> damage;
    std::vector<Button> buttons;
    std::shared_ptr<const FrozenScreen> frozen;
    std::unique_ptr<DibSection> backBuffer;
    std::unique_ptr<AnnotationModel> annotations;
    std::unique_ptr<EditorRenderer> renderer;
    std::unique_ptr<TextInput> textInput;
    UniqueGdiObject<HFONT> font;
    UniqueGdiObject<HPEN> iconPen;
    std::function<void(OverlayResult)> completion;
    std::function<bool(Bitmap, RECT)> saveRequested;
    double appearanceMilliseconds = 0;
    unsigned brightness = 100;

    [[nodiscard]] bool HasFocus() const noexcept {
        if (GetForegroundWindow() != window.Handle()) { return false; }
        const HWND focus = GetFocus();
        return focus == window.Handle() || (textInput && focus && focus == textInput->Handle());
    }

    [[nodiscard]] bool TryFocus() const noexcept {
        if (GetForegroundWindow() != window.Handle()) { SetForegroundWindow(window.Handle()); }
        if (GetForegroundWindow() == window.Handle() && !HasFocus()) { SetFocus(window.Handle()); }
        return HasFocus();
    }

    void AwaitActivation() {
        const RECT monitor = MonitorAt(pointer);
        const LONG width = std::min<LONG>(Scale(420), monitor.right - monitor.left);
        const LONG left = monitor.left + (monitor.right - monitor.left - width) / 2;
        const LONG top = monitor.top + Scale(32);
        activationHint = {left, top, left + width, top + Scale(34)};
        activationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        if (!SetTimer(window.Handle(), ActivationWatch, 100, nullptr)) {
            throw std::runtime_error("Cannot schedule selection activation timeout");
        }
    }

    void EndActivationWait() noexcept {
        if (!activationDeadline) { return; }
        KillTimer(window.Handle(), ActivationWatch);
        activationDeadline.reset();
    }

    void Activated() {
        if (!activationDeadline || !HasFocus()) { return; }
        EndActivationWait();
        AddDamage(activationHint);
    }

    void CheckFocus() {
        if (!active || saving) { return; }
        if (HasFocus()) {
            Activated();
            return;
        }
        if (activationDeadline) {
            const HWND foreground = GetForegroundWindow();
            if (std::chrono::steady_clock::now() < *activationDeadline &&
                (!foreground || foreground == previousForeground || foreground == window.Handle())) {
                return;
            }
        }
        Finish(OverlayAction::Cancel);
    }

    void CheckFocusLater() {
        if (!saving && !PostMessageW(window.Handle(), CheckFocusMessage, 0, 0)) { CheckFocus(); }
    }

    [[nodiscard]] int Scale(const int logical) const noexcept { return MulDiv(logical, static_cast<int>(dpi), 96); }
    [[nodiscard]] RECT Local(RECT rectangle) const noexcept {
        OffsetRect(&rectangle, -desktop.left, -desktop.top);
        return rectangle;
    }
    [[nodiscard]] POINT Local(const POINT point) const noexcept {
        return {point.x - desktop.left, point.y - desktop.top};
    }
    [[nodiscard]] RECT VisualSelection() const noexcept {
        return selection.HasSelection() ? selection.Bounds() : hover;
    }
    [[nodiscard]] AnnotationTool PropertyTool() const noexcept {
        if (!annotations || mode == CaptureMode::Gif) { return AnnotationTool::None; }
        if (textInput && textInput->Active()) { return AnnotationTool::Text; }
        if (const auto* selected = annotations->Selected()) { return selected->tool; }
        return annotations->Tool();
    }
    [[nodiscard]] AnnotationStyle PropertyStyle() const {
        if (annotations->Selected() && !(textInput && textInput->Active())) { return annotations->SelectedStyle(); }
        return annotations->Style();
    }
    [[nodiscard]] RECT AnnotationUiBounds() const noexcept {
        if (!annotations || mode == CaptureMode::Gif || (textInput && textInput->Active())) { return {}; }
        return annotations->SelectedBounds();
    }
    [[nodiscard]] COLORREF Accent() const noexcept {
        return mode == CaptureMode::Screenshot ? RGB(58, 122, 254)
               : mode == CaptureMode::Pin      ? RGB(52, 199, 89)
                                               : RGB(255, 59, 48);
    }
    [[nodiscard]] const wchar_t* Text(const wchar_t* chinese, const wchar_t* translated) const noexcept {
        return english ? translated : chinese;
    }

    [[nodiscard]] RECT MonitorAt(const POINT point) const noexcept {
        for (const auto& monitor : monitors) {
            if (PtInRect(&monitor.bounds, point)) { return monitor.bounds; }
        }
        MONITORINFO information{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &information)) {
            return information.rcMonitor;
        }
        return desktop;
    }

    [[nodiscard]] UINT DpiAt(const POINT point) const noexcept {
        const RECT bounds = MonitorAt(point);
        for (const auto& monitor : monitors) {
            if (EqualRect(monitor.bounds, bounds)) { return monitor.dpiX; }
        }
        return 96;
    }

    void UpdateDpi(const POINT point) {
        const UINT next = DpiAt(point);
        if (next == dpi && font.Get() && iconPen.Get()) { return; }
        dpi = next;
        font.Reset(CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei"));
        if (!font.Get()) { throw std::runtime_error("Create overlay font failed"); }
        iconPen = CreateToolbarIconPen(static_cast<int>(dpi));
    }

    void UpdateSelectionDpi() {
        if (selection.HasSelection()) {
            const RECT bounds = selection.Bounds();
            UpdateDpi({bounds.right - 1, bounds.bottom - 1});
        } else {
            UpdateDpi(pointer);
        }
    }

    [[nodiscard]] RECT SnapAt(const POINT point) const noexcept {
        if (config.capture.windowSnap) {
            for (const RECT candidate : windows) {
                if (PtInRect(&candidate, point)) { return candidate; }
            }
        }
        return MonitorAt(point);
    }

    [[nodiscard]] RECT LabelBounds() const noexcept {
        const RECT rectangle = VisualSelection();
        if (IsRectEmpty(&rectangle)) { return {}; }
        const RECT monitor = MonitorAt({rectangle.left, rectangle.top});
        const LONG width = Scale(160);
        const LONG height = Scale(23);
        const LONG left = std::clamp(rectangle.left, monitor.left, std::max(monitor.left, monitor.right - width));
        const LONG top = rectangle.top - height >= monitor.top ? rectangle.top - height : rectangle.top;
        return {left, top, left + width, top + height};
    }

    [[nodiscard]] RECT MagnifierBounds() const noexcept {
        if (!config.capture.magnifier || (selection.HasSelection() && !selection.Dragging())) { return {}; }
        return LayoutMagnifier(pointer, MonitorAt(pointer), dpi).bounds;
    }

    [[nodiscard]] std::uint32_t SampledColor() const noexcept {
        const auto image = frozen->Image();
        const POINT source = Local(pointer);
        return image.Row(
                   std::clamp<int>(source.y, 0, image.Height() - 1))[std::clamp<int>(source.x, 0, image.Width() - 1)] &
               0xFFFFFFU;
    }

    [[nodiscard]] const Button* HoverButton() const noexcept {
        for (const auto& button : buttons) {
            if (PtInRect(&button.bounds, pointer)) { return &button; }
        }
        return nullptr;
    }

    [[nodiscard]] RECT TooltipBounds() const noexcept {
        const Button* button = HoverButton();
        if (!button || !*button->hint || (choosingGifFrameRate && button->kind == ButtonKind::GifFrameRatePicker)) {
            return {};
        }
        return PlaceToolbar(button->bounds, {Scale(210), Scale(25)}, MonitorAt(pointer), Scale(5));
    }

    void AddDamage(const RECT rectangle) {
        RECT clipped{};
        if (!IntersectRect(&clipped, &rectangle, &desktop)) { return; }
        if (damage.size() >= 96) {
            damage.clear();
            damage.push_back(desktop);
        } else {
            damage.push_back(clipped);
        }
        const RECT local = Local(clipped);
        InvalidateRect(window.Handle(), &local, FALSE);
    }

    void AddBorder(const RECT rectangle) {
        if (IsRectEmpty(&rectangle)) { return; }
        const LONG extent = Scale(6);
        AddDamage({rectangle.left - extent, rectangle.top - extent, rectangle.right + extent, rectangle.top + extent});
        AddDamage(
            {rectangle.left - extent, rectangle.bottom - extent, rectangle.right + extent, rectangle.bottom + extent});
        AddDamage({rectangle.left - extent, rectangle.top, rectangle.left + extent, rectangle.bottom});
        AddDamage({rectangle.right - extent, rectangle.top, rectangle.right + extent, rectangle.bottom});
    }

    void Difference(const RECT source, const RECT removed) {
        RECT overlap{};
        if (IsRectEmpty(&source)) { return; }
        if (!IntersectRect(&overlap, &source, &removed)) {
            AddDamage(source);
            return;
        }
        AddDamage({source.left, source.top, source.right, overlap.top});
        AddDamage({source.left, overlap.bottom, source.right, source.bottom});
        AddDamage({source.left, overlap.top, overlap.left, overlap.bottom});
        AddDamage({overlap.right, overlap.top, source.right, overlap.bottom});
    }

    struct UiSnapshot {
        RECT selection;
        RECT label;
        RECT toolbar;
        RECT magnifier;
        RECT tooltip;
        RECT annotation;
    };
    [[nodiscard]] UiSnapshot Snapshot() const noexcept {
        return {VisualSelection(), LabelBounds(), toolbar, MagnifierBounds(), TooltipBounds(), AnnotationUiBounds()};
    }

    void Changed(const UiSnapshot& old) {
        LayoutToolbar();
        const auto next = Snapshot();
        if (!EqualRect(old.selection, next.selection)) {
            Difference(old.selection, next.selection);
            Difference(next.selection, old.selection);
        }
        AddBorder(old.selection);
        AddBorder(next.selection);
        AddBorder(old.annotation);
        AddBorder(next.annotation);
        for (const RECT rectangle : {old.label, next.label, old.toolbar, next.toolbar, old.magnifier, next.magnifier,
                                     old.tooltip, next.tooltip}) {
            AddDamage(rectangle);
        }
    }

    void LayoutGifToolbar(const RECT controls, const RECT monitor, const LONG margin) {
        const LONG top = controls.top + margin;
        const LONG bottom = controls.top + Scale(32);
        const LONG left = controls.left + margin;
        const LONG right = controls.right - margin;
        const LONG width = right - left;
        const LONG recordRight = left + width * 2 / 5;
        const LONG cancelRight = left + width * 3 / 5;
        buttons.push_back({{left, top, recordRight, bottom},
                           ButtonKind::Confirm,
                           0,
                           Text(L"开始录制 (Enter)", L"Start recording (Enter)"),
                           ToolbarIcon::Record});
        buttons.push_back({{recordRight, top, cancelRight, bottom},
                           ButtonKind::Cancel,
                           0,
                           Text(L"取消 (Esc)", L"Cancel (Esc)"),
                           ToolbarIcon::Cancel});
        buttons.push_back({{cancelRight, top, right, bottom},
                           ButtonKind::GifFrameRatePicker,
                           0,
                           Text(L"选择录制帧率", L"Choose recording frame rate")});
        if (!choosingGifFrameRate) { return; }

        const LONG rowHeight = Scale(32);
        const LONG spaceBelow = monitor.bottom - controls.bottom;
        const LONG spaceAbove = controls.top - monitor.top;
        const bool below = spaceBelow >= rowHeight || spaceBelow >= spaceAbove;
        if (below) {
            toolbar.bottom += std::min(rowHeight, spaceBelow);
        } else {
            toolbar.top -= std::min(rowHeight, spaceAbove);
        }
        const LONG rowTop = below ? controls.bottom : toolbar.top + margin;
        const LONG rowBottom = below ? toolbar.bottom - margin : controls.top;
        std::array frameRates{5, 10, 15, 20, 24, 25, 30, 40, 50, config.gif.fps};
        std::sort(frameRates.begin(), frameRates.end());
        const auto count = static_cast<LONG>(std::unique(frameRates.begin(), frameRates.end()) - frameRates.begin());
        for (LONG index = 0; index < count; ++index) {
            buttons.push_back({{left + width * index / count, rowTop, left + width * (index + 1) / count, rowBottom},
                               ButtonKind::GifFrameRate,
                               frameRates[index]});
        }
    }

    void LayoutToolbar() {
        buttons.clear();
        toolbar = {};
        if (!selection.HasSelection() || selection.Dragging()) { return; }
        const RECT selected = selection.Bounds();
        const RECT monitor = MonitorAt({selected.right - 1, selected.bottom - 1});
        const auto propertyTool = PropertyTool();
        const bool properties = propertyTool != AnnotationTool::None;
        const LONG width = std::min<LONG>(Scale(mode == CaptureMode::Gif ? 320 : 400), monitor.right - monitor.left);
        toolbar = PlaceToolbar(selected, {width, Scale(properties ? 70 : 36)}, monitor, Scale(6));
        const LONG margin = Scale(4);
        if (mode == CaptureMode::Gif) {
            LayoutGifToolbar(toolbar, monitor, margin);
            return;
        }
        const LONG top = toolbar.top + margin;
        const LONG bottom = toolbar.top + Scale(32);
        LONG left = toolbar.left + margin;
        const LONG slot = (width - margin * 2) / 11;
        static constexpr std::array<const wchar_t*, 6> ChineseHints{L"矩形 (R)", L"椭圆 (E)", L"箭头 (A)",
                                                                    L"画笔 (P)", L"文字 (T)", L"马赛克 (M)"};
        static constexpr std::array<const wchar_t*, 6> EnglishHints{L"Rectangle (R)", L"Ellipse (E)", L"Arrow (A)",
                                                                    L"Pen (P)",       L"Text (T)",    L"Mosaic (M)"};
        static constexpr std::array toolIcons{ToolbarIcon::Rectangle, ToolbarIcon::Ellipse, ToolbarIcon::Arrow,
                                              ToolbarIcon::Pencil,    ToolbarIcon::Text,    ToolbarIcon::Mosaic};
        for (int index = 0; index < 6; ++index) {
            buttons.push_back({{left, top, left + slot, bottom},
                               ButtonKind::Tool,
                               index + 1,
                               english ? EnglishHints[index] : ChineseHints[index],
                               toolIcons[index]});
            left += slot;
        }
        const auto add = [&](const ButtonKind kind, const ToolbarIcon icon, const wchar_t* hint, const int slots = 1) {
            buttons.push_back({{left, top, left + slot * slots, bottom}, kind, 0, hint, icon});
            left += slot * slots;
        };
        add(ButtonKind::Save, ToolbarIcon::Save, Text(L"保存到文件 (Ctrl+S)", L"Save to file (Ctrl+S)"));
        const bool pinMode = mode == CaptureMode::Pin;
        if (pinMode) {
            add(ButtonKind::Copy, ToolbarIcon::Copy, Text(L"复制到剪贴板 (Ctrl+C)", L"Copy to clipboard (Ctrl+C)"));
        } else {
            add(ButtonKind::Pin, ToolbarIcon::Pin, Text(L"贴为参考图 (Ctrl+T)", L"Pin as reference (Ctrl+T)"));
        }
        add(ButtonKind::Cancel, ToolbarIcon::Cancel, Text(L"取消 (Esc)", L"Cancel (Esc)"));
        add(ButtonKind::Confirm, pinMode ? ToolbarIcon::Pin : ToolbarIcon::Check,
            pinMode ? Text(L"贴为参考图 (Enter)", L"Pin as reference (Enter)")
                    : Text(L"复制到剪贴板 (Enter / Ctrl+C)", L"Copy to clipboard (Enter / Ctrl+C)"),
            2);
        if (!properties) { return; }
        static constexpr std::array ChineseWidths{L"线宽 2 px", L"线宽 4 px", L"线宽 6 px"};
        static constexpr std::array EnglishWidths{L"Stroke width 2 px", L"Stroke width 4 px", L"Stroke width 6 px"};
        static constexpr std::array ChineseFonts{L"字号 14 px", L"字号 20 px", L"字号 28 px"};
        static constexpr std::array EnglishFonts{L"Font size 14 px", L"Font size 20 px", L"Font size 28 px"};
        static constexpr std::array ChineseMosaics{L"马赛克块 6 px", L"马赛克块 10 px", L"马赛克块 16 px"};
        static constexpr std::array EnglishMosaics{L"Mosaic block 6 px", L"Mosaic block 10 px", L"Mosaic block 16 px"};
        const bool mosaic = propertyTool == AnnotationTool::Mosaic;
        const int propertyCount = mosaic ? 3 : 11;
        const LONG propertySlot = (width - margin * 2) / propertyCount;
        left = toolbar.left + margin;
        for (int index = 0; index < propertyCount; ++index) {
            const auto kind = mosaic                                 ? ButtonKind::MosaicSize
                              : index < 8                            ? ButtonKind::Color
                              : propertyTool == AnnotationTool::Text ? ButtonKind::Font
                                                                     : ButtonKind::Width;
            const int value = mosaic || index < 8 ? index : index - 8;
            const auto icon = kind == ButtonKind::Width  ? ToolbarIcon::Stroke
                              : kind == ButtonKind::Font ? ToolbarIcon::Font
                                                         : ToolbarIcon::None;
            const auto hint = kind == ButtonKind::Color   ? Text(L"标注颜色", L"Annotation color")
                              : kind == ButtonKind::Width ? Text(ChineseWidths[value], EnglishWidths[value])
                              : kind == ButtonKind::Font  ? Text(ChineseFonts[value], EnglishFonts[value])
                                                          : Text(ChineseMosaics[value], EnglishMosaics[value]);
            buttons.push_back({{left, toolbar.top + Scale(38), left + propertySlot, toolbar.bottom - margin},
                               kind,
                               value,
                               hint,
                               icon});
            left += propertySlot;
        }
    }

    void DrawToolbar(const HDC context) {
        if (IsRectEmpty(&toolbar)) { return; }
        Fill(context, Local(toolbar), RGB(34, 36, 40));
        for (const auto& button : buttons) {
            const RECT local = Local(button.bounds);
            bool selected = false;
            if (annotations) {
                const auto style = PropertyStyle();
                selected = (button.kind == ButtonKind::Tool && static_cast<int>(PropertyTool()) == button.value) ||
                           (button.kind == ButtonKind::Color && style.color == PresetColors[button.value]) ||
                           (button.kind == ButtonKind::Width && style.strokeWidth == StrokeWidths[button.value]) ||
                           (button.kind == ButtonKind::Font && style.fontSize == FontSizes[button.value]) ||
                           (button.kind == ButtonKind::MosaicSize && style.mosaicSize == MosaicSizes[button.value]);
            }
            if (selected || button.kind == ButtonKind::Confirm ||
                (button.kind == ButtonKind::GifFrameRate && button.value == gifFramesPerSecond)) {
                Fill(context, local, Accent());
            } else if (PtInRect(&button.bounds, pointer)) {
                Fill(context, local, RGB(75, 78, 85));
            }
            if (button.kind == ButtonKind::Save) {
                Fill(context, {local.left, local.top + Scale(5), local.left + Scale(1), local.bottom - Scale(5)},
                     RGB(79, 83, 91));
            }
            if (button.icon != ToolbarIcon::None) {
                const int detail = button.kind == ButtonKind::Width ? StrokeWidths[button.value] : button.value;
                DrawToolbarIcon(context, local, button.icon, static_cast<int>(dpi), iconPen.Get(), detail);
            } else if (button.kind == ButtonKind::Color) {
                RECT swatch = local;
                InflateRect(&swatch, -Scale(6), -Scale(6));
                Fill(context, swatch, Rgb(PresetColors[button.value]));
            } else {
                RECT text = local;
                wchar_t rateLabel[16]{};
                if (button.kind == ButtonKind::GifFrameRatePicker) {
                    swprintf_s(rateLabel, local.right - local.left < Scale(65) ? L"%d ▾" : L"%d fps ▾",
                               gifFramesPerSecond);
                } else if (button.kind == ButtonKind::GifFrameRate) {
                    swprintf_s(rateLabel, L"%d", button.value);
                } else if (button.kind == ButtonKind::MosaicSize) {
                    swprintf_s(rateLabel, L"%d px", MosaicSizes[button.value]);
                }
                SetTextColor(context, RGB(245, 245, 245));
                DrawTextW(context, rateLabel, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }

    void DrawMagnifier(const HDC context) const {
        const RECT screenBounds = MagnifierBounds();
        if (IsRectEmpty(&screenBounds)) { return; }
        const RECT bounds = Local(screenBounds);
        const auto layout = LayoutMagnifier(pointer, MonitorAt(pointer), dpi);
        const RECT image = Local(layout.image);
        Fill(context, bounds, RGB(30, 32, 36));
        const auto view = frozen->Image();
        const POINT source = Local(pointer);
        std::array<std::uint32_t, 15 * 15> pixels{};
        for (int row = 0; row < 15; ++row) {
            const int sourceY = std::clamp<int>(source.y + row - 7, 0, view.Height() - 1);
            for (int column = 0; column < 15; ++column) {
                const int sourceX = std::clamp<int>(source.x + column - 7, 0, view.Width() - 1);
                pixels[static_cast<std::size_t>(row) * 15 + column] = view.Row(sourceY)[sourceX];
            }
        }
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = 15;
        information.bmiHeader.biHeight = -15;
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        SetStretchBltMode(context, COLORONCOLOR);
        StretchDIBits(context, image.left, image.top, image.right - image.left, image.bottom - image.top, 0, 0, 15, 15,
                      pixels.data(), &information, DIB_RGB_COLORS, SRCCOPY);
        SetDCBrushColor(context, RGB(255, 255, 255));
        const RECT center = Local(layout.center);
        FrameRect(context, &center, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const auto color = pixels[112] & 0xFFFFFFU;
        wchar_t caption[96]{};
        swprintf_s(caption, L"%ld, %ld\n#%06X  RGB %u %u %u", pointer.x, pointer.y, color, (color >> 16U) & 255U,
                   (color >> 8U) & 255U, color & 255U);
        RECT label = Local(layout.label);
        DrawTextW(context, caption, -1, &label, DT_CENTER | DT_NOPREFIX);
    }

    void PrepareMask(const RECT screenDirty) {
        const auto source = frozen->Image();
        const auto destination = backBuffer->Pixels();
        const RECT dirty = Local(screenDirty);
        for (LONG row = dirty.top; row < dirty.bottom; ++row) {
            auto* output = destination.data() + static_cast<std::size_t>(row) * source.Width() + dirty.left;
            const auto* input = source.Row(row) + dirty.left;
            for (LONG column = dirty.left; column < dirty.right; ++column) {
                const auto color = *input++;
                *output++ = 0xFF000000U | (((color >> 16U) & 255U) * brightness / 100U << 16U) |
                            (((color >> 8U) & 255U) * brightness / 100U << 8U) | ((color & 255U) * brightness / 100U);
            }
        }
    }

    void DrawAnnotationSelection(const HDC context) const {
        const RECT bounds = AnnotationUiBounds();
        if (IsRectEmpty(&bounds)) { return; }
        const SavedDc saved(context);
        const RECT clip = Local(selection.Bounds());
        IntersectClipRect(context, clip.left, clip.top, clip.right, clip.bottom);
        if (annotations->Selected()->tool != AnnotationTool::Arrow) {
            const RECT outline = Local(bounds);
            SetDCBrushColor(context, Accent());
            FrameRect(context, &outline, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        for (const auto& handle : annotations->Handles()) {
            const POINT point = Local(handle.point);
            const LONG radius = Scale(3);
            const RECT rectangle{point.x - radius, point.y - radius, point.x + radius + 1, point.y + radius + 1};
            Fill(context, rectangle, RGB(250, 250, 250));
            SetDCBrushColor(context, Accent());
            FrameRect(context, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
    }

    void Compose(const RECT screenDirty) {
        const HDC context = backBuffer->DeviceContext();
        const SavedDc saved(context);
        const RECT dirty = Local(screenDirty);
        IntersectClipRect(context, dirty.left, dirty.top, dirty.right, dirty.bottom);
        const RECT selected = VisualSelection();
        RECT visible{};
        if (IntersectRect(&visible, &selected, &screenDirty)) {
            if (mode != CaptureMode::Gif && renderer && selection.HasSelection() &&
                (annotations->HasAnnotations() || annotations->Preview())) {
                renderer->Draw(context, {desktop.left, desktop.top}, visible);
            } else {
                const RECT local = Local(visible);
                frozen->BlitTo(context, local, {local.left, local.top});
            }
        }
        SelectObject(context, font.Get());
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context, RGB(245, 245, 245));
        if (!IsRectEmpty(&selected)) {
            const RECT outline = Local(selected);
            SetDCBrushColor(context, Accent());
            FrameRect(context, &outline, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (selection.HasSelection() && !selection.Creating() &&
                (mode == CaptureMode::Gif || !annotations || !annotations->Selected())) {
                for (const POINT point : SelectionHandles(outline)) {
                    const LONG radius = Scale(3);
                    const RECT handle{point.x - radius, point.y - radius, point.x + radius + 1, point.y + radius + 1};
                    Fill(context, handle, RGB(250, 250, 250));
                    SetDCBrushColor(context, Accent());
                    FrameRect(context, &handle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                }
            }
            RECT label = Local(LabelBounds());
            Fill(context, label, RGB(30, 32, 36));
            const auto dimensions = std::to_wstring(selected.right - selected.left) + L" × " +
                                    std::to_wstring(selected.bottom - selected.top);
            DrawTextW(context, dimensions.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        DrawAnnotationSelection(context);
        DrawToolbar(context);
        DrawMagnifier(context);
        const Button* hovered = HoverButton();
        if (hovered && *hovered->hint) {
            RECT hint = Local(TooltipBounds());
            Fill(context, hint, RGB(55, 58, 65));
            DrawTextW(context, hovered->hint, -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (activationDeadline) {
            RECT hint = Local(activationHint);
            Fill(context, hint, RGB(30, 32, 36));
            DrawTextW(context, Text(L"拖动鼠标选择区域，右键取消", L"Drag to select an area or right-click to cancel"),
                      -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    void Paint() {
        const PaintScope paint(window.Handle());
        if (!active || !backBuffer) { return; }
        const RECT dirty = paint.Bounds();
        if (IsRectEmpty(&dirty)) { return; }
        // All CPU writes precede the GDI draw batch. The previous paint may
        // still be reading the back buffer when this one starts.
        if (!GdiFlush()) { throw std::runtime_error("Overlay GDI batch failed"); }
        if (damage.empty()) {
            RECT screenDirty = dirty;
            OffsetRect(&screenDirty, desktop.left, desktop.top);
            if (IntersectRect(&screenDirty, &screenDirty, &desktop)) {
                PrepareMask(screenDirty);
                Compose(screenDirty);
            }
        } else {
            for (const RECT screenDirty : damage) { PrepareMask(screenDirty); }
            for (const RECT screenDirty : damage) { Compose(screenDirty); }
            damage.clear();
        }
        if (!BitBlt(paint.Context(), dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                    backBuffer->DeviceContext(), dirty.left, dirty.top, SRCCOPY)) {
            throw std::runtime_error("Overlay present failed");
        }
    }

    bool CommitText() {
        if (textInput && textInput->Active()) { AddDamage(textInput->Commit()); }
        return !textInput || !textInput->Active();
    }

    void CreateEditor() {
        if (annotations) { return; }
        auto model = std::make_unique<AnnotationModel>(frozen->Image(), POINT{desktop.left, desktop.top});
        model->SetStyle({config.editor.color, config.editor.strokeWidth, config.editor.fontSize, config.editor.fontName,
                         config.editor.mosaicSize});
        model->SetSelection(selection.Bounds());
        auto painter = std::make_unique<EditorRenderer>(*model);
        auto input = std::make_unique<TextInput>(*model, *painter, window.Handle(), CommitTextMessage);
        annotations = std::move(model);
        renderer = std::move(painter);
        textInput = std::move(input);
    }

    void ResetSession() noexcept {
        active = false;
        EndActivationWait();
        saving = false;
        drawing = false;
        choosingGifFrameRate = false;
        if (GetCapture() == window.Handle()) { ReleaseCapture(); }
        textInput.reset();
        renderer.reset();
        annotations.reset();
        backBuffer.reset();
        frozen.reset();
        font.Reset();
        iconPen.Reset();
        windows.clear();
        monitors.clear();
        buttons.clear();
        damage.clear();
        selection.Clear();
        hover = {};
        toolbar = {};
        completion = {};
        saveRequested = {};
    }

    [[nodiscard]] OverlayAction DefaultAction() const noexcept {
        return mode == CaptureMode::Screenshot ? OverlayAction::Copy
               : mode == CaptureMode::Pin      ? OverlayAction::Pin
                                               : OverlayAction::Record;
    }

    void Finish(const OverlayAction action, const bool imageAlreadySaved = false) {
        if (!active || (action != OverlayAction::Cancel && !selection.HasSelection())) { return; }
        OverlayResult result;
        result.mode = mode;
        result.action = action;
        result.selection = selection.Bounds();
        result.appearanceMilliseconds = appearanceMilliseconds;
        if (action == OverlayAction::Record) { result.gifFramesPerSecond = gifFramesPerSecond; }
        if (action != OverlayAction::Cancel && action != OverlayAction::Record && !imageAlreadySaved) {
            if (!CommitText()) { return; }
            result.image = renderer->Flatten(result.selection);
        }
        auto completed = std::move(completion);
        const HWND restoreFocus = previousForeground;
        const bool restorePrevious = GetForegroundWindow() == window.Handle();
        ResetSession();
        ShowWindow(window.Handle(), SW_HIDE);
        if (restorePrevious && IsWindow(restoreFocus)) { SetForegroundWindow(restoreFocus); }
        if (completed) { completed(std::move(result)); }
        // Callback is deliberately the last use of this session/object.
    }

    void Save() {
        if (!selection.HasSelection() || !renderer) { return; }
        if (!saveRequested) {
            Finish(OverlayAction::Save);
            return;
        }
        if (!CommitText()) { return; }
        Bitmap output = renderer->Flatten(selection.Bounds());
        const auto chooseSave = saveRequested;
        // The native modal dialog stays owned by a visible foreground window.
        // A display change or exit can still cancel the overlay in its modal loop.
        saving = true;
        bool accepted = false;
        try {
            accepted = chooseSave(std::move(output), selection.Bounds());
        } catch (...) {
            saving = false;
            if (active) { CheckFocusLater(); }
            throw;
        }
        saving = false;
        if (!active) { return; }
        if (accepted) {
            Finish(OverlayAction::Save, true);
            return;
        }
        CheckFocusLater();
        AddDamage(desktop);
    }

    void SetTool(const AnnotationTool tool) {
        if (mode == CaptureMode::Gif || !annotations || !selection.HasSelection()) { return; }
        CancelGesture();
        if (GetCapture() == window.Handle()) { ReleaseCapture(); }
        const auto old = Snapshot();
        if (!CommitText()) { return; }
        AddDamage(annotations->CancelPreview());
        const bool turnOff = !annotations->Selected() && annotations->Tool() == tool;
        annotations->SetTool(turnOff ? AnnotationTool::None : tool);
        Changed(old);
    }

    void BeginTextEdit() {
        if (!annotations || !annotations->Selected() || annotations->Selected()->tool != AnnotationTool::Text) {
            return;
        }
        const auto old = Snapshot();
        annotations->SetStyle(annotations->SelectedStyle());
        textInput->BeginEdit(*annotations->SelectedIndex());
        AddDamage(annotations->Selected()->bounds);
        Changed(old);
    }

    void ChangeProperty(const Button button) {
        if (!annotations) { return; }
        const auto old = Snapshot();
        auto style = PropertyStyle();
        if (button.kind == ButtonKind::Color) { style.color = PresetColors[button.value]; }
        if (button.kind == ButtonKind::Width) { style.strokeWidth = StrokeWidths[button.value]; }
        if (button.kind == ButtonKind::Font) { style.fontSize = FontSizes[button.value]; }
        if (button.kind == ButtonKind::MosaicSize) { style.mosaicSize = MosaicSizes[button.value]; }
        if (textInput && textInput->Active()) {
            textInput->ApplyStyle(style);
        } else if (annotations->Selected() && annotations->Selected()->tool == AnnotationTool::Text) {
            BeginTextEdit();
            textInput->ApplyStyle(style);
            AddDamage(textInput->Commit());
        } else if (annotations->Selected()) {
            AddDamage(annotations->ApplySelectedStyle(style));
        }
        annotations->SetStyle(style);
        Changed(old);
    }

    void ToggleGifFrameRatePicker() {
        const auto old = Snapshot();
        choosingGifFrameRate = !choosingGifFrameRate;
        Changed(old);
    }

    void Activate(const Button button) {
        if (button.kind == ButtonKind::Color || button.kind == ButtonKind::Width || button.kind == ButtonKind::Font ||
            button.kind == ButtonKind::MosaicSize) {
            ChangeProperty(button);
            return;
        }
        if (!CommitText()) { return; }
        switch (button.kind) {
        case ButtonKind::Tool:
            SetTool(static_cast<AnnotationTool>(button.value));
            break;
        case ButtonKind::Save:
            Save();
            break;
        case ButtonKind::Copy:
            Finish(OverlayAction::Copy);
            break;
        case ButtonKind::Pin:
            Finish(OverlayAction::Pin);
            break;
        case ButtonKind::Cancel:
            Finish(OverlayAction::Cancel);
            break;
        case ButtonKind::Confirm:
            Finish(DefaultAction());
            break;
        case ButtonKind::GifFrameRate:
            gifFramesPerSecond = button.value;
            [[fallthrough]];
        case ButtonKind::GifFrameRatePicker:
            ToggleGifFrameRatePicker();
            break;
        default:
            break;
        }
    }

    void PointerMove(const POINT point) {
        const auto old = Snapshot();
        pointer = point;
        if (drawing) {
            AddDamage(annotations->PointerMove(point, (GetKeyState(VK_SHIFT) & 0x8000) != 0));
        } else if (annotations && annotations->Editing()) {
            AddDamage(annotations->UpdateEdit(point, (GetKeyState(VK_SHIFT) & 0x8000) != 0));
        } else if (selection.Dragging()) {
            selection.Drag(point);
            if (annotations) { annotations->SetSelection(selection.Bounds()); }
        } else if (!selection.HasSelection()) {
            UpdateDpi(point);
            hover = SnapAt(point);
        }
        Changed(old);
    }

    void PointerDown(const POINT point) {
        pointer = point;
        if (const Button* button = HoverButton()) {
            Activate(*button);
            return;
        }
        if (choosingGifFrameRate) {
            ToggleGifFrameRatePicker();
            return;
        }
        const auto old = Snapshot();
        if (!CommitText()) { return; }
        if (mode != CaptureMode::Gif && annotations && selection.HasSelection() &&
            annotations->BeginEdit(point, Scale(5))) {
            SetCapture(window.Handle());
            Changed(old);
            return;
        }
        if (annotations) { annotations->ClearSelected(); }
        const auto hit = HitSelection(selection.Bounds(), point, Scale(5));
        const bool toolActive = mode != CaptureMode::Gif && annotations && annotations->Tool() != AnnotationTool::None;
        if (selection.HasSelection() && hit == SelectionHandle::None && annotations &&
            (toolActive || annotations->CanUndo() || annotations->CanRedo())) {
            Changed(old);
            return;
        }
        if (toolActive && hit == SelectionHandle::Move) {
            const UINT toolDpi = DpiAt(point);
            if (annotations->Tool() == AnnotationTool::Text) {
                textInput->Begin(point, toolDpi);
                Changed(old);
                return;
            }
            drawing = true;
            AddDamage(annotations->PointerDown(point, (GetKeyState(VK_SHIFT) & 0x8000) != 0, toolDpi));
        } else {
            if (!selection.HasSelection()) { UpdateDpi(point); }
            selection.Begin(point, Scale(5));
        }
        SetCapture(window.Handle());
        Changed(old);
    }

    void PointerUp(const POINT point) {
        const auto old = Snapshot();
        if (drawing) {
            AddDamage(annotations->PointerUp(point, (GetKeyState(VK_SHIFT) & 0x8000) != 0));
            drawing = false;
        } else if (annotations && annotations->Editing()) {
            AddDamage(annotations->UpdateEdit(point, (GetKeyState(VK_SHIFT) & 0x8000) != 0));
            AddDamage(annotations->CommitEdit());
        } else if (selection.Dragging()) {
            selection.End(point, SnapAt(point));
            if (annotations) { annotations->SetSelection(selection.Bounds()); }
        } else {
            return;
        }
        if (GetCapture() == window.Handle()) { ReleaseCapture(); }
        UpdateSelectionDpi();
        Changed(old);
        if (mode == CaptureMode::Pin && config.pin.instant && selection.HasSelection()) { Finish(OverlayAction::Pin); }
    }

    void CancelGesture() {
        if (!drawing && !selection.Dragging() && !(annotations && annotations->Editing())) { return; }
        const auto old = Snapshot();
        if (drawing) {
            drawing = false;
            AddDamage(annotations->CancelPreview());
        }
        if (annotations && annotations->Editing()) { AddDamage(annotations->CancelEdit()); }
        selection.CancelDrag();
        if (annotations) { annotations->SetSelection(selection.Bounds()); }
        UpdateSelectionDpi();
        Changed(old);
    }

    void RightClick() {
        if (!HasFocus()) {
            Finish(OverlayAction::Cancel);
            return;
        }
        if (choosingGifFrameRate) {
            ToggleGifFrameRatePicker();
            return;
        }
        if (textInput && textInput->Active()) {
            CommitText();
            return;
        }
        if (mode != CaptureMode::Gif && annotations && annotations->Selected()) {
            CancelGesture();
            if (GetCapture() == window.Handle()) { ReleaseCapture(); }
            const auto old = Snapshot();
            annotations->ClearSelected();
            Changed(old);
            return;
        }
        if (mode != CaptureMode::Gif && annotations && annotations->Tool() != AnnotationTool::None) {
            SetTool(annotations->Tool());
            return;
        }
        if (!selection.HasSelection()) {
            Finish(OverlayAction::Cancel);
            return;
        }
        if (annotations && (annotations->CanUndo() || annotations->CanRedo())) {
            Finish(OverlayAction::Cancel);
            return;
        }
        const auto old = Snapshot();
        selection.Clear();
        UpdateSelectionDpi();
        hover = SnapAt(pointer);
        Changed(old);
    }

    void DoubleClick(const POINT point) {
        pointer = point;
        if (HoverButton() || !selection.HasSelection()) { return; }
        const auto old = Snapshot();
        if (!CommitText()) { return; }
        if (mode != CaptureMode::Gif && annotations && annotations->SelectAt(point, Scale(5))) {
            if (annotations->Selected()->tool == AnnotationTool::Text) { BeginTextEdit(); }
            Changed(old);
            return;
        }
        const auto bounds = selection.Bounds();
        if (PtInRect(&bounds, point)) { Finish(DefaultAction()); }
    }

    void SelectBounds(const RECT bounds) {
        if (selection.Dragging() || drawing || (annotations && annotations->Editing()) || !CommitText()) { return; }
        const auto old = Snapshot();
        selection.Set(bounds);
        if (annotations) {
            annotations->SetSelection(selection.Bounds());
            annotations->ClearSelected();
        }
        choosingGifFrameRate = false;
        UpdateSelectionDpi();
        Changed(old);
    }

    void Key(const WPARAM key) {
        if (key == VK_ESCAPE) {
            if (choosingGifFrameRate) {
                ToggleGifFrameRatePicker();
            } else if (textInput && textInput->Active()) {
                CommitText();
            } else if (selection.Dragging()) {
                Finish(OverlayAction::Cancel);
            } else if (drawing || (annotations && annotations->Editing())) {
                CancelGesture();
                if (GetCapture() == window.Handle()) { ReleaseCapture(); }
            } else if (annotations && annotations->Selected() && mode != CaptureMode::Gif) {
                const auto old = Snapshot();
                annotations->ClearSelected();
                Changed(old);
            } else {
                Finish(OverlayAction::Cancel);
            }
            return;
        }
        if (key == VK_RETURN) {
            if (mode != CaptureMode::Gif && annotations && annotations->Selected() &&
                annotations->Selected()->tool == AnnotationTool::Text) {
                BeginTextEdit();
                return;
            }
            Finish(DefaultAction());
            return;
        }
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        // The native text editor owns its selection, Tab and character input.
        if (textInput && textInput->Active()) { return; }
        if (alt) { return; }
        if (control && key == 'A') {
            SelectBounds(MonitorAt(pointer));
            return;
        }
        if (!control && key == VK_TAB) {
            if (const auto candidate = CycleWindowSelection(windows, VisualSelection(), shift)) {
                SelectBounds(*candidate);
            }
            return;
        }
        if (!control && key == 'C') {
            CopyText(window.Handle(), FormatSampledColor(SampledColor(), shift));
            return;
        }
        if (!selection.HasSelection()) { return; }
        if (control && annotations && mode != CaptureMode::Gif) {
            if (key == 'C') {
                Finish(OverlayAction::Copy);
                return;
            }
            if (key == 'S') {
                Save();
                return;
            }
            if (key == 'T') {
                Finish(OverlayAction::Pin);
                return;
            }
            if (!CommitText()) { return; }
            if (key == 'Z') {
                CancelGesture();
                if (GetCapture() == window.Handle()) { ReleaseCapture(); }
                const auto old = Snapshot();
                AddDamage(annotations->Undo());
                Changed(old);
                return;
            }
            if (key == 'Y') {
                CancelGesture();
                if (GetCapture() == window.Handle()) { ReleaseCapture(); }
                const auto old = Snapshot();
                AddDamage(annotations->Redo());
                Changed(old);
                return;
            }
        }
        if (key == VK_DELETE && annotations && annotations->Selected() && mode != CaptureMode::Gif) {
            CancelGesture();
            if (GetCapture() == window.Handle()) { ReleaseCapture(); }
            const auto old = Snapshot();
            AddDamage(annotations->RemoveSelected());
            Changed(old);
            return;
        }
        if (key >= VK_LEFT && key <= VK_DOWN) {
            if (selection.Dragging() || drawing || (annotations && annotations->Editing()) || !CommitText()) { return; }
            const auto old = Snapshot();
            const LONG step = shift ? 10 : 1;
            const LONG horizontal = key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0;
            const LONG vertical = key == VK_UP ? -step : key == VK_DOWN ? step : 0;
            if (mode != CaptureMode::Gif && annotations && annotations->Selected()) {
                AddDamage(annotations->NudgeSelected(horizontal, vertical));
            } else if (control) {
                selection.ResizeBy(horizontal, vertical);
            } else {
                selection.Nudge(horizontal, vertical);
            }
            if (annotations && !annotations->Selected()) { annotations->SetSelection(selection.Bounds()); }
            UpdateSelectionDpi();
            Changed(old);
            return;
        }
        if (control) { return; }
        switch (key) {
        case 'R':
            SetTool(AnnotationTool::Rectangle);
            break;
        case 'E':
            SetTool(AnnotationTool::Ellipse);
            break;
        case 'A':
            SetTool(AnnotationTool::Arrow);
            break;
        case 'P':
            SetTool(AnnotationTool::Pen);
            break;
        case 'T':
            SetTool(AnnotationTool::Text);
            break;
        case 'M':
            SetTool(AnnotationTool::Mosaic);
            break;
        default:
            break;
        }
    }

    void SetPointerCursor() const {
        LPCWSTR cursor = IDC_CROSS;
        if (HoverButton()) {
            cursor = IDC_HAND;
        } else if (mode != CaptureMode::Gif && annotations &&
                   (annotations->HitSelected(pointer, Scale(5)) != AnnotationHandle::None ||
                    annotations->HitTest(pointer, Scale(5)))) {
            const auto handle = annotations->HitSelected(pointer, Scale(5));
            if (handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::BottomRight) {
                cursor = IDC_SIZENWSE;
            } else if (handle == AnnotationHandle::TopRight || handle == AnnotationHandle::BottomLeft) {
                cursor = IDC_SIZENESW;
            } else if (handle == AnnotationHandle::Top || handle == AnnotationHandle::Bottom) {
                cursor = IDC_SIZENS;
            } else if (handle == AnnotationHandle::Left || handle == AnnotationHandle::Right) {
                cursor = IDC_SIZEWE;
            } else if (handle == AnnotationHandle::Start || handle == AnnotationHandle::End) {
                cursor = IDC_CROSS;
            } else {
                cursor = IDC_SIZEALL;
            }
        } else if (selection.HasSelection()) {
            const auto handle = HitSelection(selection.Bounds(), pointer, Scale(5));
            if (handle == SelectionHandle::TopLeft || handle == SelectionHandle::BottomRight) {
                cursor = IDC_SIZENWSE;
            } else if (handle == SelectionHandle::TopRight || handle == SelectionHandle::BottomLeft) {
                cursor = IDC_SIZENESW;
            } else if (handle == SelectionHandle::Top || handle == SelectionHandle::Bottom) {
                cursor = IDC_SIZENS;
            } else if (handle == SelectionHandle::Left || handle == SelectionHandle::Right) {
                cursor = IDC_SIZEWE;
            } else if (handle == SelectionHandle::Move &&
                       (!annotations || annotations->Tool() == AnnotationTool::None)) {
                cursor = IDC_SIZEALL;
            } else if (annotations && annotations->Tool() == AnnotationTool::Text) {
                cursor = IDC_IBEAM;
            }
        }
        SetCursor(LoadCursorW(nullptr, cursor));
    }
};

Overlay::Overlay(const HWND owner) : state_(std::make_unique<State>(*this)) {
    Create({L"VisPick.Overlay", CS_DBLCLKS, IDC_CROSS}, L"VisPick selection", WS_POPUP | WS_CLIPCHILDREN,
           WS_EX_TOOLWINDOW | WS_EX_TOPMOST, {0, 0, 1, 1}, WindowActivation::Interactive, owner);
    DisableWindowAnimations(Handle());
}

Overlay::~Overlay() {
    state_->ResetSession();
    Destroy();
}
bool Overlay::Active() const noexcept {
    return state_->active;
}
void Overlay::Cancel() {
    state_->Finish(OverlayAction::Cancel);
}

void Overlay::SwitchMode(const CaptureMode mode) {
    auto& state = *state_;
    if (!state.active || state.saving) { return; }
    if (state.activationDeadline && state.TryFocus()) { state.Activated(); }
    if (state.mode == mode || !state.CommitText()) { return; }
    const auto old = state.Snapshot();
    state.CancelGesture();
    if (GetCapture() == Handle()) { ReleaseCapture(); }
    if (mode != CaptureMode::Gif) { state.CreateEditor(); }
    state.choosingGifFrameRate = false;
    state.mode = mode;
    state.Changed(old);
    state.AddDamage(state.selection.Bounds()); // GIF hides the editor without losing its history.
}

void Overlay::Begin(const CaptureMode mode, const Config& config, std::function<void(OverlayResult)> completion,
                    std::function<bool(Bitmap, RECT)> saveRequested) {
    if (Active()) { return; }
    const auto started = std::chrono::steady_clock::now();
    auto& state = *state_;
    state.config = config;
    state.mode = mode;
    state.gifFramesPerSecond = config.gif.fps;
    state.english =
        config.general.language == Language::English ||
        (config.general.language == Language::Auto && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
    state.previousForeground = GetForegroundWindow();
    try {
        state.desktop = VirtualDesktopBounds();
        const auto width = static_cast<std::int64_t>(state.desktop.right) - state.desktop.left;
        const auto height = static_cast<std::int64_t>(state.desktop.bottom) - state.desktop.top;
        // Mouse message coordinates are signed 16-bit. Check before allocating
        // two full desktop DIBs, including on 32-bit builds with fragmented VA.
        constexpr std::int64_t maximumDesktopPixels = sizeof(void*) == 4 ? 32'000'000 : 64'000'000;
        constexpr auto maximumMouseExtent = std::numeric_limits<short>::max();
        if (width <= 0 || height <= 0 || width > maximumMouseExtent || height > maximumMouseExtent ||
            width * height > maximumDesktopPixels) {
            throw std::length_error(state.english ? "The desktop exceeds the selection overlay size limit."
                                                  : "虚拟桌面尺寸超出选区浮层支持范围。");
        }
        state.monitors = EnumerateMonitors();
        WindowSnapshot snapshot{Handle(), state.desktop, {}, {}};
        const BOOL enumerated = EnumWindows(SnapshotWindow, reinterpret_cast<LPARAM>(&snapshot));
        if (snapshot.error) { std::rethrow_exception(snapshot.error); }
        if (!enumerated) { throw std::runtime_error("Enumerate capture windows failed"); }
        state.windows = std::move(snapshot.rectangles);
        state.frozen = CaptureScreen(state.desktop, true, config.capture.includeCursor);
        state.backBuffer = std::make_unique<DibSection>(static_cast<int>(width), static_cast<int>(height));
        state.brightness = static_cast<unsigned>(100 - std::clamp(config.capture.maskOpacity, 0, 100));
        state.selection.Reset(state.desktop);
        if (mode != CaptureMode::Gif) { state.CreateEditor(); }
        GetCursorPos(&state.pointer);
        state.UpdateDpi(state.pointer);
        state.hover = state.SnapAt(state.pointer);
        state.completion = std::move(completion);
        state.saveRequested = std::move(saveRequested);
        state.active = true;
        if (!SetWindowPos(Handle(), HWND_TOPMOST, state.desktop.left, state.desktop.top, static_cast<int>(width),
                          static_cast<int>(height), SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
            throw std::runtime_error("Show selection overlay failed");
        }
        RECT visibleBounds{};
        if (!GetWindowRect(Handle(), &visibleBounds) || !EqualRect(visibleBounds, state.desktop)) {
            throw std::runtime_error("Selection overlay bounds changed during activation");
        }
        // The global hotkey requests capture even if Windows denies activation.
        // A click on the overlay can activate it without synthetic input.
        if (!state.TryFocus()) { state.AwaitActivation(); }
        state.AddDamage(state.desktop);
        UpdateWindow(Handle());
        GdiFlush();
        // When composition is enabled, include the compositor handoff in the
        // activation measurement. Win7 without DWM simply returns a failure.
        DwmFlush();
        state.appearanceMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    } catch (...) {
        state.ResetSession();
        ShowWindow(Handle(), SW_HIDE);
        throw;
    }
}

LRESULT Overlay::OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) {
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT) {
        state_->Paint();
        return 0;
    }
    if (!state_->active) { return Window::OnMessage(message, parameter, data); }
    const POINT point{state_->desktop.left + GET_X_LPARAM(data), state_->desktop.top + GET_Y_LPARAM(data)};
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(parameter) == WA_INACTIVE) { state_->CheckFocusLater(); }
        break;
    case WM_KILLFOCUS:
        if (!state_->textInput || reinterpret_cast<HWND>(parameter) != state_->textInput->Handle()) {
            state_->CheckFocusLater();
        }
        break;
    case WM_SETFOCUS:
        state_->Activated();
        break;
    case CheckFocusMessage:
        state_->CheckFocus();
        return 0;
    case WM_TIMER:
        if (parameter == ActivationWatch) {
            if (state_->activationDeadline) { state_->CheckFocus(); }
            return 0;
        }
        break;
    case WM_DPICHANGED:
        return 0; // The overlay spans the physical virtual desktop, independent of its HWND DPI.
    case WM_MOUSEMOVE:
        if (!state_->activationDeadline) { state_->PointerMove(point); }
        return 0;
    case WM_LBUTTONDOWN:
        if (state_->activationDeadline) {
            if (!state_->TryFocus()) { return 0; }
            state_->Activated();
        }
        state_->PointerDown(point);
        return 0;
    case WM_LBUTTONUP:
        state_->PointerUp(point);
        return 0;
    case WM_LBUTTONDBLCLK:
        state_->DoubleClick(point);
        return 0;
    case WM_RBUTTONUP:
        state_->RightClick();
        return 0;
    case WM_KEYDOWN:
        state_->Key(parameter);
        return 0;
    case WM_SETCURSOR:
        state_->SetPointerCursor();
        return TRUE;
    case WM_CLOSE:
        Cancel();
        return 0;
    case WM_DISPLAYCHANGE:
        Cancel();
        return 0;
    case WM_CAPTURECHANGED:
        state_->CancelGesture();
        return 0;
    case CommitTextMessage:
        if (state_->textInput) {
            const auto old = state_->Snapshot();
            state_->AddDamage(state_->textInput->CommitRequested(parameter));
            state_->Changed(old);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (state_->textInput) {
            if (const HBRUSH brush =
                    state_->textInput->ControlColor(reinterpret_cast<HDC>(parameter), reinterpret_cast<HWND>(data))) {
                return reinterpret_cast<LRESULT>(brush);
            }
        }
        break;
    default:
        break;
    }
    return Window::OnMessage(message, parameter, data);
}

} // namespace vispick
