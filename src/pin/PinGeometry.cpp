#include "pin/PinGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vispick {

SIZE PinImageSize(const SIZE source, const unsigned quarterTurns) noexcept {
    return quarterTurns % 2 == 0 ? source : SIZE{source.cy, source.cx};
}

SIZE ScaledPinSize(const SIZE source, const double scale) {
    if (source.cx <= 0 || source.cy <= 0 || !std::isfinite(scale) || scale <= 0 || source.cx * scale > 32760 ||
        source.cy * scale > 32760) {
        throw std::invalid_argument("Pin dimensions exceed layered-window limits");
    }
    return {std::max<LONG>(1, static_cast<LONG>(std::lround(source.cx * scale))),
            std::max<LONG>(1, static_cast<LONG>(std::lround(source.cy * scale)))};
}

double LimitPinScale(const SIZE source, const double requested, const std::uint64_t maximumPixels) {
    if (source.cx <= 0 || source.cy <= 0 || !std::isfinite(requested) || maximumPixels == 0) {
        throw std::invalid_argument("Invalid pin scale or pixel budget");
    }
    const double limit =
        std::min({8.0, 32760.0 / source.cx, 32760.0 / source.cy,
                  std::sqrt(static_cast<double>(maximumPixels) / (static_cast<double>(source.cx) * source.cy))});
    if (limit < 0.1) { throw std::length_error("Image exceeds pin rendering budget even at 10% scale"); }
    double scale = std::clamp(requested, 0.1, limit);
    SIZE size = ScaledPinSize(source, scale);
    // Rounding each extent can push the area slightly beyond the exact budget.
    if (static_cast<std::uint64_t>(size.cx) * size.cy > maximumPixels) {
        double low = 0.1;
        double high = scale;
        for (int iteration = 0; iteration < 48; ++iteration) {
            const double middle = (low + high) / 2;
            size = ScaledPinSize(source, middle);
            if (static_cast<std::uint64_t>(size.cx) * size.cy <= maximumPixels) {
                low = middle;
            } else {
                high = middle;
            }
        }
        scale = low;
        size = ScaledPinSize(source, scale);
        if (static_cast<std::uint64_t>(size.cx) * size.cy > maximumPixels) {
            throw std::length_error("Pin minimum scale exceeds pixel budget");
        }
    }
    return scale;
}

POINT ZoomPinOrigin(const POINT origin, const POINT anchor, const double previousScale, const double nextScale) {
    if (!std::isfinite(previousScale) || !std::isfinite(nextScale) || previousScale <= 0 || nextScale <= 0) {
        throw std::invalid_argument("Invalid zoom anchor scale");
    }
    const auto coordinate = [&](const LONG start, const LONG fixed) {
        const double value = fixed - (static_cast<double>(fixed) - start) * nextScale / previousScale;
        if (value < std::numeric_limits<LONG>::min() || value > std::numeric_limits<LONG>::max()) {
            throw std::overflow_error("Pin anchor exceeds screen coordinate range");
        }
        return static_cast<LONG>(std::lround(value));
    };
    return {coordinate(origin.x, anchor.x), coordinate(origin.y, anchor.y)};
}

RECT CompactPinBounds(const POINT imageOrigin, const POINT viewAnchor, const int side) noexcept {
    const LONG left = imageOrigin.x + viewAnchor.x - side / 2;
    const LONG top = imageOrigin.y + viewAnchor.y - side / 2;
    return {left, top, left + side, top + side};
}

POINT RestorePinOrigin(const POINT compactOrigin, const POINT viewAnchor, const int side) noexcept {
    return {compactOrigin.x + side / 2 - viewAnchor.x, compactOrigin.y + side / 2 - viewAnchor.y};
}

