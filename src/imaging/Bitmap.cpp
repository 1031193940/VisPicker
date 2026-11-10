#include "imaging/Bitmap.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick {

BitmapView::BitmapView(int width, int height, std::span<const std::uint32_t> pixels)
    : width_(width), height_(height), pixels_(pixels) {
    if (width < 0 || height < 0 || (width == 0) != (height == 0)) {
        throw std::invalid_argument("Bitmap view dimensions must be positive or both zero");
    }
    const auto pixelWidth = static_cast<std::size_t>(width);
    const auto pixelHeight = static_cast<std::size_t>(height);
    if (pixelHeight != 0 && pixelWidth > pixels.size() / pixelHeight) {
        throw std::invalid_argument("Bitmap view storage does not match dimensions");
    }
    if (pixelWidth * pixelHeight != pixels.size()) {
        throw std::invalid_argument("Bitmap view must be tightly packed with exactly width*height pixels");
    }
}

std::size_t BitmapView::StrideBytes() const noexcept {
    return static_cast<std::size_t>(width_) * sizeof(std::uint32_t);
}

const std::uint32_t* BitmapView::Row(int row) const {
    if (row < 0 || row >= height_) { throw std::out_of_range("Bitmap view row outside image"); }
    return pixels_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
}

Bitmap BitmapView::CropOpaque(int left, int top, int width, int height) const {
    if (left < 0 || top < 0 || width <= 0 || height <= 0 || left > width_ || top > height_ || width > width_ - left ||
        height > height_ - top) {
        throw std::out_of_range("Crop rectangle outside image view");
    }
    Bitmap cropped(width, height);
    for (int row = 0; row < height; ++row) {
        const auto* firstPixel = Row(top + row) + left;
        std::transform(firstPixel, firstPixel + width, cropped.Row(row),
                       [](std::uint32_t pixel) { return pixel | 0xFF000000U; });
    }
    return cropped;
}

Bitmap::Bitmap(int width, int height) : width_(width), height_(height) {
    if (width <= 0 || height <= 0) { throw std::invalid_argument("Bitmap dimensions must be positive"); }
    const auto pixelWidth = static_cast<std::size_t>(width);
    const auto pixelHeight = static_cast<std::size_t>(height);
    // Byte offsets also need to fit ptrdiff_t (e.g. algorithms walking a BGRA
    // byte view), which can be a stricter bound than vector<uint32_t>::max_size.
    const auto maximumPixelCount =
        std::min(pixels_.max_size(),
                 static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(std::uint32_t));
    if (pixelWidth > maximumPixelCount / pixelHeight) {
        throw std::length_error("Bitmap dimensions overflow the addressable buffer");
    }
    pixels_.resize(pixelWidth * pixelHeight);
}

std::size_t Bitmap::StrideBytes() const noexcept {
    return static_cast<std::size_t>(width_) * sizeof(std::uint32_t);
}

std::uint32_t* Bitmap::Row(int row) {
    if (row < 0 || row >= height_) { throw std::out_of_range("Bitmap row outside image"); }
    return pixels_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
}

const std::uint32_t* Bitmap::Row(int row) const {
    if (row < 0 || row >= height_) { throw std::out_of_range("Bitmap row outside image"); }
    return pixels_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
}

Bitmap Bitmap::Crop(int left, int top, int width, int height) const {
    // Subtraction avoids overflow in left + width and top + height.
    if (left < 0 || top < 0 || width <= 0 || height <= 0 || left > width_ || top > height_ || width > width_ - left ||
        height > height_ - top) {
        throw std::out_of_range("Crop rectangle outside image");
    }
    Bitmap cropped(width, height);
    for (int row = 0; row < height; ++row) { std::copy_n(Row(top + row) + left, width, cropped.Row(row)); }
    return cropped;
}

BitmapView Bitmap::View() const {
    return BitmapView(width_, height_, pixels_);
}

} // namespace vispick
