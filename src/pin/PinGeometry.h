#pragma once

#include "imaging/Bitmap.h"
#include <windows.h>
#include <cstdint>
#include <span>

namespace vispick {

[[nodiscard]] SIZE PinImageSize(SIZE source, unsigned quarterTurns) noexcept;
[[nodiscard]] SIZE ScaledPinSize(SIZE source, double scale);
[[nodiscard]] double LimitPinScale(SIZE source, double requested, std::uint64_t maximumPixels);
[[nodiscard]] POINT ZoomPinOrigin(POINT origin, POINT anchor, double previousScale, double nextScale);
[[nodiscard]] RECT CompactPinBounds(POINT imageOrigin, POINT viewAnchor, int side) noexcept;
[[nodiscard]] POINT RestorePinOrigin(POINT compactOrigin, POINT viewAnchor, int side) noexcept;
[[nodiscard]] POINT SnapPinOrigin(RECT proposed, std::span<const RECT> targets, int distance) noexcept;
[[nodiscard]] POINT KeepPinVisible(RECT imageBounds, std::span<const RECT> monitors) noexcept;
[[nodiscard]] Bitmap TransformPin(BitmapView source, unsigned quarterTurns, bool flipHorizontal, bool flipVertical);
[[nodiscard]] std::uint32_t PremultiplyPinPixel(std::uint32_t pixel) noexcept;

} // namespace vispick
