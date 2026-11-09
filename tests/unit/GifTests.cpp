#include <doctest/doctest.h>

#include "gif/GifWriter.h"
#include "gif/Lzw.h"
#include "gif/Quantizer.h"
#include "gif/Timing.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

// The developer benchmark keeps the reviewed pre-change implementation in a
// separate namespace; use that independent fixture for byte-for-byte regression.
namespace vispick::quantizer_baseline {
gif::IndexedImage Quantize(std::span<const std::uint32_t> pixels);
}

namespace {

// Independent table-of-strings decoder. Intentionally different from the
// encoder's prefix/hash dictionary to catch bit-width and reset mistakes.
std::vector<std::uint8_t> DecodeLzw(std::span<const std::uint8_t> bytes,
                                  int minimumCodeSize) {
    const int clearCode = 1 << minimumCodeSize;
    const int endCode = clearCode + 1;
    std::vector<std::vector<std::uint8_t>> dictionary(4096);
    for (int value = 0; value < clearCode; ++value) {
        dictionary[static_cast<std::size_t>(value)] = {static_cast<std::uint8_t>(value)};
    }
    std::size_t bitOffset = 0;
    int codeSize = minimumCodeSize + 1;
    int nextCode = clearCode + 2;
    std::vector<std::uint8_t> previous;
    std::vector<std::uint8_t> output;
    for (;;) {
        if (bitOffset + static_cast<std::size_t>(codeSize) > bytes.size() * 8) {
            throw std::runtime_error("Truncated LZW stream");
        }
        int code = 0;
        for (int bit = 0; bit < codeSize; ++bit, ++bitOffset) {
            if ((bytes[bitOffset / 8] & (1U << (bitOffset % 8))) != 0) {
                code |= 1 << bit;
            }
        }
        if (code == clearCode) {
            codeSize = minimumCodeSize + 1;
            nextCode = clearCode + 2;
            previous.clear();
            continue;
        }
        if (code == endCode) {
            return output;
        }
        std::vector<std::uint8_t> entry;
        if (code < nextCode) {
            entry = dictionary[static_cast<std::size_t>(code)];
        } else if (code == nextCode && !previous.empty()) {
            entry = previous;
            entry.push_back(previous.front());
        } else {
            throw std::runtime_error("Invalid LZW dictionary code");
        }
        if (entry.empty()) {
            throw std::runtime_error("Empty LZW dictionary entry");
        }
        output.insert(output.end(), entry.begin(), entry.end());
        if (!previous.empty() && nextCode < 4096) {
            auto added = previous;
            added.push_back(entry.front());
            dictionary[static_cast<std::size_t>(nextCode++)] = std::move(added);
            if (nextCode == (1 << codeSize) && codeSize < 12) {
                ++codeSize;
            }
        }
        previous = std::move(entry);
    }
}

class TestFile {
public:
    TestFile() {
        static std::atomic<unsigned int> sequence{0};
        path = std::filesystem::temp_directory_path() /
            (L"VisPick-gif-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(sequence.fetch_add(1)) + L".gif");
    }
    ~TestFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    TestFile(const TestFile&) = delete;
    TestFile& operator=(const TestFile&) = delete;
    std::filesystem::path path;
};

} // namespace

TEST_CASE("LZW empty input emits a valid clear/end stream") {
    CHECK(DecodeLzw(vispick::gif::EncodeLzw({}), 8).empty());
}

TEST_CASE("LZW validates the alphabet") {
    const std::array<std::uint8_t, 1> invalid{4};
    CHECK_THROWS_AS(vispick::gif::EncodeLzw({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(vispick::gif::EncodeLzw({}, 9), std::invalid_argument);
    CHECK_THROWS_AS(vispick::gif::EncodeLzw(invalid, 2), std::invalid_argument);
}

TEST_CASE("LZW all single-byte alphabet entries round trip") {
    for (int symbol = 0; symbol < 256; ++symbol) {
        const std::vector<std::uint8_t> input{static_cast<std::uint8_t>(symbol)};
        CHECK(DecodeLzw(vispick::gif::EncodeLzw(input), 8) == input);
    }
}

TEST_CASE("LZW repetitive data exercises the missing-code special case") {
    const std::vector<std::uint8_t> input(100000, 23);
    const auto compressed = vispick::gif::EncodeLzw(input);
    CHECK(compressed.size() < input.size() / 20);
    CHECK(DecodeLzw(compressed, 8) == input);
}

TEST_CASE("LZW random input grows to twelve bits and clears full dictionaries") {
    std::mt19937 random(0x45678);
    std::vector<std::uint8_t> input(250000);
    for (auto& pixel : input) {
        pixel = static_cast<std::uint8_t>(random() & 255);
    }
    CHECK(DecodeLzw(vispick::gif::EncodeLzw(input), 8) == input);
}

TEST_CASE("LZW supports all GIF minimum code sizes") {
    std::mt19937 random(0x12345);
    for (int minimumCodeSize = 2; minimumCodeSize <= 8; ++minimumCodeSize) {
        std::vector<std::uint8_t> input(10000);
        for (auto& pixel : input) {
            pixel = static_cast<std::uint8_t>(random() & ((1U << minimumCodeSize) - 1));
        }
        CHECK(DecodeLzw(vispick::gif::EncodeLzw(input, minimumCodeSize), minimumCodeSize) == input);
    }
}

TEST_CASE("LZW end marker survives every short code-size boundary") {
    std::mt19937 random(0x112233);
    std::vector<std::uint8_t> input;
    for (int count = 0; count < 1200; ++count) {
        input.push_back(static_cast<std::uint8_t>(random() & 255));
        CHECK(DecodeLzw(vispick::gif::EncodeLzw(input), 8) == input);
    }
}

TEST_CASE("Exact palette preserves 255 distinct colors in arbitrary order") {
    std::vector<std::uint32_t> pixels;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (std::uint32_t color = 0; color < 255; ++color) {
            pixels.push_back(0xFF000000U | (color << 16) | ((254 - color) << 8) | color);
        }
    }
    const auto result = vispick::gif::Quantize(pixels);
    REQUIRE(result.exact);
    REQUIRE(result.palette.size() == 255);
    REQUIRE(result.pixels.size() == pixels.size());
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        CHECK(result.palette[result.pixels[pixelIndex]] == pixels[pixelIndex]);
    }
}

TEST_CASE("Exact palette ignores alpha and handles a uniform black image") {
    const std::vector<std::uint32_t> pixels{0, 0x80000000U, 0xFF000000U, 0};
    const auto result = vispick::gif::Quantize(pixels);
    REQUIRE(result.exact);
    REQUIRE(result.palette.size() == 1);
    CHECK(result.palette.front() == 0xFF000000U);
    CHECK(std::all_of(result.pixels.begin(), result.pixels.end(), [](auto pixel) { return pixel == 0; }));
}

TEST_CASE("Empty palette input is valid but cannot encode a GIF frame") {
    CHECK(vispick::gif::Quantize({}).palette.empty());
    CHECK_THROWS_AS(vispick::gif::EncodeFrame({}), std::invalid_argument);
}

TEST_CASE("Wu fallback stays within 255 colors and is deterministic") {
    std::vector<std::uint32_t> pixels;
    for (std::uint32_t red = 0; red < 256; red += 8) {
        for (std::uint32_t green = 0; green < 256; green += 8) {
            for (std::uint32_t blue = 0; blue < 256; blue += 8) {
                pixels.push_back(0xFF000000U | (red << 16) | (green << 8) | blue);
            }
        }
    }
    const auto result = vispick::gif::Quantize(pixels);
    const auto again = vispick::gif::Quantize(pixels);
    REQUIRE_FALSE(result.exact);
    CHECK(result.palette.size() <= 255);
    CHECK(result.palette == again.palette);
    CHECK(result.pixels == again.pixels);
    double squaredError = 0;
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        REQUIRE(result.pixels[pixelIndex] < result.palette.size());
        const auto mapped = result.palette[result.pixels[pixelIndex]];
        for (int shift = 0; shift <= 16; shift += 8) {
            const int difference = static_cast<int>((mapped >> shift) & 255) -
                static_cast<int>((pixels[pixelIndex] >> shift) & 255);
            squaredError += difference * difference;
        }
    }
    CHECK(squaredError / static_cast<double>(pixels.size() * 3) < 250.0);
}

TEST_CASE("Wu handles 256 gray values without empty boxes or invalid indices") {
    std::vector<std::uint32_t> pixels;
    for (std::uint32_t gray = 0; gray < 256; ++gray) {
        pixels.push_back(0xFF000000U | (gray * 0x010101U));
    }
    const auto result = vispick::gif::Quantize(pixels);
    REQUIRE_FALSE(result.exact);
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        REQUIRE(result.pixels[pixelIndex] < result.palette.size());
        const int gray = static_cast<int>(result.palette[result.pixels[pixelIndex]] & 255);
        CHECK(std::abs(gray - static_cast<int>(pixelIndex)) <= 4);
    }
}

