#include <doctest/doctest.h>

#include "capture/ScreenGrab.h"
#include "platform/Compat.h"

#include <climits>
#include <memory>
#include <stdexcept>

TEST_CASE("Capture validates rectangle extents including overflowing negative origins") {
    CHECK_THROWS_AS(vispick::CaptureSession(RECT{0, 0, 0, 1}), std::invalid_argument);
    CHECK_THROWS_AS(vispick::CaptureSession(RECT{0, 0, 1, 0}), std::invalid_argument);
    CHECK_THROWS_AS(vispick::CaptureSession(RECT{2, 0, 1, 1}), std::invalid_argument);
    CHECK_THROWS_AS(vispick::CaptureSession(RECT{LONG_MIN, 0, LONG_MAX, 1}), std::invalid_argument);
}

TEST_CASE("Repeated capture reuses DIB storage without owning Bitmap copies") {
    const RECT desktop = vispick::VirtualDesktopBounds();
    vispick::CaptureSession session({desktop.left, desktop.top, desktop.left + 2, desktop.top + 2});
    const auto* firstStorage = session.Image().Pixels().data();
    session.Capture(false);
    const auto view = session.Image();
    CHECK(view.Width() == 2);
    CHECK(view.Height() == 2);
    CHECK(view.Pixels().data() == firstStorage);
    const auto timings = session.CaptureTimed(false);
    CHECK(timings.bitBltMilliseconds >= 0);
    CHECK(timings.gdiFlushMilliseconds >= 0);
    CHECK(session.Image().Pixels().data() == firstStorage);
}

TEST_CASE("FrozenScreen shares one immutable DIB until the last owner releases") {
    const RECT desktop = vispick::VirtualDesktopBounds();
    const RECT bounds{desktop.left, desktop.top, desktop.left + 2, desktop.top + 2};
    auto owner = vispick::CaptureScreen(bounds);
    std::weak_ptr<const vispick::FrozenScreen> observer = owner;
    const auto* pixels = owner->Image().Pixels().data();
    auto retained = owner;
    owner.reset();
    CHECK_FALSE(observer.expired());
    CHECK(retained->Image().Pixels().data() == pixels);
    CHECK(retained->Bounds().left == bounds.left);
    CHECK(retained->Bounds().top == bounds.top);
    const auto cropped = retained->Image().CropOpaque(0, 0, 1, 1);
    CHECK((cropped.Pixels().front() >> 24U) == 255U);
    CHECK_THROWS_AS(retained->BlitTo(nullptr, RECT{-1, 0, 1, 1}, POINT{}), std::out_of_range);
    retained.reset();
    CHECK(observer.expired());
}
