#include <doctest/doctest.h>
#include "pin/PinGeometry.h"
#include "pin/PinSource.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

TEST_CASE("Pin 1:1 dimensions remain physical across orientation and UI DPI") {
    for (const UINT dpi : {96U, 120U, 144U, 168U, 192U}) {
        (void)dpi; // DPI deliberately does not participate in image geometry.
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
            const SIZE oriented = vispick::PinImageSize({1920, 1080}, rotation);
            const SIZE size = vispick::ScaledPinSize(oriented, 1);
            CHECK(size.cx == ((rotation % 2) != 0 ? 1080 : 1920));
            CHECK(size.cy == ((rotation % 2) != 0 ? 1920 : 1080));
        }
    }
}

TEST_CASE("Pin scale clamps ten to eight hundred percent and obeys rendering pixel budget") {
    CHECK(vispick::LimitPinScale({100, 100}, 0.01, 1000000) == doctest::Approx(0.1));
    CHECK(vispick::LimitPinScale({100, 100}, 20, 1000000) == doctest::Approx(8));
    for (const std::uint64_t budget : {30000ULL, 999999ULL, 8000000ULL, 16588800ULL}) {
        const double scale = vispick::LimitPinScale({1920, 1080}, 8, budget);
        const SIZE size = vispick::ScaledPinSize({1920, 1080}, scale);
        CHECK(static_cast<std::uint64_t>(size.cx) * size.cy <= budget);
        CHECK(scale >= 0.1);
        CHECK(scale <= 8);
    }
}

TEST_CASE("Pin scale validates malformed dimensions floating point and impossible minimum") {
    CHECK_THROWS_AS(static_cast<void>(vispick::LimitPinScale({0, 100}, 1, 10000)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::LimitPinScale({100, 100}, std::numeric_limits<double>::quiet_NaN(), 10000)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::LimitPinScale({100, 100}, 1, 0)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::LimitPinScale({10000, 10000}, 1, 100)), std::length_error);
    CHECK_THROWS_AS(static_cast<void>(vispick::ScaledPinSize({10000, 10000}, 4)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::ScaledPinSize({100, 100}, -1)), std::invalid_argument);
}

TEST_CASE("Pin cursor anchored zoom keeps the selected image point within half a pixel") {
    constexpr POINT origin{-1500, -700};
    constexpr POINT anchor{-1234, -456};
    for (const double previous : {0.1, 0.5, 1.0, 1.75, 4.0, 8.0}) {
        for (const double next : {0.1, 0.51, 1.1, 2.0, 8.0}) {
            const POINT moved = vispick::ZoomPinOrigin(origin, anchor, previous, next);
            const double renderedX = moved.x + (anchor.x - origin.x) * next / previous;
            const double renderedY = moved.y + (anchor.y - origin.y) * next / previous;
            CHECK(std::abs(renderedX - anchor.x) <= 0.500001);
            CHECK(std::abs(renderedY - anchor.y) <= 0.500001);
        }
    }
}

TEST_CASE("Pin zoom rejects zero nonfinite and overflowing coordinate calculations") {
    CHECK_THROWS_AS(static_cast<void>(vispick::ZoomPinOrigin({}, {}, 0, 1)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::ZoomPinOrigin({}, {}, 1, std::numeric_limits<double>::infinity())), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::ZoomPinOrigin({-1000000000, 0}, {1000000000, 0}, 0.1, 8)), std::overflow_error);
}