TEST_CASE("Fused Wu mapping preserves baseline palette indices and exact error accounting") {
    for (const std::uint32_t colorCount : {300U, 512U, 4096U}) {
        std::vector<std::uint32_t> source(8192);
        for (std::size_t pixelIndex = 0; pixelIndex < source.size(); ++pixelIndex) {
            const auto color = static_cast<std::uint32_t>((pixelIndex * 37 + pixelIndex / 131) % colorCount);
            source[pixelIndex] = ((static_cast<std::uint32_t>(pixelIndex) & 3U) << 24) |
                ((color * 2654435761U) & 0x00FFFFFFU);
        }
        const auto baseline = vispick::quantizer_baseline::Quantize(source);
        REQUIRE_FALSE(baseline.exact);
        int expectedMaximumError = 0;
        for (std::size_t pixelIndex = 0; pixelIndex < source.size(); ++pixelIndex) {
            const auto mapped = baseline.palette[baseline.pixels[pixelIndex]];
            for (const unsigned shift : {0U, 8U, 16U}) {
                expectedMaximumError = std::max(expectedMaximumError,
                    std::abs(static_cast<int>((mapped >> shift) & 255U) -
                             static_cast<int>((source[pixelIndex] >> shift) & 255U)));
            }
        }
        for (const bool reuse : {false, true}) {
            vispick::gif::Quantizer quantizer({reuse, 8});
            const auto result = quantizer.Quantize(source);
            CHECK(result.palette == baseline.palette);
            CHECK(result.pixels == baseline.pixels);
            CHECK(result.maximumChannelError == expectedMaximumError);
            const auto repeated = quantizer.Quantize(source);
            CHECK(repeated.palette == baseline.palette);
            CHECK(repeated.pixels == baseline.pixels);
            CHECK(repeated.maximumChannelError == expectedMaximumError);
        }
    }
}

