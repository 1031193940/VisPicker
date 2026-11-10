#pragma once

#include "gif/Quantizer.h"
#include "gif/Lzw.h"
#include "gif/GifReader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace vispick::gif {

struct EncodedFrame {
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> compressedPixels;
    std::size_t pixelCount = 0;
    bool exact = true;
    int minimumCodeSize = 8;
    bool paletteReused = false;
    int maximumChannelError = 0;
};

class FrameEncoder {
public:
    explicit FrameEncoder(QuantizerOptions options = {}) : quantizer_(options) {}
    EncodedFrame Encode(std::span<const std::uint32_t> pixels);
    EncodedFrame EncodeIndexed(const IndexedImage& image);

private:
    Quantizer quantizer_;
    LzwEncoder lzw_;
    IndexedImage indexed_;
};

EncodedFrame EncodeFrame(std::span<const std::uint32_t> pixels);
EncodedFrame EncodeIndexedFrame(const IndexedImage& image);
std::uint16_t FrameDelay(int frameNumber, int framesPerSecond);
std::uint64_t PixelHash(std::span<const std::uint32_t> pixels);

class GifWriter {
public:
    GifWriter(const std::filesystem::path& path, int width, int height,
              std::span<const std::uint32_t> globalPalette = {}, std::uint8_t backgroundColorIndex = 0);
    ~GifWriter() = default;
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;

    // Full opaque frame convenience overload; local palettes automatically
    // omitted when identical to the optional global palette.
    void WriteFrame(const EncodedFrame& frame, std::uint16_t delayCentiseconds);
    void WriteFrame(const EncodedFrame& frame, const GifFrameMetadata& metadata);
    void Finish();
    [[nodiscard]] int FrameCount() const noexcept { return frameCount_; }

private:
    void Byte(std::uint8_t value);
    void Word(std::uint16_t value);
    void Bytes(std::span<const std::uint8_t> bytes);
    void Palette(std::span<const std::uint32_t> palette, int tableSize);
    std::ofstream stream_;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    int frameCount_ = 0;
    bool finished_ = false;
    std::vector<std::uint32_t> globalPalette_;
};

struct GifInspection {
    unsigned int frameCount = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    std::uint64_t durationCentiseconds = 0;
    unsigned int mismatchedFrames = 0;
};

// WIC reads raw subframes; GifCanvas applies location, transparency and disposal
// before hashing the logical-screen image. Memory is bounded by a few canvases.
GifInspection InspectGif(const std::filesystem::path& path, std::span<const std::uint64_t> expectedHashes = {});

} // namespace vispick::gif
