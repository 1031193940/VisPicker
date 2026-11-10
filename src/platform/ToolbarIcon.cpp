#include "platform/ToolbarIcon.h"
#include "platform/GdiPlus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <stdexcept>

namespace vispick {
namespace {

class IconCanvas {
public:
    IconCanvas(const HDC context, const LONG left, const LONG top, const LONG size, const HPEN strokePen) noexcept
        : context_(context), left_(static_cast<float>(left)), top_(static_cast<float>(top)),
          scale_(static_cast<float>(size) / 24.0F) {
        if (!EnsureGdiPlus()) { return; }
        LOGPEN description{};
        if (GetObjectW(strokePen, 0, nullptr) != static_cast<int>(sizeof(description)) ||
            !GetObjectW(strokePen, sizeof(description), &description)) {
            return;
        }
        graphics_.emplace(context);
        stroke_.emplace(Color(description.lopnColor), static_cast<float>(std::max(1L, description.lopnWidth.x)));
        if (!Succeeded(graphics_->GetLastStatus()) || !Succeeded(stroke_->GetLastStatus()) ||
            !Succeeded(graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias)) ||
            !Succeeded(graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf)) ||
            !Succeeded(stroke_->SetLineJoin(Gdiplus::LineJoinRound)) ||
            !Succeeded(stroke_->SetStartCap(Gdiplus::LineCapRound)) ||
            !Succeeded(stroke_->SetEndCap(Gdiplus::LineCapRound))) {
            return;
        }
    }

    void Path(const std::initializer_list<POINT> points) noexcept {
        std::array<Gdiplus::PointF, 16> vertices{};
        if (points.size() < 2 || points.size() > vertices.size()) { return; }
        std::size_t count = 0;
        for (const auto point : points) { vertices[count++] = {X(point.x), Y(point.y)}; }
        if (graphics_ && Succeeded(graphics_->DrawLines(&*stroke_, vertices.data(), static_cast<INT>(count)))) {
            return;
        }
        MoveToEx(context_, Round(vertices[0].X), Round(vertices[0].Y), nullptr);
        for (std::size_t position = 1; position < count; ++position) {
            LineTo(context_, Round(vertices[position].X), Round(vertices[position].Y));
        }
    }

    void Rectangle(const RECT bounds) noexcept {
        const auto rectangle = Physical(bounds);
        if (graphics_ && Succeeded(graphics_->DrawRectangle(&*stroke_, rectangle))) { return; }
        ::Rectangle(context_, Round(rectangle.X), Round(rectangle.Y), Round(rectangle.GetRight()),
                    Round(rectangle.GetBottom()));
    }

    void Ellipse(const RECT bounds, const bool filled = false) noexcept {
        const auto rectangle = Physical(bounds);
        if (graphics_) {
            const Gdiplus::SolidBrush brush(Color(RGB(245, 245, 245)));
            if (Succeeded(filled ? graphics_->FillEllipse(&brush, rectangle)
                                 : graphics_->DrawEllipse(&*stroke_, rectangle))) {
                return;
            }
        }
        if (filled) {
            SelectObject(context_, GetStockObject(DC_BRUSH));
            SetDCBrushColor(context_, RGB(245, 245, 245));
        }
        ::Ellipse(context_, Round(rectangle.X), Round(rectangle.Y), Round(rectangle.GetRight()),
                  Round(rectangle.GetBottom()));
        if (filled) { SelectObject(context_, GetStockObject(NULL_BRUSH)); }
    }

    void Fill(const RECT bounds, const COLORREF color) noexcept { FillPhysical(Physical(bounds), color); }

