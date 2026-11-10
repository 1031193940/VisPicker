#include <doctest/doctest.h>
#include "editor/Annotation.h"
#include "editor/Renderer.h"
#include "platform/Gdi.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
vispick::Bitmap Pattern(int width = 80, int height = 60) {
    vispick::Bitmap image(width, height);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            image.Row(row)[column] = static_cast<std::uint32_t>((column * 3) << 16 | (row * 3) << 8 | column + row);
        }
    }
    return image;
}
void Draw(vispick::AnnotationModel& model, vispick::AnnotationTool tool, POINT start, POINT end, bool shift = false,
          unsigned dpi = 96) {
    model.SetTool(tool);
    model.PointerDown(start, shift, dpi);
    model.PointerUp(end, shift);
}
bool SamePixels(const vispick::Bitmap& first, const vispick::Bitmap& second) {
    return first.Width() == second.Width() && first.Height() == second.Height() &&
           std::equal(first.Pixels().begin(), first.Pixels().end(), second.Pixels().begin());
}
} // namespace

TEST_CASE("Annotation model borrows frozen pixels without allocating a full copy") {
    const auto image = Pattern();
    const vispick::AnnotationModel model(image.View(), {-100, 20});
    CHECK(model.Frozen().Pixels().data() == image.Pixels().data());
    CHECK(model.DesktopBounds().left == -100);
    CHECK(model.DesktopBounds().right == -20);
}
TEST_CASE("Annotation model rejects empty frozen input") {
    CHECK_THROWS_AS(vispick::AnnotationModel({}, {}), std::invalid_argument);
}
TEST_CASE("Annotation physical size conversion applies DPI only to logical style sizes") {
    CHECK(vispick::AnnotationPixels(4, 96) == 4);
    CHECK(vispick::AnnotationPixels(4, 144) == 6);
    CHECK(vispick::AnnotationPixels(4, 192) == 8);
    CHECK_THROWS_AS(static_cast<void>(vispick::AnnotationPixels(0, 96)), std::invalid_argument);
}
TEST_CASE("Annotation shift creates squares and circles in all drag quadrants") {
    for (const auto tool : {vispick::AnnotationTool::Rectangle, vispick::AnnotationTool::Ellipse}) {
        for (const POINT end : {POINT{20, 5}, POINT{-20, 5}, POINT{20, -5}, POINT{-20, -5}}) {
            const auto constrained = vispick::ConstrainAnnotation(tool, {}, end, true);
            CHECK(std::abs(constrained.x) == 20);
            CHECK(std::abs(constrained.y) == 20);
        }
    }
}
TEST_CASE("Annotation arrow shift snaps to horizontal vertical and 45 degrees") {
    const auto horizontal = vispick::ConstrainAnnotation(vispick::AnnotationTool::Arrow, {}, {20, 3}, true);
    CHECK(horizontal.y == 0);
    const auto diagonal = vispick::ConstrainAnnotation(vispick::AnnotationTool::Arrow, {}, {20, 19}, true);
    CHECK(diagonal.x == diagonal.y);
    const auto vertical = vispick::ConstrainAnnotation(vispick::AnnotationTool::Arrow, {}, {2, -20}, true);
    CHECK(vertical.x == 0);
    CHECK(vertical.y < 0);
}
TEST_CASE("Annotation unconstrained tools preserve pointer coordinates") {
    const auto point = vispick::ConstrainAnnotation(vispick::AnnotationTool::Rectangle, {}, {20, 5}, false);
    CHECK(point.x == 20);
    CHECK(point.y == 5);
    const auto pen = vispick::ConstrainAnnotation(vispick::AnnotationTool::Pen, {}, {20, 5}, true);
    CHECK(pen.x == 20);
    CHECK(pen.y == 5);
}
TEST_CASE("Annotation gestures outside selection do not start") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.SetSelection({10, 10, 30, 30});
    model.SetTool(vispick::AnnotationTool::Rectangle);
    model.PointerDown({5, 5});
    CHECK(model.Preview() == nullptr);
    model.PointerUp({20, 20});
    CHECK_FALSE(model.HasAnnotations());
}
TEST_CASE("Annotation gestures preserve screen coordinates at negative desktop origin") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-100, -50});
    Draw(model, vispick::AnnotationTool::Arrow, {-90, -40}, {-60, -20}, false, 192);
    REQUIRE(model.Committed().size() == 1);
    CHECK(model.Committed()[0].points[0].x == -90);
    CHECK(model.Committed()[0].points[1].y == -20);
    CHECK(model.Committed()[0].strokePixels == 8);
}
TEST_CASE("Annotation shift remains square when dragging past selection boundaries") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.SetSelection({0, 0, 40, 20});
    Draw(model, vispick::AnnotationTool::Rectangle, {5, 5}, {70, 15}, true);
    const auto& points = model.Committed()[0].points;
    CHECK(points[1].x - points[0].x == points[1].y - points[0].y);
    CHECK(points[1].y < 20);
}
TEST_CASE("Annotation changing selection never shifts committed geometry or revision") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-20, -10});
    Draw(model, vispick::AnnotationTool::Ellipse, {-10, 0}, {20, 30});
    const auto revision = model.Revision();
    model.SetSelection({0, 0, 40, 40});
    CHECK(model.Revision() == revision);
    CHECK(model.Committed()[0].points.front().x == -10);
    CHECK(model.Committed()[0].points.back().y == 30);
}
TEST_CASE("Annotation empty clicks do not create shapes while pen clicks create dots") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    Draw(model, vispick::AnnotationTool::Rectangle, {10, 10}, {10, 10});
    CHECK_FALSE(model.HasAnnotations());
    Draw(model, vispick::AnnotationTool::Pen, {10, 10}, {10, 10});
    CHECK(model.Committed().size() == 1);
}
TEST_CASE("Annotation pen records distinct samples and cancels without adding history") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.SetTool(vispick::AnnotationTool::Pen);
    model.PointerDown({10, 10});
    model.PointerMove({10, 10});
    model.PointerMove({20, 20});
    REQUIRE(model.Preview() != nullptr);
    CHECK(model.Preview()->points.size() == 2);
    model.CancelPreview();
    CHECK_FALSE(model.HasAnnotations());
    CHECK(model.Revision() == 0);
}
TEST_CASE("Annotation changing tools cancels the active preview") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.SetTool(vispick::AnnotationTool::Pen);
    model.PointerDown({10, 10});
    model.SetTool(vispick::AnnotationTool::Arrow);
    CHECK(model.Preview() == nullptr);
    CHECK_FALSE(model.HasAnnotations());
}
TEST_CASE("Annotation style is captured when a gesture starts") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.SetTool(vispick::AnnotationTool::Pen);
    model.PointerDown({10, 10});
    auto style = model.Style();
    style.color = 0x0011FF;
    style.strokeWidth = 6;
    model.SetStyle(style);
    model.PointerUp({20, 20});
    CHECK(model.Committed()[0].color == 0xFF3B30);
    CHECK(model.Committed()[0].strokePixels == 4);
}
TEST_CASE("Annotation style rejects unsupported sizes and colors") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    auto style = model.Style();
    style.strokeWidth = 3;
    CHECK_THROWS_AS(model.SetStyle(style), std::invalid_argument);
    style = model.Style();
    style.color = 0xFFFFFFFF;
    CHECK_THROWS_AS(model.SetStyle(style), std::invalid_argument);
}
TEST_CASE("Annotation undo and redo retain more than fifty ordered steps") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    for (int index = 0; index < 70; ++index) { Draw(model, vispick::AnnotationTool::Pen, {index, 10}, {index, 20}); }
    for (int count = 70; count > 0; --count) {
        CHECK(model.Committed().size() == static_cast<std::size_t>(count));
        model.Undo();
    }
    CHECK_FALSE(model.CanUndo());
    CHECK(model.CanRedo());
    for (int count = 0; count < 70; ++count) {
        model.Redo();
        CHECK(model.Committed().back().points.front().x == count);
    }
    CHECK_FALSE(model.CanRedo());
    CHECK(model.Committed().size() == 70);
}
TEST_CASE("Annotation new edit after undo discards redo branch") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    Draw(model, vispick::AnnotationTool::Pen, {10, 10}, {20, 20});
    Draw(model, vispick::AnnotationTool::Pen, {30, 10}, {40, 20});
    model.Undo();
    Draw(model, vispick::AnnotationTool::Arrow, {15, 15}, {25, 25});
    CHECK_FALSE(model.CanRedo());
    CHECK(model.Committed().size() == 2);
    CHECK(model.Committed().back().tool == vispick::AnnotationTool::Arrow);
}
TEST_CASE("Annotation empty undo redo leaves revision unchanged") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.Undo();
    model.Redo();
    CHECK(model.Revision() == 0);
}
TEST_CASE("Annotation mosaic averages frozen RGB and normalizes alpha") {
    vispick::Bitmap image(2, 2);
    image.Pixels()[0] = 0x00000000;
    image.Pixels()[1] = 0x00FF0000;
    image.Pixels()[2] = 0x0000FF00;
    image.Pixels()[3] = 0x000000FF;
    const auto mosaic = vispick::MosaicFrozen(image.View(), {-2, -2}, {-2, -2, 0, 0}, 2);
    for (const auto pixel : mosaic.Pixels()) { CHECK(pixel == 0xFF3F3F3F); }
}
TEST_CASE("Annotation mosaic handles incomplete edge blocks without reading past crop") {
    const auto image = Pattern(5, 3);
    const auto mosaic = vispick::MosaicFrozen(image.View(), {}, {0, 0, 5, 3}, 2);
    CHECK(mosaic.Row(2)[4] == (image.Row(2)[4] | 0xFF000000));
    CHECK(mosaic.Row(0)[0] == mosaic.Row(1)[1]);
}
TEST_CASE("Annotation mosaic validates source bounds and block size") {
    const auto image = Pattern();
    CHECK_THROWS_AS(static_cast<void>(vispick::MosaicFrozen(image.View(), {}, {-1, 0, 5, 5}, 10)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(vispick::MosaicFrozen(image.View(), {}, {0, 0, 5, 5}, 0)), std::invalid_argument);
}
TEST_CASE("Annotation mosaic owns only its committed crop and stays fixed after selection changes") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-20, -10});
    Draw(model, vispick::AnnotationTool::Mosaic, {-10, 0}, {4, 9});
    const auto& annotation = model.Committed()[0];
    REQUIRE(static_cast<bool>(annotation.mosaic));
    CHECK(annotation.mosaic->Width() == 15);
    CHECK(annotation.mosaic->Height() == 10);
    model.SetSelection({0, 0, 30, 30});
    CHECK(annotation.bounds.left == -10);
    CHECK(annotation.bounds.top == 0);
}
TEST_CASE("Annotation renderer no annotations uses frozen data without building full frame cache") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-20, -10});
    vispick::EditorRenderer renderer(model);
    const auto crop = renderer.Flatten({-15, -7, 5, 8});
    CHECK(crop.Width() == 20);
    CHECK(crop.Height() == 15);
    CHECK(crop.Row(0)[0] == (image.Row(3)[5] | 0xFF000000));
    CHECK(renderer.CacheBuildCount() == 0);
}
TEST_CASE("Annotation renderer direct frozen draw maps top down rows and destination origin") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-20, -10});
    vispick::EditorRenderer renderer(model);
    vispick::DibSection destination(80, 60);
    std::fill(destination.Pixels().begin(), destination.Pixels().end(), 0x12345678);
    renderer.Draw(destination.DeviceContext(), {-20, -10}, {-15, -7, 5, 8});
    GdiFlush();
    CHECK((destination.Pixels()[3 * 80 + 5] & 0xFFFFFF) == (image.Row(3)[5] & 0xFFFFFF));
    CHECK((destination.Pixels()[17 * 80 + 24] & 0xFFFFFF) == (image.Row(17)[24] & 0xFFFFFF));
    CHECK(destination.Pixels()[0] == 0x12345678);
}
TEST_CASE("Annotation renderer cache rebuilds only for committed history revisions") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Rectangle, {5, 5}, {25, 25});
    static_cast<void>(renderer.Flatten({0, 0, 40, 40}));
    CHECK(renderer.CacheBuildCount() == 1);
    model.SetTool(vispick::AnnotationTool::Pen);
    model.PointerDown({30, 30});
    model.PointerMove({40, 40});
    static_cast<void>(renderer.Flatten({0, 0, 40, 40}));
    CHECK(renderer.CacheBuildCount() == 1);
    model.PointerUp({45, 45});
    static_cast<void>(renderer.Flatten({0, 0, 40, 40}));
    CHECK(renderer.CacheBuildCount() == 2);
    model.SetSelection({10, 10, 30, 30});
    static_cast<void>(renderer.Flatten(model.Selection()));
    CHECK(renderer.CacheBuildCount() == 2);
}
TEST_CASE("Annotation renderer output is opaque and preserves untouched frozen pixels") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Rectangle, {10, 10}, {30, 30});
    const auto output = renderer.Flatten({0, 0, 80, 60});
    CHECK(output.Row(50)[70] == (image.Row(50)[70] | 0xFF000000));
    CHECK(output.Row(10)[20] != (image.Row(10)[20] | 0xFF000000));
    CHECK(std::all_of(output.Pixels().begin(), output.Pixels().end(),
                      [](std::uint32_t pixel) { return (pixel >> 24) == 255; }));
}
TEST_CASE("Annotation renderer keeps a sparse desktop out of the cache and exports matching tiles") {
    const auto image = Pattern(2304, 2112);
    constexpr POINT origin{-48, -70};
    constexpr RECT exportBounds{-20, -45, 620, 60};
    vispick::AnnotationModel model(image.View(), origin);
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Rectangle, {-16, -30}, {590, 40});
    const auto cached = renderer.Flatten(exportBounds);
    REQUIRE(renderer.CacheBuildCount() == 1);
    Draw(model, vispick::AnnotationTool::Ellipse, {2200, 1980}, {2230, 2010});
    CHECK(SamePixels(renderer.Flatten(exportBounds), cached));
    CHECK(renderer.CacheBuildCount() == 1);
}
TEST_CASE("Annotation renderer crop preserves fixed screen anchors after selection move") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {-20, -10});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Arrow, {-10, 0}, {30, 30});
    const auto full = renderer.Flatten(model.DesktopBounds());
    model.SetSelection({0, 0, 40, 40});
    const auto crop = renderer.Flatten(model.Selection());
    const auto expected = full.Crop(20, 10, 40, 40);
    CHECK(SamePixels(crop, expected));
}
TEST_CASE("Annotation renderer mosaic samples frozen source even above existing annotations") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Pen, {5, 10}, {35, 10});
    const auto painted = renderer.Flatten({0, 0, 40, 30});
    Draw(model, vispick::AnnotationTool::Mosaic, {0, 0}, {39, 29});
    const auto covered = renderer.Flatten({0, 0, 40, 30});
    const auto expected = vispick::MosaicFrozen(image.View(), {}, {0, 0, 40, 30}, 10);
    CHECK(SamePixels(covered, expected));
    model.Undo();
    CHECK(SamePixels(renderer.Flatten({0, 0, 40, 30}), painted));
}
TEST_CASE("Annotation renderer undo all restores exact original RGB") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Ellipse, {10, 10}, {30, 30});
    static_cast<void>(renderer.Flatten(model.DesktopBounds()));
    model.Undo();
    CHECK(SamePixels(renderer.Flatten(model.DesktopBounds()), image.View().CropOpaque(0, 0, 80, 60)));
}
TEST_CASE("Annotation renderer clips selection to desktop and rejects empty output") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    CHECK(renderer.Flatten({-5, -5, 10, 10}).Width() == 10);
    CHECK_THROWS_AS(static_cast<void>(renderer.Flatten({-10, -10, -1, -1})), std::invalid_argument);
}
TEST_CASE("Annotation renderer renders all vector tools and smoothed pen without mutating source") {
    const auto image = Pattern();
    const auto original = image.Pixels()[10 * 80 + 10];
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    Draw(model, vispick::AnnotationTool::Rectangle, {5, 5}, {20, 20});
    Draw(model, vispick::AnnotationTool::Ellipse, {25, 5}, {45, 25});
    Draw(model, vispick::AnnotationTool::Arrow, {5, 40}, {50, 40});
    model.SetTool(vispick::AnnotationTool::Pen);
    model.PointerDown({50, 10});
    model.PointerMove({60, 20});
    model.PointerMove({55, 30});
    model.PointerUp({70, 40});
    CHECK_FALSE(SamePixels(renderer.Flatten(model.DesktopBounds()), image.View().CropOpaque(0, 0, 80, 60)));
    CHECK(image.Pixels()[10 * 80 + 10] == original);
}
TEST_CASE("Annotation text preserves supplied GDI font and native visual line offsets") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    LOGFONTW font{};
    font.lfHeight = -14;
    font.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(font.lfFaceName, L"Arial");
    model.AddText({5, 5, 75, 55}, font, 0xFFFFFF, {{L"Text", {0, 0}}, {L"多行", {0, 20}}});
    REQUIRE(model.Committed().size() == 1);
    CHECK(model.Committed()[0].font.lfHeight == -14);
    CHECK(model.Committed()[0].textLines[1].offset.y == 20);
    const auto output = renderer.Flatten(model.DesktopBounds());
    CHECK_FALSE(SamePixels(output, image.View().CropOpaque(0, 0, 80, 60)));
    CHECK(output.Row(59)[79] == (image.Row(59)[79] | 0xFF000000));
}
TEST_CASE("Annotation empty text does not create an undo step") {
    const auto image = Pattern();
    vispick::AnnotationModel model(image.View(), {});
    model.AddText({0, 0, 20, 20}, {}, 0, {{L"", {}}});
    CHECK_FALSE(model.CanUndo());
}
TEST_CASE("Annotation smoothed pen dirty regions match complete preview redraws") {
    const auto image = Pattern(240, 180);
    vispick::AnnotationModel model(image.View(), {});
    vispick::EditorRenderer renderer(model);
    vispick::DibSection incremental(240, 180);
    vispick::DibSection complete(240, 180);
    renderer.Draw(incremental.DeviceContext(), {}, model.DesktopBounds());
    model.SetTool(vispick::AnnotationTool::Pen);
    const POINT points[]{{20, 20}, {200, 20}, {200, 150}, {30, 140}, {25, 30}, {120, 75}};
    renderer.Draw(incremental.DeviceContext(), {}, model.PointerDown(points[0]));
    for (std::size_t point = 1; point < std::size(points); ++point) {
        const auto dirty = model.PointerMove(points[point]);
        renderer.Draw(incremental.DeviceContext(), {}, dirty);
        renderer.Draw(complete.DeviceContext(), {}, model.DesktopBounds());
        GdiFlush();
        CHECK(std::equal(
            incremental.Pixels().begin(), incremental.Pixels().end(), complete.Pixels().begin(),
            [](std::uint32_t first, std::uint32_t second) { return (first & 0xFFFFFF) == (second & 0xFFFFFF); }));
    }
    CHECK(renderer.CacheBuildCount() == 0);
}
