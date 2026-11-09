#include "gif/GifWriter.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace vispick::gif {
namespace {

int PaletteBits(std::size_t colorCount) {
    if (colorCount == 0 || colorCount > 256) {
        throw std::invalid_argument("A GIF palette must contain 1..256 colors");
    }
    int bits = 1;
    while ((std::size_t{1} << bits) < colorCount) {
        ++bits;
    }
    return bits;
}

} // namespace

EncodedFrame FrameEncoder::Encode(std::span<const std::uint32_t> pixels) {
    quantizer_.QuantizeTo(pixels, indexed_);
    return EncodeIndexed(indexed_);
}

EncodedFrame FrameEncoder::EncodeIndexed(const IndexedImage& image) {
    if (image.pixels.empty()) {
        throw std::invalid_argument("A GIF frame must contain pixels");
    }
    EncodedFrame result;
    result.minimumCodeSize = std::max(2, PaletteBits(image.palette.size()));
    result.palette = image.palette;
    result.pixelCount = image.pixels.size();
    result.exact = image.exact;
    result.paletteReused = image.paletteReused;
    result.maximumChannelError = image.maximumChannelError;
    if (image.pixels.size() > (std::numeric_limits<std::size_t>::max() - 64) / 2) {
        throw std::length_error("GIF frame exceeds the LZW buffer bound");
    }
    // A GIF code never exceeds 12 bits. This includes clear/end-code overhead
    // and avoids vector growth exceeding the recorder's explicit memory bound.
    result.compressedPixels.reserve(image.pixels.size() * 2 + 64);
    lzw_.EncodeTo(image.pixels, result.minimumCodeSize, result.compressedPixels,
                  static_cast<int>(image.palette.size()));
    return result;
}

EncodedFrame EncodeFrame(std::span<const std::uint32_t> pixels) {
    FrameEncoder encoder;
    return encoder.Encode(pixels);
}

EncodedFrame EncodeIndexedFrame(const IndexedImage& image) {
    FrameEncoder encoder;
    return encoder.EncodeIndexed(image);
}

// Kept for fixed-rate fixtures. Production recordings use TimestampDelayAccumulator
// with actual capture-completion times, so latency and skipped captures affect delay.
std::uint16_t FrameDelay(int frameNumber, int framesPerSecond) {
    if (frameNumber < 0 || framesPerSecond < 1 || framesPerSecond > 50) {
        throw std::invalid_argument("GIF frame number or frame rate is invalid");
    }
    const auto start = static_cast<std::int64_t>(frameNumber) * 100 / framesPerSecond;
    const auto end = (static_cast<std::int64_t>(frameNumber) + 1) * 100 / framesPerSecond;
    return static_cast<std::uint16_t>(end - start);
}