TEST_CASE("GIF 30 fps delays are 3 3 4 centiseconds without accumulated drift") {
    CHECK(vispick::gif::FrameDelay(0, 30) == 3);
    CHECK(vispick::gif::FrameDelay(1, 30) == 3);
    CHECK(vispick::gif::FrameDelay(2, 30) == 4);
    unsigned int duration = 0;
    for (int frame = 0; frame < 1800; ++frame) {
        duration += vispick::gif::FrameDelay(frame, 30);
    }
    CHECK(duration == 6000);
    CHECK_THROWS_AS(vispick::gif::FrameDelay(-1, 30), std::invalid_argument);
    CHECK_THROWS_AS(vispick::gif::FrameDelay(0, 0), std::invalid_argument);
    CHECK_THROWS_AS(vispick::gif::FrameDelay(0, 51), std::invalid_argument);
}

TEST_CASE("GIF delay accounting preserves a second for every supported integer fps") {
    for (int rate = 1; rate <= 50; ++rate) {
        unsigned int duration = 0;
        for (int frame = 0; frame < rate; ++frame) {
            duration += vispick::gif::FrameDelay(frame, rate);
        }
        CHECK(duration == 100);
    }
}

TEST_CASE("GIF writer rejects invalid dimensions and frame size") {
    TestFile file;
    CHECK_THROWS_AS(vispick::gif::GifWriter(file.path, 0, 10), std::invalid_argument);
    CHECK_THROWS_AS(vispick::gif::GifWriter(file.path, 10, 65536), std::invalid_argument);
    vispick::gif::GifWriter writer(file.path, 2, 2);
    CHECK_THROWS_AS(writer.Finish(), std::logic_error);
    const std::vector<std::uint32_t> pixels(3, 0xFF123456U);
    CHECK_THROWS_AS(writer.WriteFrame(vispick::gif::EncodeFrame(pixels), 3), std::invalid_argument);
}

