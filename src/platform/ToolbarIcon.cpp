#include "platform/ToolbarIcon.h"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>

namespace vispick {
namespace {

void Fill(HDC context, const RECT& bounds, COLORREF color) {
    SetDCBrushColor(context, color);
    FillRect(context, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

} // namespace

UniqueGdiObject<HPEN> CreateToolbarIconPen(int dpi) {
    UniqueGdiObject<HPEN> pen(CreatePen(PS_SOLID, std::max(1, MulDiv(3, dpi, 192)), RGB(245, 245, 245)));
    if (!pen.Get()) { throw std::runtime_error("Create toolbar icon pen failed"); }
    return pen;
}

void DrawToolbarIcon(HDC context, RECT bounds, ToolbarIcon icon, int dpi, HPEN strokePen, int detail) {
    const auto scale = [dpi](int logical) { return MulDiv(logical, dpi, 96); };
    // Draw each glyph on the same centered 24-unit grid, including in the wider confirm button.
    const LONG size = std::min({static_cast<LONG>(scale(22)), bounds.right - bounds.left - scale(4), bounds.bottom - bounds.top - scale(4)});
    if (size <= 0) { return; }
    const LONG left = (bounds.left + bounds.right - size) / 2;
    const LONG top = (bounds.top + bounds.bottom - size) / 2;
    const auto x = [&](LONG unit) { return left + MulDiv(unit, size, 24); };
    const auto y = [&](LONG unit) { return top + MulDiv(unit, size, 24); };
    const auto path = [&](std::initializer_list<POINT> points) {
        auto point = points.begin();
        MoveToEx(context, x(point->x), y(point->y), nullptr);
        for (++point; point != points.end(); ++point) { LineTo(context, x(point->x), y(point->y)); }
    };
    const SelectObjectGuard pen(context, strokePen);
    const SelectObjectGuard brush(context, GetStockObject(NULL_BRUSH));
    switch (icon) {
    case ToolbarIcon::Rectangle:
        Rectangle(context, x(3), y(6), x(21), y(18));
        break;
    case ToolbarIcon::Ellipse:
        Ellipse(context, x(3), y(6), x(21), y(18));
        break;
    case ToolbarIcon::Arrow:
        path({{5,19},{19,5},{19,12}});
        path({{12,5},{19,5}});
        break;
    case ToolbarIcon::Pencil:
        path({{4,20},{5,15},{15,5},{19,9},{9,19},{4,20}});
        path({{12,8},{16,12}});
        break;
    case ToolbarIcon::Text:
        path({{5,6},{19,6}});
        path({{12,6},{12,20}});
        break;
    case ToolbarIcon::Mosaic:
        Fill(context, {x(4),y(4),x(11),y(11)}, RGB(245, 245, 245));
        Fill(context, {x(13),y(4),x(20),y(11)}, RGB(132, 138, 148));
        Fill(context, {x(4),y(13),x(11),y(20)}, RGB(132, 138, 148));
        Fill(context, {x(13),y(13),x(20),y(20)}, RGB(245, 245, 245));
        break;
    case ToolbarIcon::Save:
        path({{4,4},{16,4},{20,8},{20,20},{4,20},{4,4}});
        path({{8,4},{8,10},{15,10},{15,4}});
        path({{8,20},{8,14},{16,14},{16,20}});
        break;
    case ToolbarIcon::Copy:
        Rectangle(context, x(8), y(8), x(21), y(21));
        path({{5,16},{4,16},{4,4},{16,4},{16,5}});
        break;
    case ToolbarIcon::Pin:
        path({{15,4},{21,10},{17,11},{13,15},{12,19},{5,12},{9,11},{13,7},{15,4}});
        path({{9,16},{4,21}});
        break;
    case ToolbarIcon::Cancel:
        path({{6,6},{18,18}});
        path({{18,6},{6,18}});
        break;
    case ToolbarIcon::Check:
        path({{4,12},{9,17},{20,6}});
        break;
    case ToolbarIcon::Stroke: {
        const LONG height = scale(detail);
        Fill(context, {x(4), y(12)-height/2, x(20), y(12)+(height+1)/2}, RGB(245, 245, 245));
        break;
    }
    case ToolbarIcon::Font: {
        const LONG inset = 3 - detail;
        const LONG apex = 4 + inset;
        const LONG baseline = 20 - inset;
        const LONG halfWidth = 8 - inset;
        const LONG crossbar = MulDiv(halfWidth, 14 - apex, baseline - apex);
        path({{12-halfWidth,baseline},{12,apex},{12+halfWidth,baseline}});
        path({{12-crossbar,14},{12+crossbar,14}});
        break;
    }
    case ToolbarIcon::Record: {
        const SelectObjectGuard dot(context, GetStockObject(DC_BRUSH));
        SetDCBrushColor(context, RGB(245, 245, 245));
        Ellipse(context, x(5), y(5), x(19), y(19));
        break;
    }
    case ToolbarIcon::Stop:
        Fill(context, {x(5),y(5),x(19),y(19)}, RGB(245, 245, 245));
        break;
    case ToolbarIcon::None:
        break;
    }
}

} // namespace vispick
