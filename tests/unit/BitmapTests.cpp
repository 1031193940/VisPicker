#include <doctest/doctest.h>

#include "imaging/Bitmap.h"
#include "platform/Gdi.h"

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <type_traits>

using vispick::Bitmap;

TEST_CASE("BitmapView borrows storage and exposes only const pixels") {
    Bitmap bitmap(3, 2);
    const auto view = bitmap.View();
    static_assert(std::is_same_v<decltype(view.Pixels()), std::span<const std::uint32_t>>);
    CHECK(view.Width() == 3);
    CHECK(view.Height() == 2);
    CHECK(view.StrideBytes() == 12);
    CHECK(view.Pixels().data() == bitmap.Pixels().data());
    bitmap.Row(1)[2] = 0x7F123456U;
    CHECK(view.Row(1)[2] == 0x7F123456U);
    CHECK_THROWS_AS(static_cast<void>(view.Row(-1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(view.Row(2)), std::out_of_range);
}

TEST_CASE("BitmapView validates dimensions without multiplying overflowing sizes") {
    const Bitmap bitmap(2, 2);
    CHECK_THROWS_AS(vispick::BitmapView(1, 2, bitmap.Pixels()), std::invalid_argument);
    CHECK_THROWS_AS(vispick::BitmapView(3, 2, bitmap.Pixels()), std::invalid_argument);
    CHECK_THROWS_AS(vispick::BitmapView(INT_MAX, INT_MAX, bitmap.Pixels()), std::invalid_argument);
    CHECK_THROWS_AS(vispick::BitmapView(-1, 2, bitmap.Pixels()), std::invalid_argument);
    CHECK_THROWS_AS(vispick::BitmapView(0, 2, {}), std::invalid_argument);
    CHECK(vispick::BitmapView(0, 0, {}).Empty());
}

TEST_CASE("BitmapView allocates and normalizes alpha only for an explicit crop") {
    Bitmap source(3, 2);
    source.Row(0)[1] = 0x00123456U;
    source.Row(0)[2] = 0x7F654321U;
    source.Row(1)[1] = 0x01ABCDEFU;
    source.Row(1)[2] = 0xFFAABBCCU;
    const auto view = source.View();
    const Bitmap cropped = view.CropOpaque(1, 0, 2, 2);
    CHECK(cropped.Row(0)[0] == 0xFF123456U);
    CHECK(cropped.Row(0)[1] == 0xFF654321U);
    CHECK(cropped.Row(1)[0] == 0xFFABCDEFU);
    CHECK(cropped.Row(1)[1] == 0xFFAABBCCU);
    CHECK(source.Row(0)[1] == 0x00123456U);
    CHECK_THROWS_AS(static_cast<void>(view.CropOpaque(-1, 0, 1, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(view.CropOpaque(1, 0, INT_MAX, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(view.CropOpaque(0, 0, 1, 0)), std::out_of_range);
}

TEST_CASE("Bitmap rejects invalid dimensions and unaddressable allocations") {
    CHECK_THROWS_AS(Bitmap(0, 1), std::invalid_argument);
    CHECK_THROWS_AS(Bitmap(1, 0), std::invalid_argument);
    CHECK_THROWS_AS(Bitmap(-1, 1), std::invalid_argument);
    CHECK_THROWS_AS(Bitmap(1, -1), std::invalid_argument);
    CHECK_THROWS_AS(Bitmap(INT_MAX, INT_MAX), std::length_error);
}

TEST_CASE("Default Bitmap is empty and has no addressable rows") {
    const Bitmap empty;
    CHECK(empty.Empty());
    CHECK(empty.Width() == 0);
    CHECK(empty.Height() == 0);
    CHECK(empty.Pixels().empty());
    CHECK_THROWS_AS(static_cast<void>(empty.Row(0)), std::out_of_range);
}

TEST_CASE("Bitmap BGRA rows are tightly packed and top-down") {
    Bitmap bitmap(3, 2);
    CHECK(bitmap.StrideBytes() == 12);
    CHECK(bitmap.Pixels().size() == 6);
    bitmap.Row(0)[0] = 0xFF010203U;
    bitmap.Row(1)[2] = 0xFF040506U;
    CHECK(bitmap.Pixels()[0] == 0xFF010203U);
    CHECK(bitmap.Pixels()[5] == 0xFF040506U);
    CHECK(bitmap.Row(1) - bitmap.Row(0) == 3);
    const Bitmap& readOnly = bitmap;
    CHECK(readOnly.Row(1)[2] == 0xFF040506U);
}

TEST_CASE("Bitmap row bounds are enforced on mutable and const access") {
    Bitmap bitmap(3, 2);
    const Bitmap& readOnly = bitmap;
    CHECK_THROWS_AS(static_cast<void>(bitmap.Row(-1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(bitmap.Row(2)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(readOnly.Row(-1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(readOnly.Row(2)), std::out_of_range);
}

TEST_CASE("Cropping preserves orientation and independent storage") {
    Bitmap source(4, 3);
    for (std::size_t pixel = 0; pixel < source.Pixels().size(); ++pixel) {
        source.Pixels()[pixel] = 0xFF000000U | static_cast<std::uint32_t>(pixel);
    }
    Bitmap cropped = source.Crop(1, 1, 2, 2);
    CHECK(cropped.Width() == 2);
    CHECK(cropped.Height() == 2);
    CHECK(cropped.Row(0)[0] == 0xFF000005U);
    CHECK(cropped.Row(0)[1] == 0xFF000006U);
    CHECK(cropped.Row(1)[0] == 0xFF000009U);
    CHECK(cropped.Row(1)[1] == 0xFF00000AU);
    cropped.Row(0)[0] = 0;
    CHECK(source.Row(1)[1] == 0xFF000005U);
}

TEST_CASE("Cropping permits exact edges and rejects wrapped rectangles") {
    Bitmap source(4, 3);
    CHECK(source.Crop(0, 0, 4, 3).Pixels().size() == 12);
    CHECK(source.Crop(3, 2, 1, 1).Pixels().size() == 1);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(-1, 0, 1, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(0, -1, 1, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(0, 0, 0, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(4, 0, 1, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(0, 3, 1, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(INT_MAX, 0, INT_MAX, 1)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(source.Crop(0, INT_MAX, 1, INT_MAX)), std::out_of_range);
}

TEST_CASE("DIB stores a 32-bit top-down buffer usable by GDI") {
    vispick::DibSection surface(3, 2);
    CHECK(surface.Pixels().size() == 6);
    std::fill(surface.Pixels().begin(), surface.Pixels().end(), 0U);
    REQUIRE(SetPixelV(surface.DeviceContext(), 2, 1, RGB(17, 34, 51)) != FALSE);
    REQUIRE(GdiFlush() != FALSE);
    CHECK((surface.Pixels()[5] & 0x00FFFFFFU) == 0x00112233U);
    CHECK(surface.Pixels()[2] == 0);
}

TEST_CASE("DIB creation rejects invalid sizes") {
    CHECK_THROWS_AS(vispick::DibSection(0, 10), std::invalid_argument);
    CHECK_THROWS_AS(vispick::DibSection(10, -1), std::invalid_argument);
    CHECK_THROWS_AS(vispick::DibSection(INT_MAX, INT_MAX), std::length_error);
}

TEST_CASE("Repeated DIB ownership releases GDI objects") {
    { const vispick::DibSection warmup(8, 8); }
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 100; ++iteration) {
        const vispick::DibSection surface(32, 32);
        CHECK(surface.Handle() != nullptr);
    }
    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    CHECK(after == before);
}