TEST_CASE("Pin compact and restore preserve double click anchor after arbitrary dragging") {
    constexpr POINT origin{-1400, -500};
    constexpr POINT clicked{213, 137};
    for (const UINT dpi : {96U, 120U, 144U, 168U, 192U}) {
        const int side = MulDiv(50, static_cast<int>(dpi), 96);
        if (dpi == 168) { CHECK(side == 88); }
        const RECT compact = vispick::CompactPinBounds(origin, clicked, side);
        CHECK(compact.right - compact.left == side);
        CHECK(compact.bottom - compact.top == side);
        CHECK(compact.left + side / 2 == origin.x + clicked.x);
        CHECK(compact.top + side / 2 == origin.y + clicked.y);
        const POINT dragged{compact.left + 50, compact.top - 97};
        const POINT expanded = vispick::RestorePinOrigin(dragged, clicked, side);
        CHECK(expanded.x == origin.x + 50);
        CHECK(expanded.y == origin.y - 97);
        CHECK(expanded.x + clicked.x == dragged.x + side / 2);
        CHECK(expanded.y + clicked.y == dragged.y + side / 2);
    }
}

TEST_CASE("Pin snap chooses nearest eligible edge and leaves distant axes unchanged") {
    constexpr std::array<RECT, 2> targets{{{-1920, -1080, 0, 0}, {300, 100, 500, 300}}};
    const POINT monitorSnap = vispick::SnapPinOrigin({-101, -104, -1, -4}, targets, 5);
    CHECK(monitorSnap.x == -100);
    CHECK(monitorSnap.y == -100);
    const POINT peerSnap = vispick::SnapPinOrigin({195, 120, 295, 220}, targets, 10);
    CHECK(peerSnap.x == 200);
    CHECK(peerSnap.y == 120);
    const POINT disabled = vispick::SnapPinOrigin({195, 120, 295, 220}, targets, 0);
    CHECK(disabled.x == 195);
    const POINT distant = vispick::SnapPinOrigin({195, 1000, 295, 1100}, targets, 10);
    CHECK(distant.x == 195);
    CHECK(distant.y == 1000);
}

TEST_CASE("Pin display changes keep a usable portion in the nearest monitor") {
    constexpr std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {0, 0, 2560, 1440}}};
    const POINT partial = vispick::KeepPinVisible({-1980, 50, -1880, 150}, monitors);
    CHECK(partial.x == -1980);
    CHECK(partial.y == 50);
    const POINT sliver = vispick::KeepPinVisible({-2019, 50, -1919, 150}, monitors);
    CHECK(sliver.x == -1920);
    CHECK(sliver.y == 50);
    const POINT missing = vispick::KeepPinVisible({3000, 50, 3200, 250}, monitors);
    CHECK(missing.x == 2360);
    CHECK(missing.y == 50);
    const POINT above = vispick::KeepPinVisible({-1000, -500, -800, -300}, monitors);
    CHECK(above.x == -1000);
    CHECK(above.y == 0);
    const POINT oversized = vispick::KeepPinVisible({4000, 3000, 8000, 6000}, monitors);
    CHECK(oversized.x == 0);
    CHECK(oversized.y == 0);
}

TEST_CASE("Pin rotate and flip operate at original resolution and preserve alpha") {
    vispick::Bitmap image(2, 3);
    for (std::uint32_t index = 0; index < 6; ++index) { image.Pixels()[index] = ((index * 40) << 24U) | (index + 1); }
    const std::array<std::array<unsigned, 6>, 4> orders{{{{0, 1, 2, 3, 4, 5}}, {{4, 2, 0, 5, 3, 1}},
        {{5, 4, 3, 2, 1, 0}}, {{1, 3, 5, 0, 2, 4}}}};
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        const auto output = vispick::TransformPin(image.View(), rotation, false, false);
        CHECK(output.Width() == ((rotation % 2) != 0 ? 3 : 2));
        CHECK(output.Height() == ((rotation % 2) != 0 ? 2 : 3));
        for (unsigned index = 0; index < 6; ++index) { CHECK(output.Pixels()[index] == image.Pixels()[orders[rotation][index]]); }
    }
    const auto horizontal = vispick::TransformPin(image.View(), 1, true, false);
    const std::array<unsigned, 6> horizontalOrder{0, 2, 4, 1, 3, 5};
    for (unsigned index = 0; index < 6; ++index) { CHECK(horizontal.Pixels()[index] == image.Pixels()[horizontalOrder[index]]); }
    const auto both = vispick::TransformPin(image.View(), 0, true, true);
    for (unsigned index = 0; index < 6; ++index) { CHECK(both.Pixels()[index] == image.Pixels()[5 - index]); }
}