TEST_CASE("WIC independently decodes animated GIF pixels and delay metadata") {
    TestFile file;
    std::vector<std::uint64_t> hashes;
    vispick::gif::GifWriter writer(file.path, 64, 32);
    for (int frame = 0; frame < 30; ++frame) {
        std::vector<std::uint32_t> pixels(64 * 32);
        for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
            const auto color = static_cast<std::uint32_t>((pixelIndex + static_cast<std::size_t>(frame)) % 255);
            pixels[pixelIndex] = 0xFF000000U | color | (color << 8) | ((254 - color) << 16);
        }
        hashes.push_back(vispick::gif::PixelHash(pixels));
        writer.WriteFrame(vispick::gif::EncodeFrame(pixels), vispick::gif::FrameDelay(frame, 30));
    }
    writer.Finish();
    writer.Finish(); // Idempotent close does not append a second trailer.
    const auto decoded = vispick::gif::InspectGif(file.path, hashes);
    CHECK(decoded.width == 64);
    CHECK(decoded.height == 32);
    CHECK(decoded.frameCount == 30);
    CHECK(decoded.durationCentiseconds == 100);
    CHECK(decoded.mismatchedFrames == 0);
    hashes.pop_back();
    CHECK_THROWS_AS(vispick::gif::InspectGif(file.path, hashes), std::runtime_error);
}

TEST_CASE("WIC decodes Wu-quantized output to exactly the chosen palette colors") {
    TestFile file;
    std::mt19937 random(0x9876);
    std::vector<std::uint32_t> pixels(256 * 128);
    for (auto& pixel : pixels) {
        pixel = static_cast<std::uint32_t>(random()) | 0xFF000000U;
    }
    const auto quantized = vispick::gif::Quantize(pixels);
    REQUIRE_FALSE(quantized.exact);
    std::vector<std::uint32_t> expected;
    expected.reserve(pixels.size());
    for (const auto paletteIndex : quantized.pixels) {
        expected.push_back(quantized.palette[paletteIndex]);
    }
    vispick::gif::GifWriter writer(file.path, 256, 128);
    writer.WriteFrame(vispick::gif::EncodeFrame(pixels), 5);
    writer.Finish();
    const std::array<std::uint64_t, 1> hashes{vispick::gif::PixelHash(expected)};
    CHECK(vispick::gif::InspectGif(file.path, hashes).mismatchedFrames == 0);
}

TEST_CASE("WIC reports altered expected pixels as a mismatch") {
    TestFile file;
    const std::vector<std::uint32_t> pixels(16, 0xFF123456U);
    vispick::gif::GifWriter writer(file.path, 4, 4);
    writer.WriteFrame(vispick::gif::EncodeFrame(pixels), 10);
    writer.Finish();
    const std::array<std::uint64_t, 1> wrongHash{0};
    CHECK(vispick::gif::InspectGif(file.path, wrongHash).mismatchedFrames == 1);
    CHECK_THROWS_AS(writer.WriteFrame(vispick::gif::EncodeFrame(pixels), 10), std::invalid_argument);
}

TEST_CASE("GIF inspection rejects truncated files instead of reporting success") {
    TestFile file;
    {
        std::ofstream stream(file.path, std::ios::binary);
        stream.write("GIF89a", 6);
    }
    CHECK_THROWS_AS(vispick::gif::InspectGif(file.path), std::runtime_error);
}

TEST_CASE("GIF writer rejects empty data and preserves valid zero-delay intermediate frames") {
    TestFile file;
    vispick::gif::GifWriter writer(file.path, 4, 4);
    CHECK_THROWS_AS(writer.WriteFrame({}, 3), std::invalid_argument);
    const std::vector<std::uint32_t> pixels(16, 0xFF123456U);
    writer.WriteFrame(vispick::gif::EncodeFrame(pixels), 0);
    writer.Finish();
    CHECK(vispick::gif::InspectGif(file.path).durationCentiseconds == 0);
}