POINT SnapPinOrigin(const RECT proposed, const std::span<const RECT> targets, const int distance) noexcept {
    LONG horizontal = 0;
    LONG vertical = 0;
    long long bestHorizontal = static_cast<long long>(distance) + 1;
    long long bestVertical = bestHorizontal;
    if (distance <= 0) { return {proposed.left, proposed.top}; }
    for (const RECT target : targets) {
        if (proposed.bottom >= target.top - distance && proposed.top <= target.bottom + distance) {
            for (const LONG moving : {proposed.left, proposed.right}) {
                for (const LONG fixed : {target.left, target.right}) {
                    const auto difference = static_cast<long long>(fixed) - moving;
                    if (std::abs(difference) < bestHorizontal) {
                        bestHorizontal = std::abs(difference);
                        horizontal = static_cast<LONG>(difference);
                    }
                }
            }
        }
        if (proposed.right >= target.left - distance && proposed.left <= target.right + distance) {
            for (const LONG moving : {proposed.top, proposed.bottom}) {
                for (const LONG fixed : {target.top, target.bottom}) {
                    const auto difference = static_cast<long long>(fixed) - moving;
                    if (std::abs(difference) < bestVertical) {
                        bestVertical = std::abs(difference);
                        vertical = static_cast<LONG>(difference);
                    }
                }
            }
        }
    }
    return {proposed.left + horizontal, proposed.top + vertical};
}

POINT KeepPinVisible(const RECT imageBounds, const std::span<const RECT> monitors) noexcept {
    if (monitors.empty()) { return {imageBounds.left, imageBounds.top}; }
    const LONG width = imageBounds.right - imageBounds.left;
    const LONG height = imageBounds.bottom - imageBounds.top;
    const LONG visibleWidth = std::min(32L, width);
    const LONG visibleHeight = std::min(32L, height);
    for (const RECT monitor : monitors) {
        RECT intersection{};
        if (IntersectRect(&intersection, &imageBounds, &monitor) &&
            intersection.right - intersection.left >= visibleWidth &&
            intersection.bottom - intersection.top >= visibleHeight) {
            return {imageBounds.left, imageBounds.top};
        }
    }
    const RECT* nearest = &monitors.front();
    double bestDistance = std::numeric_limits<double>::max();
    for (const RECT& monitor : monitors) {
        const double horizontal =
            static_cast<double>(imageBounds.left) - std::clamp(imageBounds.left, monitor.left, monitor.right);
        const double vertical =
            static_cast<double>(imageBounds.top) - std::clamp(imageBounds.top, monitor.top, monitor.bottom);
        const double distance = horizontal * horizontal + vertical * vertical;
        if (distance < bestDistance) {
            bestDistance = distance;
            nearest = &monitor;
        }
    }
    return {std::clamp(imageBounds.left, nearest->left, std::max(nearest->left, nearest->right - width)),
            std::clamp(imageBounds.top, nearest->top, std::max(nearest->top, nearest->bottom - height))};
}

Bitmap TransformPin(const BitmapView source, const unsigned quarterTurns, const bool flipHorizontal,
                    const bool flipVertical) {
    const SIZE size = PinImageSize({source.Width(), source.Height()}, quarterTurns);
    Bitmap output(size.cx, size.cy);
    for (int row = 0; row < size.cy; ++row) {
        for (int column = 0; column < size.cx; ++column) {
            const int horizontal = flipHorizontal ? size.cx - column - 1 : column;
            const int vertical = flipVertical ? size.cy - row - 1 : row;
            int sourceX = horizontal;
            int sourceY = vertical;
            switch (quarterTurns % 4) {
            case 1:
                sourceX = vertical;
                sourceY = source.Height() - horizontal - 1;
                break;
            case 2:
                sourceX = source.Width() - horizontal - 1;
                sourceY = source.Height() - vertical - 1;
                break;
            case 3:
                sourceX = source.Width() - vertical - 1;
                sourceY = horizontal;
                break;
            default:
                break;
            }
            output.Row(row)[column] = source.Row(sourceY)[sourceX];
        }
    }
    return output;
}

std::uint32_t PremultiplyPinPixel(const std::uint32_t pixel) noexcept {
    const auto alpha = pixel >> 24U;
    return (alpha << 24U) | (((((pixel >> 16U) & 255U) * alpha + 127U) / 255U) << 16U) |
           (((((pixel >> 8U) & 255U) * alpha + 127U) / 255U) << 8U) | (((pixel & 255U) * alpha + 127U) / 255U);
}

} // namespace vispick
