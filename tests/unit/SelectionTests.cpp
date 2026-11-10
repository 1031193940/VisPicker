#include <doctest/doctest.h>
#include "overlay/Selection.h"

#include <array>
#include <limits>

namespace {
constexpr RECT Desktop{-1920, -1080, 2560, 1440};
bool Same(const RECT first, const RECT second) {
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}
} // namespace

TEST_CASE("Selection normalizes reversed drags across negative screen origins") {
    CHECK(Same(vispick::NormalizeSelection({200, 300}, {-300, -400}, Desktop), {-300, -400, 200, 300}));
    CHECK(Same(vispick::NormalizeSelection({-2500, -1500}, {4000, 3000}, Desktop), Desktop));
    CHECK(Same(vispick::NormalizeSelection({-100, 50}, {-100, 50}, Desktop), {-100, 50, -100, 50}));
}

TEST_CASE("Selection move clamps translation without shrinking the rectangle") {
    constexpr RECT selection{-100, -50, 100, 50};
    CHECK(Same(vispick::MoveSelection(selection, -10000, -10000, Desktop), {-1920, -1080, -1720, -980}));
    CHECK(Same(vispick::MoveSelection(selection, 10000, 10000, Desktop), {2360, 1340, 2560, 1440}));
    CHECK(Same(vispick::MoveSelection(Desktop, 100, -100, Desktop), Desktop));
    CHECK(Same(vispick::MoveSelection(selection, 1, 10, Desktop), {-99, -40, 101, 60}));
}

TEST_CASE("Selection eight handles resize their intended edges") {
    constexpr RECT selected{-100, -100, 100, 100};
    const std::array<RECT, 8> expected{{{-150, -160, 100, 100},
                                        {-100, -160, 100, 100},
                                        {-100, -160, 1, 100},
                                        {-100, -100, 1, 100},
                                        {-100, -100, 1, 1},
                                        {-100, -100, 100, 1},
                                        {-150, -100, 100, 1},
                                        {-150, -100, 100, 100}}};
    // A common point lets each test assert which axes are held fixed. Right and
    // bottom handles use the separate point (1,1) to retain positive extent.
    for (unsigned index = 0; index < 8; ++index) {
        const auto handle =
            static_cast<vispick::SelectionHandle>(static_cast<unsigned>(vispick::SelectionHandle::TopLeft) + index);
        const POINT point{index == 0 || index == 6 || index == 7 ? -150L : 1L, index <= 2 ? -160L : 1L};
        CHECK(Same(vispick::ResizeSelection(selected, handle, point, Desktop), expected[index]));
    }
}

TEST_CASE("Selection resize clamps to desktop and does not invert at opposing edge") {
    constexpr RECT selected{-100, -100, 100, 100};
    CHECK(Same(vispick::ResizeSelection(selected, vispick::SelectionHandle::TopLeft, {-9999, -9999}, Desktop),
               {-1920, -1080, 100, 100}));
    CHECK(Same(vispick::ResizeSelection(selected, vispick::SelectionHandle::BottomRight, {9999, 9999}, Desktop),
               {-100, -100, 2560, 1440}));
    CHECK(Same(vispick::ResizeSelection(selected, vispick::SelectionHandle::TopLeft, {9999, 9999}, Desktop),
               {99, 99, 100, 100}));
    CHECK(Same(vispick::ResizeSelection(selected, vispick::SelectionHandle::BottomRight, {-9999, -9999}, Desktop),
               {-100, -100, -99, -99}));
}

TEST_CASE("Selection handle hit test prioritizes handles over movement and outside") {
    constexpr RECT selected{-100, -100, 100, 100};
    const auto points = vispick::SelectionHandles(selected);
    for (unsigned index = 0; index < points.size(); ++index) {
        CHECK(vispick::HitSelection(selected, points[index], 4) ==
              static_cast<vispick::SelectionHandle>(static_cast<unsigned>(vispick::SelectionHandle::TopLeft) + index));
    }
    CHECK(vispick::HitSelection(selected, {0, 0}, 4) == vispick::SelectionHandle::Move);
    CHECK(vispick::HitSelection(selected, {200, 200}, 4) == vispick::SelectionHandle::None);
    CHECK(vispick::HitSelection({}, {0, 0}, 4) == vispick::SelectionHandle::None);
    CHECK(vispick::HitSelection(selected, {-104, -104}, 4) == vispick::SelectionHandle::TopLeft);
    CHECK(vispick::HitSelection(selected, {-105, -105}, 4) == vispick::SelectionHandle::None);
}

