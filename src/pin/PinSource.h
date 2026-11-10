#pragma once

#include "imaging/Bitmap.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vispick {

// Screenshots are opaque: retain lossless packed BGR24 and synthesize alpha on
// reads. Images with any non-opaque pixel keep their original straight BGRA.
class PinSource {
public:
    PinSource() = default;
    explicit PinSource(Bitmap image) : width_(image.Width()), height_(image.Height()) {
        if (image.Empty()) { throw std::invalid_argument("Cannot retain an empty pin source"); }
        const auto pixels = image.Pixels();
        const bool opaque =
            std::all_of(pixels.begin(), pixels.end(), [](const std::uint32_t pixel) { return (pixel >> 24U) == 255U; });
        if (!opaque) {
            bgra_ = std::move(image);
            return;
        }
        bgr_.resize(pixels.size() * 3);
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto pixel = pixels[index];
            bgr_[index * 3] = static_cast<std::uint8_t>(pixel);
            bgr_[index * 3 + 1] = static_cast<std::uint8_t>(pixel >> 8U);
            bgr_[index * 3 + 2] = static_cast<std::uint8_t>(pixel >> 16U);
        }
    }
    PinSource(const PinSource&) = delete;
    PinSource& operator=(const PinSource&) = delete;
    PinSource(PinSource&&) noexcept = default;
    PinSource& operator=(PinSource&&) noexcept = default;
    [[nodiscard]] int Width() const noexcept { return width_; }
    [[nodiscard]] int Height() const noexcept { return height_; }
    [[nodiscard]] bool Empty() const noexcept { return bgr_.empty() && bgra_.Empty(); }
    [[nodiscard]] bool PackedOpaque() const noexcept { return !bgr_.empty(); }
    [[nodiscard]] std::uint64_t LogicalBytes() const noexcept {
        return static_cast<std::uint64_t>(width_) * height_ * 4;
    }
    [[nodiscard]] std::uint64_t StorageBytes() const noexcept { return bgr_.capacity() + bgra_.Pixels().size_bytes(); }
    [[nodiscard]] BitmapView NativeBgra() const { return bgra_.Empty() ? BitmapView{} : bgra_.View(); }
    // Coordinates are already clipped by the pin compositor.
    [[nodiscard]] std::uint32_t Pixel(int column, int row) const noexcept {
        const auto index = static_cast<std::size_t>(row) * width_ + column;
        if (bgr_.empty()) { return bgra_.Pixels()[index]; }
        const auto offset = index * 3;
        return 0xFF000000U | (static_cast<std::uint32_t>(bgr_[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bgr_[offset + 1]) << 8U) | bgr_[offset];
    }
    [[nodiscard]] Bitmap Decode() const {
        if (Empty()) { throw std::logic_error("Cannot decode an empty pin source"); }
        Bitmap output(width_, height_);
        for (int row = 0; row < height_; ++row) {
            auto* destination = output.Row(row);
            for (int column = 0; column < width_; ++column) { destination[column] = Pixel(column, row); }
        }
        return output;
    }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> bgr_;
    Bitmap bgra_;
};

} // namespace vispick
