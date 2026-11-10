#pragma once

#include "platform/Gdi.h"

namespace vispick {

enum class ToolbarIcon { None, Rectangle, Ellipse, Arrow, Pencil, Text, Mosaic, Save, Copy, Pin, Cancel, Check, Stroke, Font, Record, Stop };

[[nodiscard]] UniqueGdiObject<HPEN> CreateToolbarIconPen(int dpi);
// Stroke uses a logical pixel width. Font uses a size level from 0 through 2.
void DrawToolbarIcon(HDC context, RECT bounds, ToolbarIcon icon, int dpi, HPEN strokePen, int detail = 0);

} // namespace vispick