    void Stroke(const LONG height) noexcept {
        FillPhysical({X(4), Y(12) - static_cast<float>(height) * 0.5F, X(20) - X(4), static_cast<float>(height)},
                     RGB(245, 245, 245));
    }

private:
    [[nodiscard]] static Gdiplus::Color Color(const COLORREF color) noexcept {
        return {255, GetRValue(color), GetGValue(color), GetBValue(color)};
    }
    [[nodiscard]] static LONG Round(const float coordinate) noexcept {
        return static_cast<LONG>(std::lround(coordinate));
    }
    [[nodiscard]] float X(const LONG unit) const noexcept { return left_ + static_cast<float>(unit) * scale_; }
    [[nodiscard]] float Y(const LONG unit) const noexcept { return top_ + static_cast<float>(unit) * scale_; }
    [[nodiscard]] Gdiplus::RectF Physical(const RECT bounds) const noexcept {
        return {X(bounds.left), Y(bounds.top), X(bounds.right) - X(bounds.left), Y(bounds.bottom) - Y(bounds.top)};
    }
    [[nodiscard]] bool Succeeded(const Gdiplus::Status status) noexcept {
        if (status == Gdiplus::Ok) { return true; }
        // Release the Graphics before using GDI on the same HDC again.
        stroke_.reset();
        graphics_.reset();
        return false;
    }
    void FillPhysical(const Gdiplus::RectF bounds, const COLORREF color) noexcept {
        if (graphics_) {
            const Gdiplus::SolidBrush brush(Color(color));
            if (Succeeded(graphics_->FillRectangle(&brush, bounds))) { return; }
        }
        const RECT rectangle{Round(bounds.X), Round(bounds.Y), Round(bounds.GetRight()), Round(bounds.GetBottom())};
        SetDCBrushColor(context_, color);
        FillRect(context_, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    HDC context_;
    float left_;
    float top_;
    float scale_;
    std::optional<Gdiplus::Graphics> graphics_;
    std::optional<Gdiplus::Pen> stroke_;
};

} // namespace

UniqueGdiObject<HPEN> CreateToolbarIconPen(int dpi) {
    UniqueGdiObject<HPEN> pen(CreatePen(PS_SOLID, std::max(1, MulDiv(3, dpi, 192)), RGB(245, 245, 245)));
    if (!pen.Get()) { throw std::runtime_error("Create toolbar icon pen failed"); }
    return pen;
}

void DrawToolbarIcon(HDC context, RECT bounds, ToolbarIcon icon, int dpi, HPEN strokePen, int detail) noexcept {
    if (!context || !strokePen || icon == ToolbarIcon::None) { return; }
    const auto scale = [dpi](int logical) { return MulDiv(logical, dpi, 96); };
    // Draw each glyph on the same centered 24-unit grid, including in the wider confirm button.
    const LONG size = std::min(
        {static_cast<LONG>(scale(22)), bounds.right - bounds.left - scale(4), bounds.bottom - bounds.top - scale(4)});
    if (size <= 0) { return; }
    const LONG left = (bounds.left + bounds.right - size) / 2;
    const LONG top = (bounds.top + bounds.bottom - size) / 2;
    const int saved = SaveDC(context);
    if (saved == 0) { return; }
    struct RestoreContext {
        HDC context;
        int saved;
        ~RestoreContext() { RestoreDC(context, saved); }
    } restore{context, saved};
    SelectObject(context, strokePen);
    SelectObject(context, GetStockObject(NULL_BRUSH));
    IconCanvas canvas(context, left, top, size, strokePen);
    const auto path = [&](std::initializer_list<POINT> points) { canvas.Path(points); };
    switch (icon) {
    case ToolbarIcon::Rectangle:
        canvas.Rectangle({3, 6, 21, 18});
        break;
    case ToolbarIcon::Ellipse:
        canvas.Ellipse({3, 6, 21, 18});
        break;
    case ToolbarIcon::Arrow:
        path({{5, 19}, {19, 5}, {19, 12}});
        path({{12, 5}, {19, 5}});
        break;
    case ToolbarIcon::Pencil:
        path({{4, 20}, {5, 15}, {15, 5}, {19, 9}, {9, 19}, {4, 20}});
        path({{12, 8}, {16, 12}});
        break;
    case ToolbarIcon::Text:
        path({{5, 6}, {19, 6}});
        path({{12, 6}, {12, 20}});
        break;
    case ToolbarIcon::Mosaic:
        canvas.Fill({4, 4, 11, 11}, RGB(245, 245, 245));
        canvas.Fill({13, 4, 20, 11}, RGB(132, 138, 148));
        canvas.Fill({4, 13, 11, 20}, RGB(132, 138, 148));
        canvas.Fill({13, 13, 20, 20}, RGB(245, 245, 245));
        break;
    case ToolbarIcon::Save:
        path({{4, 4}, {16, 4}, {20, 8}, {20, 20}, {4, 20}, {4, 4}});
        path({{8, 4}, {8, 10}, {15, 10}, {15, 4}});
        path({{8, 20}, {8, 14}, {16, 14}, {16, 20}});
        break;
    case ToolbarIcon::Copy:
        canvas.Rectangle({8, 8, 21, 21});
        path({{5, 16}, {4, 16}, {4, 4}, {16, 4}, {16, 5}});
        break;
    case ToolbarIcon::Pin:
        path({{15, 4}, {21, 10}, {17, 11}, {13, 15}, {12, 19}, {5, 12}, {9, 11}, {13, 7}, {15, 4}});
        path({{9, 16}, {4, 21}});
        break;
    case ToolbarIcon::Cancel:
        path({{6, 6}, {18, 18}});
        path({{18, 6}, {6, 18}});
        break;
    case ToolbarIcon::Check:
        path({{4, 12}, {9, 17}, {20, 6}});
        break;
    case ToolbarIcon::Stroke: {
        canvas.Stroke(scale(detail));
        break;
    }
    case ToolbarIcon::Font: {
        const LONG inset = 3 - std::clamp(detail, 0, 2);
        const LONG apex = 4 + inset;
        const LONG baseline = 20 - inset;
        const LONG halfWidth = 8 - inset;
        const LONG crossbar = MulDiv(halfWidth, 14 - apex, baseline - apex);
        path({{12 - halfWidth, baseline}, {12, apex}, {12 + halfWidth, baseline}});
        path({{12 - crossbar, 14}, {12 + crossbar, 14}});
        break;
    }
    case ToolbarIcon::Record: {
        canvas.Ellipse({5, 5, 19, 19}, true);
        break;
    }
    case ToolbarIcon::Stop:
        canvas.Fill({5, 5, 19, 19}, RGB(245, 245, 245));
        break;
    case ToolbarIcon::Pause:
        canvas.Fill({5, 5, 10, 19}, RGB(245, 245, 245));
        canvas.Fill({14, 5, 19, 19}, RGB(245, 245, 245));
        break;
    case ToolbarIcon::Play:
        path({{7, 4}, {20, 12}, {7, 20}, {7, 4}});
        break;
    case ToolbarIcon::TrimStart:
        path({{8, 4}, {4, 4}, {4, 20}, {8, 20}});
        path({{12, 7}, {18, 12}, {12, 17}});
        break;
    case ToolbarIcon::TrimEnd:
        path({{16, 4}, {20, 4}, {20, 20}, {16, 20}});
        path({{12, 7}, {6, 12}, {12, 17}});
        break;
    case ToolbarIcon::Reset:
        path({{5, 10}, {8, 5}, {16, 5}, {20, 10}, {19, 17}, {13, 20}, {7, 17}});
        path({{4, 4}, {4, 11}, {11, 11}});
        break;
    case ToolbarIcon::None:
        break;
    }
}

} // namespace vispick
