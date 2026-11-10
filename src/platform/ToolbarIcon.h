#pragma once

#include "platform/Gdi.h"

namespace vispick {

enum class ToolbarIcon {
    None,
    Rectangle,
    Ellipse,
    Arrow,
    Pencil,
    Text,
    Mosaic,
    Save,
    Copy,
    Pin,
    Cancel,
    Check,
    Stroke,
    Font,
    Record,
    Stop,
    Pause,
    Play,
    TrimStart,
    TrimEnd,
    Reset
};

[[nodiscard]] UniqueGdiObject<HPEN> CreateToolbarIconPen(int dpi);
// Stroke uses a logical pixel width. Font uses a size level from 0 through 2.
// Falls back to GDI if antialiased drawing cannot initialize or render.
void DrawToolbarIcon(HDC context, RECT bounds, ToolbarIcon icon, int dpi, HPEN strokePen, int detail = 0) noexcept;

} // namespace vispick