TEST_CASE("Reusable LZW validates late invalid indices and recovers for the next frame") {
    vispick::gif::LzwEncoder encoder;
    std::vector<std::uint8_t> output;
    const std::vector<std::uint8_t> valid(100000, 3);
    encoder.EncodeTo(valid, 2, output);
    CHECK(DecodeLzw(output, 2) == valid);
    auto invalid = valid;
    invalid.back() = 4;
    CHECK_THROWS_AS(encoder.EncodeTo(invalid, 2, output), std::invalid_argument);
    encoder.EncodeTo(valid, 2, output);
    CHECK(DecodeLzw(output, 2) == valid);
    const std::vector<std::uint8_t> paddedButInvalid{0, 1, 2, 3};
    CHECK_THROWS_AS(encoder.EncodeTo(paddedButInvalid, 2, output, 3), std::invalid_argument);
}

TEST_CASE("Compact GIF palettes and LZW alphabets are bit-exact in independent WIC decoding") {
    for (const int colorCount : {2, 3, 16, 255}) {
        CAPTURE(colorCount);
        std::vector<std::uint32_t> pixels(64 * 32);
        for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
            const auto color = static_cast<std::uint32_t>(pixelIndex % static_cast<std::size_t>(colorCount));
            pixels[pixelIndex] = 0xFF000000U | ((color * 97777U) & 0x00FFFFFFU);
        }
        const auto indexed = vispick::gif::Quantize(pixels);
        auto compact = vispick::gif::EncodeIndexedFrame(indexed);
        const int expectedCodeSize = colorCount <= 3 ? 2 : colorCount <= 16 ? 4 : 8;
        CHECK(compact.minimumCodeSize == expectedCodeSize);
        TestFile compactFile;
        {
            vispick::gif::GifWriter writer(compactFile.path, 64, 32);
            writer.WriteFrame(compact, 3);
            writer.Finish();
        }
        const std::array<std::uint64_t, 1> hash{vispick::gif::PixelHash(pixels)};
        CHECK(vispick::gif::InspectGif(compactFile.path, hash).mismatchedFrames == 0);
        TestFile legacyFile;
        compact.minimumCodeSize = 8;
        compact.compressedPixels = vispick::gif::EncodeLzw(indexed.pixels, 8);
        compact.palette.resize(256, 0);
        {
            vispick::gif::GifWriter writer(legacyFile.path, 64, 32);
            writer.WriteFrame(compact, 3);
            writer.Finish();
        }
        CHECK(vispick::gif::InspectGif(legacyFile.path, hash).mismatchedFrames == 0);
        if (colorCount <= 3) {
            CHECK(std::filesystem::file_size(compactFile.path) < std::filesystem::file_size(legacyFile.path));
        }
    }
}

TEST_CASE("Exact palette reuse keeps stable indices even when color encounter order changes") {
    vispick::gif::Quantizer quantizer;
    const std::vector<std::uint32_t> first{0xFF112233U, 0xFF445566U, 0xFF778899U, 0xFF112233U};
    const auto original = quantizer.Quantize(first);
    const auto repeated = quantizer.Quantize(first);
    CHECK(repeated.paletteReused);
    CHECK(repeated.palette == original.palette);
    CHECK(repeated.pixels == original.pixels);
    const std::vector<std::uint32_t> reversed{first[2], first[1], first[0], first[2]};
    const auto reordered = quantizer.Quantize(reversed);
    CHECK(reordered.exact);
    CHECK(reordered.paletteReused);
    CHECK(reordered.palette == original.palette);
    CHECK(reordered.pixels == std::vector<std::uint8_t>{2, 1, 0, 2});
    const auto newColor = quantizer.Quantize(std::array<std::uint32_t, 1>{0xFF112234U});
    CHECK(newColor.exact);
    CHECK_FALSE(newColor.paletteReused);
    CHECK(newColor.palette.front() == 0xFF112234U); // Never approximate an exact UI input.
}

