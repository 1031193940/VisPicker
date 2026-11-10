#include <doctest/doctest.h>
#include "imaging/ImageCodec.h"
#include "imaging/Clipboard.h"
#include "platform/FileCommit.h"
#include <array>
#include <cstring>
#include <fstream>

namespace {
vispick::Bitmap Pattern() {
    vispick::Bitmap image(19, 13);
    for (int row = 0; row < image.Height(); ++row) {
        for (int column = 0; column < image.Width(); ++column) {
            image.Row(row)[column] = 0xFF000000U | (static_cast<unsigned>(row * 17) << 16) |
                                     (static_cast<unsigned>(column * 11) << 8) | static_cast<unsigned>(row + column);
        }
    }
    return image;
}
template <typename T> void Put(std::vector<std::uint8_t>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::vector<std::uint8_t> Dib(int width, int height, WORD bits, std::size_t payload) {
    BITMAPINFOHEADER header{};
    header.biSize = 40;
    header.biWidth = width;
    header.biHeight = height;
    header.biPlanes = 1;
    header.biBitCount = bits;
    std::vector<std::uint8_t> bytes(40 + payload);
    Put(bytes, 0, header);
    return bytes;
}
} // namespace

TEST_CASE("WIC PNG preserves every straight BGRA byte including alpha") {
    auto original = Pattern();
    original.Row(0)[0] = 0x00123456;
    original.Row(1)[1] = 0x80123456;
    const auto bytes = vispick::EncodeImage(original.View(), vispick::SaveFormat::Png);
    const auto decoded = vispick::DecodeImage(bytes);
    CHECK(decoded.Width() == original.Width());
    CHECK(decoded.Height() == original.Height());
    CHECK(std::equal(decoded.Pixels().begin(), decoded.Pixels().end(), original.Pixels().begin()));
}

TEST_CASE("WIC BMP preserves opaque pixels on the Win7 baseline pixel format") {
    const auto original = Pattern();
    const auto decoded = vispick::DecodeImage(vispick::EncodeImage(original.View(), vispick::SaveFormat::Bmp));
    CHECK(std::equal(decoded.Pixels().begin(), decoded.Pixels().end(), original.Pixels().begin()));
}

TEST_CASE("WIC JPEG is readable and honors dimensions and quality bounds") {
    const auto original = Pattern();
    const auto decoded = vispick::DecodeImage(vispick::EncodeImage(original.View(), vispick::SaveFormat::Jpg, 92));
    CHECK(decoded.Width() == 19);
    CHECK(decoded.Height() == 13);
    CHECK_THROWS(static_cast<void>(vispick::EncodeImage(original.View(), vispick::SaveFormat::Jpg, 0)));
    CHECK_THROWS(static_cast<void>(vispick::EncodeImage(original.View(), vispick::SaveFormat::Jpg, 101)));
    CHECK_THROWS(static_cast<void>(vispick::DecodeImage(std::array<std::uint8_t, 5>{1, 2, 3, 4, 5})));
    CHECK_THROWS(static_cast<void>(vispick::EncodeImage({}, vispick::SaveFormat::Png)));
}

TEST_CASE("WIC scale keeps physical target dimensions and transparent edges") {
    vispick::Bitmap original(2, 1);
    original.Row(0)[0] = 0xFFFF0000;
    original.Row(0)[1] = 0x000000FF;
    const auto nearest = vispick::ScaleImage(original.View(), 4, 2, vispick::ImageInterpolation::Nearest);
    CHECK(nearest.Width() == 4);
    CHECK(nearest.Height() == 2);
    CHECK(nearest.Row(0)[0] == 0xFFFF0000);
    CHECK((nearest.Row(0)[3] >> 24) == 0);
    const auto reduced = vispick::ScaleImage(original.View(), 1, 1, vispick::ImageInterpolation::Fant);
    CHECK((reduced.Row(0)[0] & 255) == 0); // transparent blue must not bleed
    CHECK(((reduced.Row(0)[0] >> 16) & 255) >= 250);
    CHECK_THROWS(static_cast<void>(vispick::ScaleImage(original.View(), 0, 1, vispick::ImageInterpolation::Nearest)));
    CHECK_THROWS(
        static_cast<void>(vispick::ScaleImage(original.View(), 100000, 100000, vispick::ImageInterpolation::Nearest)));
}

TEST_CASE("Atomic image save replaces only after successful encoding") {
    const auto directory =
        std::filesystem::temp_directory_path() / (L"VisPick-codec-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto path = directory / L"图像.png";
    {
        std::ofstream sentinel(path, std::ios::binary);
        sentinel << "original";
    }
    CHECK_THROWS(static_cast<void>(vispick::SaveImage(path, {}, vispick::SaveFormat::Png)));
    {
        std::ifstream original(path, std::ios::binary);
        std::string text;
        original >> text;
        CHECK(text == "original");
    }
    const auto pattern = Pattern();
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK_THROWS(vispick::SaveImage(path, pattern.View(), vispick::SaveFormat::Png));
    CHECK((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
    {
        std::ifstream original(path, std::ios::binary);
        std::string text;
        original >> text;
        CHECK(text == "original");
    }
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN));
    const auto streamPath = path.native() + L":VisPickTest";
    bool namedStreamsAvailable = false;
    {
        std::ofstream metadata(std::filesystem::path(streamPath), std::ios::binary);
        if (metadata) {
            metadata << "metadata";
            namedStreamsAvailable = static_cast<bool>(metadata);
        }
    }
    vispick::SaveImage(path, pattern.View(), vispick::SaveFormat::Png);
    CHECK(vispick::ReadImage(path).Pixels()[17] == pattern.Pixels()[17]);
    CHECK((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
    if (namedStreamsAvailable) {
        std::ifstream metadata(std::filesystem::path(streamPath), std::ios::binary);
        std::string value;
        metadata >> value;
        CHECK(value == "metadata");
    }
    CHECK(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1);
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

TEST_CASE("Image save and overwrite support long Unicode paths and full-length filenames") {
    const auto directory =
        std::filesystem::temp_directory_path() / (L"VisPick-codec-long-" + std::to_wstring(GetCurrentProcessId()));
    REQUIRE(std::filesystem::create_directory(directory));
    const auto path = directory / (std::wstring(250, L'图') + L".png");
    REQUIRE(path.native().size() > MAX_PATH);
    auto pattern = Pattern();
    vispick::SaveImage(path, pattern.View(), vispick::SaveFormat::Png);
    CHECK(vispick::ReadImage(path).Pixels()[0] == pattern.Pixels()[0]);
    pattern.Pixels()[0] = 0x80112233;
    vispick::SaveImage(path, pattern.View(), vispick::SaveFormat::Png);
    CHECK(vispick::ReadImage(path).Pixels()[0] == 0x80112233);
    CHECK(std::filesystem::remove(vispick::ExtendedFilePath(path)));
    CHECK(std::filesystem::remove(directory));
}

TEST_CASE("Clipboard DIB preserves RGB and ignores reserved alpha") {
    auto original = Pattern();
    const auto encoded = vispick::EncodeDib(original.View());
    const auto decoded = vispick::DecodeDib(encoded);
    CHECK(std::equal(decoded.Pixels().begin(), decoded.Pixels().end(), original.Pixels().begin()));
    auto reserved = encoded;
    reserved[43] = 0;
    CHECK((vispick::DecodeDib(reserved).Pixels()[0] >> 24) == 255);
    original.Row(0)[0] = 0x00000000;
    CHECK(vispick::DecodeDib(vispick::EncodeDib(original.View())).Pixels()[0] == 0xFFFFFFFF);
}

TEST_CASE("Clipboard DIB supports row padding bottom-up and indexed colors") {
    auto bytes = Dib(1, 2, 24, 8);
    bytes[40] = 255;
    bytes[46] = 255;
    const auto image = vispick::DecodeDib(bytes);
    CHECK(image.Row(0)[0] == 0xFFFF0000);
    CHECK(image.Row(1)[0] == 0xFF0000FF);
    auto indexed = Dib(2, -1, 1, 12);
    Put(indexed, 40, DWORD{0x00ABCDEF});
    Put(indexed, 44, DWORD{0x00112233});
    indexed[48] = 0x40;
    const auto palette = vispick::DecodeDib(indexed);
    CHECK(palette.Row(0)[0] == 0xFFABCDEF);
    CHECK(palette.Row(0)[1] == 0xFF112233);
}

TEST_CASE("Clipboard DIB honors V5 explicit alpha and validates bit masks") {
    auto bytes = Dib(1, -1, 32, 88);
    Put(bytes, 0, DWORD{124});
    Put(bytes, 16, DWORD{BI_BITFIELDS});
    Put(bytes, 40, DWORD{0xFF0000});
    Put(bytes, 44, DWORD{0xFF00});
    Put(bytes, 48, DWORD{0xFF});
    Put(bytes, 52, DWORD{0xFF000000});
    Put(bytes, 124, DWORD{0x80123456});
    CHECK(vispick::DecodeDib(bytes).Pixels()[0] == 0x80123456);
    Put(bytes, 124, DWORD{0x00123456});
    CHECK(vispick::DecodeDib(bytes).Pixels()[0] == 0x00123456);
    Put(bytes, 44, DWORD{0xFF});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
    Put(bytes, 44, DWORD{0xAF00});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
}

TEST_CASE("Clipboard V5 BI_RGB preserves producer alpha without making reserved zero bytes transparent") {
    auto bytes = Dib(2, -1, 32, 92);
    Put(bytes, 0, DWORD{124});
    Put(bytes, 124, DWORD{0x00123456});
    Put(bytes, 128, DWORD{0x00654321});
    CHECK(vispick::DecodeDib(bytes).Pixels()[0] == 0xFF123456);
    Put(bytes, 128, DWORD{0x80654321});
    const auto decoded = vispick::DecodeDib(bytes);
    CHECK(decoded.Pixels()[0] == 0x00123456);
    CHECK(decoded.Pixels()[1] == 0x80654321);
}

TEST_CASE("Clipboard DIB rejects truncated overlarge and inconsistent buffers") {
    const auto pattern = Pattern();
    auto bytes = vispick::EncodeDib(pattern.View());
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_THROWS(static_cast<void>(vispick::DecodeDib(std::span<const std::uint8_t>(bytes).first(length))));
    }
    Put(bytes, 4, LONG{2147483647});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
    Put(bytes, 4, LONG{19});
    Put(bytes, 8, LONG{LONG_MIN});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
    Put(bytes, 8, LONG{-13});
    Put(bytes, 12, WORD{2});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
    Put(bytes, 12, WORD{1});
    Put(bytes, 16, DWORD{BI_RLE8});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
}

TEST_CASE("Packed clipboard V5 profiles follow pixels and cannot overlap or escape the buffer") {
    auto bytes = Dib(1, -1, 24, 100);
    Put(bytes, 0, DWORD{124});
    Put(bytes, 56, DWORD{PROFILE_EMBEDDED});
    Put(bytes, 112, DWORD{128});
    Put(bytes, 116, DWORD{12});
    bytes[124] = 0x56;
    bytes[125] = 0x34;
    bytes[126] = 0x12;
    CHECK(vispick::DecodeDib(bytes).Pixels()[0] == 0xFF123456);
    Put(bytes, 112, DWORD{124});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
    Put(bytes, 112, DWORD{128});
    Put(bytes, 116, DWORD{13});
    CHECK_THROWS(static_cast<void>(vispick::DecodeDib(bytes)));
}
