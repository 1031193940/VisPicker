#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vispick {

class Bitmap;

// Borrowed, read-only, tightly packed BGRA; does not extend the owner's lifetime.
// Screen-capture views have unspecified alpha, so consumers compare RGB only.
class BitmapView {
public:
    BitmapView() = default;
    BitmapView(int width, int height, std::span<const std::uint32_t> pixels);
    [[nodiscard]] int Width() const noexcept { return width_; }
    [[nodiscard]] int Height() const noexcept { return height_; }
    [[nodiscard]] std::size_t StrideBytes() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::span<const std::uint32_t> Pixels() const noexcept { return pixels_; }
    [[nodiscard]] const std::uint32_t* Row(int row) const;
    // Explicit output boundary: allocate only the crop and normalize its alpha.
    [[nodiscard]] Bitmap CropOpaque(int left, int top, int width, int height) const;

private:
    int width_ = 0;
    int height_ = 0;
    std::span<const std::uint32_t> pixels_;
};

// Tightly packed, top-down BGRA pixels. The high byte is straight alpha.
class Bitmap {
public:
    Bitmap() = default;
    Bitmap(int width, int height);

    [[nodiscard]] int Width() const noexcept { return width_; }
    [[nodiscard]] int Height() const noexcept { return height_; }
    [[nodiscard]] std::size_t StrideBytes() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::span<std::uint32_t> Pixels() noexcept { return pixels_; }
    [[nodiscard]] std::span<const std::uint32_t> Pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t* Row(int row);
    [[nodiscard]] const std::uint32_t* Row(int row) const;
    [[nodiscard]] Bitmap Crop(int left, int top, int width, int height) const;
    [[nodiscard]] BitmapView View() const;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint32_t> pixels_;
};

} // namespace vispick