TEST_CASE("Wu palette reuse is static-stable and accepts only bounded per-channel changes") {
    vispick::gif::Quantizer quantizer({true, 8});
    std::vector<std::uint32_t> pixels(4096);
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        pixels[pixelIndex] = 0xFF000000U | static_cast<std::uint32_t>(pixelIndex % 256) * 0x010101U;
    }
    const auto original = quantizer.Quantize(pixels);
    const auto repeated = quantizer.Quantize(pixels);
    REQUIRE_FALSE(original.exact);
    CHECK(repeated.paletteReused);
    CHECK(repeated.palette == original.palette);
    CHECK(repeated.pixels == original.pixels);
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); pixelIndex += 3) {
        const auto red = std::min(255U, ((pixels[pixelIndex] >> 16) & 255U) + 1U);
        pixels[pixelIndex] = (pixels[pixelIndex] & 0xFF00FFFFU) | (red << 16);
    }
    const auto changed = quantizer.Quantize(pixels);
    CHECK(changed.paletteReused);
    CHECK(changed.palette == original.palette);
    CHECK(changed.maximumChannelError <= 8);
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        const auto mapped = changed.palette[changed.pixels[pixelIndex]];
        for (int shift = 0; shift <= 16; shift += 8) {
            CHECK(std::abs(static_cast<int>((mapped >> shift) & 255) -
                           static_cast<int>((pixels[pixelIndex] >> shift) & 255)) <= 8);
        }
    }
    for (auto& pixel : pixels) {
        pixel = (pixel & 0xFF00FFFFU) | 0x00FF0000U;
    }
    const auto rejected = quantizer.Quantize(pixels);
    CHECK_FALSE(rejected.paletteReused);
}

TEST_CASE("Timestamp delay conversion preserves actual irregular capture intervals and dropped frames") {
    vispick::gif::TimestampDelayAccumulator delays(1000000000);
    CHECK(delays.AdvanceTo(1033400000) == 3);
    CHECK(delays.AdvanceTo(1066700000) == 3);
    CHECK(delays.AdvanceTo(1100000000) == 4);
    CHECK(delays.AdvanceTo(1215000000) == 11); // Two or more skipped capture ticks.
    CHECK(delays.AdvanceTo(1221000000) == 1); // Carries the previous 5 ms remainder.
    CHECK(delays.TotalCentiseconds() == 22);
    CHECK(delays.RemainderNanoseconds() == 1000000);
    CHECK_THROWS_AS(static_cast<void>(delays.AdvanceTo(1220000000)), std::invalid_argument);
    CHECK(delays.AdvanceTo(1230000000) == 1); // Invalid input leaves state unchanged.
}

TEST_CASE("Timestamp delays remain within one centisecond for jitter and a full sixty seconds") {
    vispick::gif::TimestampDelayAccumulator delays(0);
    std::uint64_t accumulated = 0;
    for (std::int64_t frame = 1; frame < 1800; ++frame) {
        const auto timestamp = frame * 1000000000 / 30 + (frame % 5) * 173123;
        accumulated += delays.AdvanceTo(timestamp);
        CHECK(accumulated * 10000000 <= static_cast<std::uint64_t>(timestamp));
        CHECK(static_cast<std::uint64_t>(timestamp) - accumulated * 10000000 < 10000000);
    }
    accumulated += delays.AdvanceTo(60000000000LL);
    CHECK(accumulated == 6000);
    CHECK(delays.RemainderNanoseconds() == 0);
    vispick::gif::TimestampDelayAccumulator shortIntervals(-5000000);
    CHECK(shortIntervals.AdvanceTo(0) == 0);
    CHECK(shortIntervals.AdvanceTo(5000000) == 1);
}