TEST_CASE("Selection click and sub-four-pixel movement snap to provided window") {
    constexpr RECT window{-400, -300, 300, 200};
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Begin({-20, -20}, 4);
    selection.End({-20, -20}, window);
    CHECK(Same(selection.Bounds(), window));
    selection.Clear();
    selection.Begin({-20, -20}, 4);
    selection.End({-17, -17}, window);
    CHECK(Same(selection.Bounds(), window));
    selection.Clear();
    selection.Begin({-20, -20}, 4);
    selection.End({-16, -16}, window);
    CHECK(Same(selection.Bounds(), {-20, -20, -16, -16}));
}

TEST_CASE("Selection keeps deliberately dragged thin nonempty regions") {
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Begin({-20, -20}, 4);
    selection.End({100, -19}, Desktop);
    CHECK(Same(selection.Bounds(), {-20, -20, 100, -19}));
}

TEST_CASE("Selection drag uses gesture origin so intermediate events cannot accumulate drift") {
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Set({-100, -100, 100, 100});
    selection.Begin({0, 0}, 4);
    CHECK(selection.Handle() == vispick::SelectionHandle::Move);
    selection.Drag({15, 20});
    selection.Drag({25, 30});
    selection.End({30, 40}, Desktop);
    CHECK(Same(selection.Bounds(), {-70, -60, 130, 140}));
    CHECK_FALSE(selection.Dragging());
}

TEST_CASE("Selection outside press starts fresh and keyboard movement respects physical pixels") {
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Set({-100, -100, 100, 100});
    selection.Begin({300, 300}, 4);
    CHECK(selection.Creating());
    selection.End({500, 400}, Desktop);
    CHECK(Same(selection.Bounds(), {300, 300, 500, 400}));
    selection.Nudge(-1, 10);
    CHECK(Same(selection.Bounds(), {299, 310, 499, 410}));
    selection.Clear();
    selection.Nudge(10, 10);
    CHECK_FALSE(selection.HasSelection());
}

TEST_CASE("Selection toolbar prefers below then above then inside without crossing monitor") {
    constexpr RECT monitor{-1920, -1080, 0, 0};
    CHECK(Same(vispick::PlaceToolbar({-1000, -600, -500, -300}, {400, 40}, monitor, 6), {-900, -294, -500, -254}));
    CHECK(Same(vispick::PlaceToolbar({-1000, -300, -500, -10}, {400, 40}, monitor, 6), {-900, -346, -500, -306}));
    CHECK(Same(vispick::PlaceToolbar(monitor, {400, 40}, monitor, 6), {-400, -46, 0, -6}));
    const RECT clamped = vispick::PlaceToolbar({-1910, -500, -1900, -400}, {400, 40}, monitor, 6);
    CHECK(clamped.left == monitor.left);
    CHECK(clamped.right - clamped.left == 400);
}

TEST_CASE("Selection geometry is independent of UI scale from 100 to 200 percent") {
    constexpr RECT rectangle{-100, -50, 200, 150};
    for (const unsigned dpi : {96U, 120U, 144U, 168U, 192U}) {
        const int radius = MulDiv(5, static_cast<int>(dpi), 96);
        vispick::Selection selection;
        selection.Reset(Desktop);
        selection.Set(rectangle);
        selection.Begin({50, 50}, radius);
        selection.End({51, 60}, Desktop);
        CHECK(Same(selection.Bounds(), {-99, -40, 201, 160}));
        CHECK(selection.Bounds().right - selection.Bounds().left == 300);
        CHECK(selection.Bounds().bottom - selection.Bounds().top == 200);
    }
}

TEST_CASE("Selection losing capture restores the original rectangle and discards in-drag nudges") {
    constexpr RECT original{-100, -100, 100, 100};
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Set(original);
    selection.Begin({0, 0}, 4);
    selection.Drag({10, 20});
    selection.Nudge(1, 10);
    CHECK(Same(selection.Bounds(), {-90, -80, 110, 120}));
    selection.CancelDrag();
    CHECK_FALSE(selection.Dragging());
    CHECK(Same(selection.Bounds(), original));
}

