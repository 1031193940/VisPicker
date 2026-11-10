#include <doctest/doctest.h>
#include "platform/ToolbarIcon.h"

#include <algorithm>

namespace {
constexpr std::uint32_t Background = 0xFF000000U;

void Clear(vispick::DibSection& image) {
    GdiFlush();
    std::fill(image.Pixels().begin(), image.Pixels().end(), Background);
}
} // namespace

TEST_CASE("Toolbar glyphs draw inside their bounds at each supported UI scale") {
    vispick::DibSection image(96, 96);
    const HDC context = image.DeviceContext();
    const HGDIOBJ originalPen = GetCurrentObject(context, OBJ_PEN);
    const HGDIOBJ originalBrush = GetCurrentObject(context, OBJ_BRUSH);
    const COLORREF originalBrushColor = GetDCBrushColor(context);
    for (const int dpi : {96, 120, 144, 168, 192}) {
        const auto pen = vispick::CreateToolbarIconPen(dpi);
        const LONG size = MulDiv(32, dpi, 96);
        const RECT bounds{8, 8, 8 + size, 8 + size};
        for (int value = static_cast<int>(vispick::ToolbarIcon::Rectangle);
             value <= static_cast<int>(vispick::ToolbarIcon::Reset); ++value) {
            Clear(image);
            const auto icon = static_cast<vispick::ToolbarIcon>(value);
            vispick::DrawToolbarIcon(context, bounds, icon, dpi, pen.Get(), 2);
            GdiFlush();
            bool hasGlyph = false;
            bool hasBlendedEdge = false;
            bool outsideBounds = false;
            for (int row = 0; row < image.Height(); ++row) {
                for (int column = 0; column < image.Width(); ++column) {
                    const auto color =
                        image.Pixels()[static_cast<std::size_t>(row) * image.Width() + column] & 0xFFFFFFU;
                    if (color == 0) { continue; }
                    hasGlyph = true;
                    hasBlendedEdge = hasBlendedEdge || color != 0xF5F5F5U;
                    outsideBounds = outsideBounds || column < bounds.left || column >= bounds.right ||
                                    row < bounds.top || row >= bounds.bottom;
                }
            }
            CAPTURE(dpi);
            CAPTURE(value);
            CHECK(hasGlyph);
            CHECK_FALSE(outsideBounds);
            if (icon == vispick::ToolbarIcon::Ellipse || icon == vispick::ToolbarIcon::Arrow ||
                icon == vispick::ToolbarIcon::Check) {
                CHECK(hasBlendedEdge);
            }
            CHECK(GetCurrentObject(context, OBJ_PEN) == originalPen);
            CHECK(GetCurrentObject(context, OBJ_BRUSH) == originalBrush);
            CHECK(GetDCBrushColor(context) == originalBrushColor);
        }
    }
}

TEST_CASE("Toolbar GDI fallback preserves a geometric pen and restores the device context") {
    const LOGBRUSH brush{BS_SOLID, RGB(220, 40, 20), 0};
    vispick::UniqueGdiObject<HPEN> pen(ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT, 3, &brush, 0, nullptr));
    REQUIRE(pen.Get() != nullptr);
    REQUIRE(GetObjectW(pen.Get(), 0, nullptr) != static_cast<int>(sizeof(LOGPEN)));
    vispick::DibSection image(64, 64);
    Clear(image);
    const HGDIOBJ originalPen = GetCurrentObject(image.DeviceContext(), OBJ_PEN);
    vispick::DrawToolbarIcon(image.DeviceContext(), {4, 4, 60, 60}, vispick::ToolbarIcon::Check, 192, pen.Get());
    GdiFlush();
    CHECK(std::any_of(image.Pixels().begin(), image.Pixels().end(),
                      [](const auto pixel) { return (pixel & 0xFFFFFFU) == 0xDC2814U; }));
    CHECK(GetCurrentObject(image.DeviceContext(), OBJ_PEN) == originalPen);
}

TEST_CASE("Repeated toolbar drawing releases per-draw resources") {
    vispick::DibSection image(64, 64);
    const auto pen = vispick::CreateToolbarIconPen(192);
    for (int value = 1; value <= static_cast<int>(vispick::ToolbarIcon::Reset); ++value) {
        vispick::DrawToolbarIcon(image.DeviceContext(), {0, 0, 64, 64}, static_cast<vispick::ToolbarIcon>(value), 192,
                                 pen.Get(), 2);
    }
    GdiFlush();
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 500; ++iteration) {
        vispick::DrawToolbarIcon(image.DeviceContext(), {0, 0, 64, 64}, vispick::ToolbarIcon::Reset, 192, pen.Get());
    }
    GdiFlush();
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= before + 1);
    CHECK_NOTHROW(vispick::DrawToolbarIcon(nullptr, {}, vispick::ToolbarIcon::Check, 96, nullptr));
    CHECK_NOTHROW(vispick::DrawToolbarIcon(image.DeviceContext(), {}, vispick::ToolbarIcon::Check, 96, pen.Get()));
}