TEST_CASE("WIC raw subframes compose transparency and disposal one two three correctly") {
    using vispick::gif::DisposalMethod;
    constexpr std::uint32_t black = 0xFF000000U;
    constexpr std::uint32_t red = 0xFFFF0000U;
    constexpr std::uint32_t green = 0xFF00FF00U;
    constexpr std::uint32_t blue = 0xFF0000FFU;
    const std::vector<std::uint32_t> global{black, red, green, blue};
    TestFile file;
    std::vector<std::vector<std::uint32_t>> expected;
    std::vector<std::uint32_t> canvas(12, red);
    expected.push_back(canvas);
    canvas[5] = blue;
    expected.push_back(canvas); // Other pixel in subframe is transparent.
    canvas[0] = green;
    canvas[1] = green;
    expected.push_back(canvas);
    canvas[0] = black;
    canvas[1] = black; // Previous disposal=background only clears its rectangle.
    canvas[10] = green;
    expected.push_back(canvas);
    canvas[10] = red; // Disposal=previous restores BEFORE the green overlay.
    canvas[11] = blue;
    expected.push_back(canvas);
    {
        vispick::gif::GifWriter writer(file.path, 4, 3, global);
        const auto write = [&](std::vector<std::uint8_t> indices,
                               vispick::gif::GifFrameMetadata metadata) {
            vispick::gif::IndexedImage indexed{global, std::move(indices)};
            writer.WriteFrame(vispick::gif::EncodeIndexedFrame(indexed), metadata);
        };
        write(std::vector<std::uint8_t>(12, 1), {0, 0, 4, 3, 3, DisposalMethod::Keep});
        write({3, 0}, {1, 1, 2, 1, 4, DisposalMethod::Keep, 0});
        write({2, 2}, {0, 0, 2, 1, 3, DisposalMethod::Background});
        write({2}, {2, 2, 1, 1, 4, DisposalMethod::Previous});
        write({3}, {3, 2, 1, 1, 3, DisposalMethod::Keep});
        writer.Finish();
    }
    vispick::gif::GifReader reader(file.path);
    CHECK(reader.Metadata().width == 4);
    CHECK(reader.Metadata().height == 3);
    CHECK(reader.Metadata().backgroundColor == black);
    const auto second = reader.ReadFrame(1);
    CHECK(second.metadata.left == 1);
    CHECK(second.metadata.top == 1);
    CHECK(second.metadata.width == 2);
    CHECK(second.metadata.transparentColorIndex == 0);
    REQUIRE(second.pixels.size() == 2);
    CHECK((second.pixels[1] >> 24) == 0);
    vispick::gif::GifCanvas composited(reader.Metadata());
    std::vector<std::uint64_t> hashes;
    for (unsigned int frame = 0; frame < reader.Metadata().frameCount; ++frame) {
        composited.Compose(reader.ReadFrame(frame));
        CHECK(std::equal(composited.Pixels().begin(), composited.Pixels().end(), expected[frame].begin()));
        hashes.push_back(vispick::gif::PixelHash(expected[frame]));
    }
    const auto inspection = vispick::gif::InspectGif(file.path, hashes);
    CHECK(inspection.mismatchedFrames == 0);
    CHECK(inspection.durationCentiseconds == 17);
    CHECK_THROWS_AS(static_cast<void>(reader.ReadFrame(5)), std::out_of_range);
}

TEST_CASE("GIF compositor handles transparent initial canvas and rejects out-of-bounds subframes") {
    vispick::gif::GifCanvas canvas({2, 2, 0, 0});
    vispick::gif::DecodedGifFrame frame{{1, 1, 1, 1, 3}, {0xFFFF0000U}};
    canvas.Compose(frame);
    CHECK(canvas.Pixels()[0] == 0);
    CHECK(canvas.Pixels()[3] == 0xFFFF0000U);
    frame.metadata.left = 2;
    CHECK_THROWS_AS(canvas.Compose(frame), std::invalid_argument);
    frame.metadata.left = 0;
    frame.pixels.clear();
    CHECK_THROWS_AS(canvas.Compose(frame), std::invalid_argument);
}

TEST_CASE("Global palette reuse omits per-frame tables without changing decoded colors") {
    const std::vector<std::uint32_t> pixels(64, 0xFF123456U);
    const auto encoded = vispick::gif::EncodeFrame(pixels);
    TestFile withGlobal;
    TestFile withLocal;
    {
        vispick::gif::GifWriter globalWriter(withGlobal.path, 8, 8, encoded.palette);
        vispick::gif::GifWriter localWriter(withLocal.path, 8, 8);
        for (int frame = 0; frame < 5; ++frame) {
            globalWriter.WriteFrame(encoded, 3);
            localWriter.WriteFrame(encoded, 3);
        }
        globalWriter.Finish();
        localWriter.Finish();
    }
    const std::vector<std::uint64_t> hashes(5, vispick::gif::PixelHash(pixels));
    CHECK(vispick::gif::InspectGif(withGlobal.path, hashes).mismatchedFrames == 0);
    CHECK(std::filesystem::file_size(withGlobal.path) + 24 == std::filesystem::file_size(withLocal.path));
}