std::uint64_t PixelHash(std::span<const std::uint32_t> pixels) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto pixel : pixels) {
        hash ^= pixel & 0x00FFFFFFU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

GifWriter::GifWriter(const std::filesystem::path& path, int width, int height,
                      std::span<const std::uint32_t> globalPalette,
                      std::uint8_t backgroundColorIndex)
    : globalPalette_(globalPalette.begin(), globalPalette.end()) {
    if (width <= 0 || width > 65535 || height <= 0 || height > 65535) {
        throw std::invalid_argument("GIF dimensions must be 1..65535");
    }
    const int globalBits = globalPalette.empty() ? 0 : PaletteBits(globalPalette.size());
    if (!globalPalette.empty() && backgroundColorIndex >= globalPalette.size()) {
        throw std::invalid_argument("GIF background index is outside the global palette");
    }
    width_ = static_cast<std::uint16_t>(width);
    height_ = static_cast<std::uint16_t>(height);
    stream_.exceptions(std::ios::badbit | std::ios::failbit);
    stream_.open(path, std::ios::binary | std::ios::trunc);
    const std::array<std::uint8_t, 6> signature{'G', 'I', 'F', '8', '9', 'a'};
    Bytes(signature);
    Word(width_);
    Word(height_);
    Byte(static_cast<std::uint8_t>(0x70 | (globalBits == 0 ? 0 : 0x80 | (globalBits - 1))));
    Byte(backgroundColorIndex);
    Byte(0);
    if (globalBits != 0) {
        Palette(globalPalette, 1 << globalBits);
    }
    const std::array<std::uint8_t, 19> loop{
        0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
        3, 1, 0, 0, 0};
    Bytes(loop);
}

void GifWriter::Byte(std::uint8_t value) { stream_.put(static_cast<char>(value)); }

void GifWriter::Word(std::uint16_t value) {
    Byte(static_cast<std::uint8_t>(value));
    Byte(static_cast<std::uint8_t>(value >> 8));
}

void GifWriter::Bytes(std::span<const std::uint8_t> bytes) {
    stream_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
}

void GifWriter::Palette(std::span<const std::uint32_t> palette, int tableSize) {
    for (int colorIndex = 0; colorIndex < tableSize; ++colorIndex) {
        const auto color = static_cast<std::size_t>(colorIndex) < palette.size()
            ? palette[static_cast<std::size_t>(colorIndex)] : 0;
        Byte(static_cast<std::uint8_t>(color >> 16));
        Byte(static_cast<std::uint8_t>(color >> 8));
        Byte(static_cast<std::uint8_t>(color));
    }
}

void GifWriter::WriteFrame(const EncodedFrame& frame, std::uint16_t delayCentiseconds) {
    WriteFrame(frame, GifFrameMetadata{0, 0, width_, height_, delayCentiseconds});
}

void GifWriter::WriteFrame(const EncodedFrame& frame, const GifFrameMetadata& metadata) {
    const int tableBits = PaletteBits(frame.palette.size());
    if (finished_ || frame.compressedPixels.empty() || metadata.width == 0 || metadata.height == 0 ||
        metadata.left >= width_ || metadata.top >= height_ || metadata.width > width_ - metadata.left ||
        metadata.height > height_ - metadata.top ||
        frame.pixelCount != static_cast<std::size_t>(metadata.width) * metadata.height ||
        frame.minimumCodeSize < std::max(2, tableBits) || frame.minimumCodeSize > 8 ||
        static_cast<unsigned int>(metadata.disposal) > 3 ||
        metadata.transparentColorIndex < -1 ||
        metadata.transparentColorIndex >= static_cast<int>(frame.palette.size())) {
        throw std::invalid_argument("Invalid GIF frame or writer already finished");
    }
    // Zero-delay intermediate frames are valid GIF89a; timestamp conversion can
    // produce them. The recording caller may coalesce instead for player behavior.
    Byte(0x21);
    Byte(0xF9);
    Byte(4);
    Byte(static_cast<std::uint8_t>((static_cast<unsigned int>(metadata.disposal) << 2) |
        (metadata.transparentColorIndex >= 0 ? 1U : 0U)));
    Word(metadata.delayCentiseconds);
    Byte(static_cast<std::uint8_t>(std::max(0, metadata.transparentColorIndex)));
    Byte(0);
    Byte(0x2C);
    Word(static_cast<std::uint16_t>(metadata.left));
    Word(static_cast<std::uint16_t>(metadata.top));
    Word(static_cast<std::uint16_t>(metadata.width));
    Word(static_cast<std::uint16_t>(metadata.height));
    const bool useGlobal = frame.palette == globalPalette_;
    Byte(useGlobal ? 0 : static_cast<std::uint8_t>(0x80 | (tableBits - 1)));
    if (!useGlobal) {
        Palette(frame.palette, 1 << tableBits);
    }
    Byte(static_cast<std::uint8_t>(frame.minimumCodeSize));
    for (std::size_t offset = 0; offset < frame.compressedPixels.size();) {
        const auto blockLength = std::min<std::size_t>(255, frame.compressedPixels.size() - offset);
        Byte(static_cast<std::uint8_t>(blockLength));
        Bytes(std::span(frame.compressedPixels).subspan(offset, blockLength));
        offset += blockLength;
    }
    Byte(0);
    ++frameCount_;
}

void GifWriter::Finish() {
    if (!finished_) {
        if (frameCount_ == 0) {
            throw std::logic_error("Cannot finish a GIF with no frames");
        }
        Byte(0x3B);
        stream_.flush();
        stream_.close();
        finished_ = true;
    }
}

GifInspection InspectGif(const std::filesystem::path& path,
                         std::span<const std::uint64_t> expectedHashes) {
    GifReader reader(path);
    const auto& document = reader.Metadata();
    if (!expectedHashes.empty() && expectedHashes.size() != document.frameCount) {
        throw std::runtime_error("GIF frame count differs from captured frame count");
    }
    GifCanvas canvas(document);
    GifInspection result{document.frameCount, document.width, document.height};
    for (unsigned int frameIndex = 0; frameIndex < document.frameCount; ++frameIndex) {
        const auto frame = reader.ReadFrame(frameIndex);
        result.durationCentiseconds += frame.metadata.delayCentiseconds;
        canvas.Compose(frame);
        if (!expectedHashes.empty() && PixelHash(canvas.Pixels()) != expectedHashes[frameIndex]) {
            ++result.mismatchedFrames;
        }
    }
    return result;
}

} // namespace vispick::gif
