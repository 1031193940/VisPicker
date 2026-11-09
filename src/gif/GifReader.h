#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace vispick::gif {

enum class DisposalMethod : std::uint8_t {
    Unspecified = 0,
    Keep = 1,
    Background = 2,
    Previous = 3
};

struct GifFrameMetadata {
    unsigned int left = 0;
    unsigned int top = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    std::uint16_t delayCentiseconds = 0;
    DisposalMethod disposal = DisposalMethod::Keep;
    int transparentColorIndex = -1;
};

struct GifDocumentMetadata {
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int frameCount = 0;
    // No global table: transparent canvas. With a global table, use its
    // background color as specified by GIF89a and the WIC animation sample.
    std::uint32_t backgroundColor = 0;
};

struct DecodedGifFrame {
    GifFrameMetadata metadata;
    std::vector<std::uint32_t> pixels;
};

// Read metadata/raw subframes on the constructing COM thread. These pixels
// have not been composed over earlier frames.
class GifReader {
public:
    explicit GifReader(const std::filesystem::path& path);
    ~GifReader();
    GifReader(const GifReader&) = delete;
    GifReader& operator=(const GifReader&) = delete;
    [[nodiscard]] const GifDocumentMetadata& Metadata() const noexcept;
    [[nodiscard]] GifFrameMetadata ReadFrameMetadata(unsigned int frameIndex) const;
    [[nodiscard]] DecodedGifFrame ReadFrame(unsigned int frameIndex) const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

// Pure CPU composition, separate from WIC and file IO for independently
// testable disposal semantics. Compose returns the displayed frame BEFORE
// applying that frame's disposal; disposal runs before the next frame.
class GifCanvas {
public:
    explicit GifCanvas(const GifDocumentMetadata& metadata);
    void Compose(const DecodedGifFrame& frame);
    [[nodiscard]] std::span<const std::uint32_t> Pixels() const noexcept { return pixels_; }

private:
    GifDocumentMetadata document_;
    std::vector<std::uint32_t> pixels_;
    std::vector<std::uint32_t> savedPixels_;
    GifFrameMetadata previous_;
    bool hasPrevious_ = false;
};

} // namespace vispick::gif