TEST_CASE("Pin premultiplication preserves opaque RGB and normalizes zero alpha") {
    CHECK(vispick::PremultiplyPinPixel(0x00FFFFFFU) == 0);
    CHECK(vispick::PremultiplyPinPixel(0xFF123456U) == 0xFF123456U);
    CHECK(vispick::PremultiplyPinPixel(0x80804020U) == 0x80402010U);
    for (unsigned alpha = 0; alpha <= 255; ++alpha) {
        const auto premultiplied = vispick::PremultiplyPinPixel((alpha << 24U) | 0xFF7F01U);
        CHECK((premultiplied >> 24U) == alpha);
        CHECK(((premultiplied >> 16U) & 255U) == alpha);
        CHECK(((premultiplied >> 8U) & 255U) <= alpha);
        CHECK((premultiplied & 255U) <= alpha);
    }
}

TEST_CASE("Opaque pin sources store BGR24 with exact odd-width pixel roundtrip") {
    vispick::Bitmap image(17, 13);
    std::vector<std::uint32_t> expected(image.Pixels().size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = 0xFF000000U | (static_cast<std::uint32_t>(index * 7919) & 0xFFFFFFU);
        image.Pixels()[index] = expected[index];
    }
    const vispick::PinSource source(std::move(image));
    CHECK(source.PackedOpaque());
    CHECK(source.NativeBgra().Empty());
    CHECK(source.LogicalBytes() == expected.size() * 4);
    CHECK(source.StorageBytes() == expected.size() * 3);
    const auto decoded = source.Decode();
    REQUIRE(decoded.Width() == 17);
    REQUIRE(decoded.Height() == 13);
    for (int row = 0; row < 13; ++row) {
        for (int column = 0; column < 17; ++column) {
            const auto expectedPixel = expected[static_cast<std::size_t>(row) * 17 + column];
            CHECK(source.Pixel(column, row) == expectedPixel);
            CHECK(decoded.Row(row)[column] == expectedPixel);
        }
    }
}

TEST_CASE("Pin source retains full straight BGRA when even one pixel has alpha") {
    vispick::Bitmap image(3, 2);
    constexpr std::array<std::uint32_t, 6> expected{0xFFFFFFFFU, 0xFF123456U, 0x00123456U, 0x807F3F01U, 0x01112233U, 0xFF000000U};
    std::copy(expected.begin(), expected.end(), image.Pixels().begin());
    const vispick::PinSource source(std::move(image));
    CHECK_FALSE(source.PackedOpaque());
    CHECK(source.StorageBytes() == expected.size() * 4);
    CHECK(source.LogicalBytes() == expected.size() * 4);
    const auto native = source.NativeBgra();
    REQUIRE(native.Pixels().size() == expected.size());
    const auto decoded = source.Decode();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(native.Pixels()[index] == expected[index]);
        CHECK(decoded.Pixels()[index] == expected[index]);
    }
}

TEST_CASE("Packed pin sources move into recycle ownership without expanding pixels") {
    vispick::Bitmap image(5, 7);
    std::fill(image.Pixels().begin(), image.Pixels().end(), 0xFF3080E0U);
    vispick::PinSource original(std::move(image));
    const auto bytes = original.StorageBytes();
    const vispick::PinSource recycled(std::move(original));
    CHECK(original.Empty());
    CHECK(recycled.PackedOpaque());
    CHECK(recycled.StorageBytes() == bytes);
    const auto decoded = recycled.Decode();
    const auto rotated = vispick::TransformPin(decoded.View(), 1, false, false);
    CHECK(rotated.Width() == 7);
    CHECK(rotated.Height() == 5);
    for (const auto pixel : rotated.Pixels()) { CHECK(pixel == 0xFF3080E0U); }
}