TEST_CASE("Keyboard resize keeps the origin and clamps right and bottom without overflow") {
    vispick::Selection selection;
    selection.Reset(Desktop);
    selection.Set({-100, -100, 100, 100});
    selection.ResizeBy(-1, 10);
    CHECK(Same(selection.Bounds(), {-100, -100, 99, 110}));
    selection.ResizeBy((std::numeric_limits<LONG>::max)(), (std::numeric_limits<LONG>::max)());
    CHECK(Same(selection.Bounds(), {-100, -100, Desktop.right, Desktop.bottom}));
    selection.ResizeBy((std::numeric_limits<LONG>::min)(), (std::numeric_limits<LONG>::min)());
    CHECK(Same(selection.Bounds(), {-100, -100, -99, -99}));

    selection.Set({-100, -100, 100, 100});
    selection.Begin({0, 0}, 4);
    selection.ResizeBy(10, 10);
    CHECK(Same(selection.Bounds(), {-100, -100, 100, 100}));
    selection.Clear();
    selection.ResizeBy(10, 10);
    CHECK_FALSE(selection.HasSelection());
}

TEST_CASE("Window keyboard cycle skips empty and duplicate regions and wraps in either direction") {
    constexpr RECT front{-100, -100, 100, 100};
    constexpr RECT back{-500, -200, -200, 100};
    constexpr std::array<RECT, 5> candidates{{{}, front, front, {0, 1, 2, 1}, back}};
    const auto first = vispick::CycleWindowSelection(candidates, {}, false);
    const auto last = vispick::CycleWindowSelection(candidates, {}, true);
    REQUIRE(first);
    REQUIRE(last);
    CHECK(Same(*first, front));
    CHECK(Same(*last, back));
    const auto next = vispick::CycleWindowSelection(candidates, front, false);
    const auto previous = vispick::CycleWindowSelection(candidates, front, true);
    const auto wrapped = vispick::CycleWindowSelection(candidates, back, false);
    REQUIRE(next);
    REQUIRE(previous);
    REQUIRE(wrapped);
    CHECK(Same(*next, back));
    CHECK(Same(*previous, back));
    CHECK(Same(*wrapped, front));
    constexpr std::array<RECT, 2> duplicateOnly{front, front};
    const auto unchanged = vispick::CycleWindowSelection(duplicateOnly, front, false);
    REQUIRE(unchanged);
    CHECK(Same(*unchanged, front));
    CHECK_FALSE(vispick::CycleWindowSelection({}, {}, false));
    constexpr std::array<RECT, 2> emptyOnly{{{}, {1, 1, 1, 2}}};
    CHECK_FALSE(vispick::CycleWindowSelection(emptyOnly, {}, true));
}

TEST_CASE("Magnifier scales every sample cell with DPI and stays inside negative-origin monitors") {
    constexpr RECT monitor{-1920, -1080, 0, 0};
    constexpr std::array<POINT, 5> pointers{{{-1920, -1080}, {-1, -1080}, {-1920, -1}, {-1, -1}, {-960, -540}}};
    for (const UINT dpi : {96U, 120U, 144U, 168U, 192U}) {
        const LONG cell = MulDiv(8, static_cast<int>(dpi), 96);
        for (const POINT pointer : pointers) {
            const auto layout = vispick::LayoutMagnifier(pointer, monitor, dpi);
            CHECK(layout.image.right - layout.image.left == cell * 15);
            CHECK(layout.image.bottom - layout.image.top == cell * 15);
            CHECK(layout.center.right - layout.center.left == cell);
            CHECK(layout.center.bottom - layout.center.top == cell);
            CHECK(layout.center.left == layout.image.left + cell * 7);
            CHECK(layout.center.top == layout.image.top + cell * 7);
            CHECK(layout.label.top >= layout.image.bottom);
            CHECK(layout.label.bottom > layout.label.top);
            CHECK(layout.bounds.left >= monitor.left);
            CHECK(layout.bounds.top >= monitor.top);
            CHECK(layout.bounds.right <= monitor.right);
            CHECK(layout.bounds.bottom <= monitor.bottom);
        }
    }
}

TEST_CASE("Sampled color formats preserve RGB channels and ignore alpha") {
    CHECK(vispick::FormatSampledColor(0xAB12CD00U, false) == L"#12CD00");
    CHECK(vispick::FormatSampledColor(0xAB12CD00U, true) == L"RGB(18, 205, 0)");
    CHECK(vispick::FormatSampledColor(0, false) == L"#000000");
    CHECK(vispick::FormatSampledColor(0xFFFFFFFFU, true) == L"RGB(255, 255, 255)");
}
