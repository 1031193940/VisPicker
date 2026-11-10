#include "editor/Annotation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vispick {
namespace {

RECT Intersect(RECT first, RECT second) noexcept {
    RECT result{};
    IntersectRect(&result, &first, &second);
    return result;
}

RECT Join(RECT first, RECT second) noexcept {
    RECT result{};
    UnionRect(&result, &first, &second);
    return result;
}

LONG BoundedCoordinate(double value) noexcept {
    return static_cast<LONG>(std::clamp(std::round(value), static_cast<double>(std::numeric_limits<LONG>::min()),
                                        static_cast<double>(std::numeric_limits<LONG>::max())));
}

POINT ClampPoint(POINT point, RECT rectangle) noexcept {
    return {std::clamp(point.x, rectangle.left, rectangle.right - 1),
            std::clamp(point.y, rectangle.top, rectangle.bottom - 1)};
}

RECT PointBounds(std::span<const POINT> points, int padding) noexcept {
    if (points.empty()) { return {}; }
    RECT result{points.front().x, points.front().y, points.front().x, points.front().y};
    for (const auto point : points) {
        result.left = std::min(result.left, point.x);
        result.right = std::max(result.right, point.x);
        result.top = std::min(result.top, point.y);
        result.bottom = std::max(result.bottom, point.y);
    }
    result.left = BoundedCoordinate(static_cast<double>(result.left) - padding);
    result.top = BoundedCoordinate(static_cast<double>(result.top) - padding);
    result.right = BoundedCoordinate(static_cast<double>(result.right) + padding + 1);
    result.bottom = BoundedCoordinate(static_cast<double>(result.bottom) + padding + 1);
    return result;
}

RECT GeometryBounds(POINT start, POINT end) noexcept {
    return {std::min(start.x, end.x), std::min(start.y, end.y), std::max(start.x, end.x) + 1,
            std::max(start.y, end.y) + 1};
}

POINT ConstrainWithin(AnnotationTool tool, POINT start, POINT end, bool shift, RECT rectangle) noexcept {
    POINT constrained = ConstrainAnnotation(tool, start, ClampPoint(end, rectangle), shift);
    if (!shift || tool == AnnotationTool::Pen || tool == AnnotationTool::Mosaic) {
        return ClampPoint(constrained, rectangle);
    }
    const double horizontal = static_cast<double>(constrained.x) - start.x;
    const double vertical = static_cast<double>(constrained.y) - start.y;
    double scale = 1;
    if (horizontal != 0) {
        const double available =
            horizontal > 0 ? rectangle.right - 1.0 - start.x : static_cast<double>(start.x) - rectangle.left;
        scale = std::min(scale, available / std::abs(horizontal));
    }
    if (vertical != 0) {
        const double available =
            vertical > 0 ? rectangle.bottom - 1.0 - start.y : static_cast<double>(start.y) - rectangle.top;
        scale = std::min(scale, available / std::abs(vertical));
    }
    constrained = {BoundedCoordinate(start.x + horizontal * scale), BoundedCoordinate(start.y + vertical * scale)};
    return ClampPoint(constrained, rectangle);
}

} // namespace

int AnnotationPixels(int logicalPixels, unsigned dpi) {
    if (logicalPixels <= 0 || logicalPixels > 2048 || dpi < 48 || dpi > 960) {
        throw std::invalid_argument("Invalid annotation size or DPI");
    }
    return std::max(1, MulDiv(logicalPixels, static_cast<int>(dpi), 96));
}

POINT ConstrainAnnotation(AnnotationTool tool, POINT start, POINT end, bool shift) noexcept {
    if (!shift) { return end; }
    const double horizontal = static_cast<double>(end.x) - start.x;
    const double vertical = static_cast<double>(end.y) - start.y;
    if (tool == AnnotationTool::Rectangle || tool == AnnotationTool::Ellipse) {
        const double side = std::max(std::abs(horizontal), std::abs(vertical));
        return {BoundedCoordinate(start.x + (horizontal < 0 ? -side : side)),
                BoundedCoordinate(start.y + (vertical < 0 ? -side : side))};
    }
    if (tool == AnnotationTool::Arrow) {
        constexpr double eighthTurn = 0.78539816339744830962;
        const double angle = std::round(std::atan2(vertical, horizontal) / eighthTurn) * eighthTurn;
        const double length = std::hypot(horizontal, vertical);
        return {BoundedCoordinate(start.x + std::cos(angle) * length),
                BoundedCoordinate(start.y + std::sin(angle) * length)};
    }
    return end;
}

Bitmap MosaicFrozen(BitmapView source, POINT desktopOrigin, RECT screenRectangle, int blockPixels) {
    if (blockPixels < 1 || blockPixels > 2560 || source.Empty()) {
        throw std::invalid_argument("Invalid mosaic source or block size");
    }
    const auto left = static_cast<std::int64_t>(screenRectangle.left) - desktopOrigin.x;
    const auto top = static_cast<std::int64_t>(screenRectangle.top) - desktopOrigin.y;
    const auto width = static_cast<std::int64_t>(screenRectangle.right) - screenRectangle.left;
    const auto height = static_cast<std::int64_t>(screenRectangle.bottom) - screenRectangle.top;
    if (left < 0 || top < 0 || width <= 0 || height <= 0 || left + width > source.Width() ||
        top + height > source.Height()) {
        throw std::out_of_range("Mosaic rectangle is outside the frozen desktop");
    }
    Bitmap result(static_cast<int>(width), static_cast<int>(height));
    for (int blockTop = 0; blockTop < result.Height(); blockTop += blockPixels) {
        for (int blockLeft = 0; blockLeft < result.Width(); blockLeft += blockPixels) {
            const int blockRight = std::min(blockLeft + blockPixels, result.Width());
            const int blockBottom = std::min(blockTop + blockPixels, result.Height());
            std::uint64_t blue = 0, green = 0, red = 0;
            for (int row = blockTop; row < blockBottom; ++row) {
                const auto* pixels = source.Row(static_cast<int>(top) + row);
                for (int column = blockLeft; column < blockRight; ++column) {
                    const auto pixel = pixels[static_cast<int>(left) + column];
                    blue += pixel & 0xFF;
                    green += (pixel >> 8) & 0xFF;
                    red += (pixel >> 16) & 0xFF;
                }
            }
            const auto count =
                static_cast<std::uint64_t>(blockRight - blockLeft) * static_cast<std::uint64_t>(blockBottom - blockTop);
            const auto color =
                0xFF000000U | static_cast<std::uint32_t>((red / count) << 16 | (green / count) << 8 | blue / count);
            for (int row = blockTop; row < blockBottom; ++row) {
                std::fill(result.Row(row) + blockLeft, result.Row(row) + blockRight, color);
            }
        }
    }
    return result;
}

AnnotationModel::AnnotationModel(BitmapView frozen, POINT desktopOrigin) : frozen_(frozen) {
    const auto right = static_cast<std::int64_t>(desktopOrigin.x) + frozen.Width();
    const auto bottom = static_cast<std::int64_t>(desktopOrigin.y) + frozen.Height();
    if (frozen.Empty() || right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max()) {
        throw std::invalid_argument("Invalid frozen desktop bounds");
    }
    desktopBounds_ = {desktopOrigin.x, desktopOrigin.y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    selection_ = desktopBounds_;
}

void AnnotationModel::SetSelection(RECT screenSelection) {
    CancelPreview();
    selection_ = Intersect(screenSelection, desktopBounds_);
}

void AnnotationModel::SetTool(AnnotationTool tool) {
    CancelPreview();
    tool_ = tool;
}

void AnnotationModel::SetStyle(const AnnotationStyle& style) {
    if ((style.strokeWidth != 2 && style.strokeWidth != 4 && style.strokeWidth != 6) ||
        (style.fontSize != 14 && style.fontSize != 20 && style.fontSize != 28) || style.mosaicSize < 1 ||
        style.mosaicSize > 256 || style.fontName.empty() || style.fontName.size() >= LF_FACESIZE ||
        style.color > 0xFFFFFF) {
        throw std::invalid_argument("Invalid annotation style");
    }
    style_ = style;
}

std::span<const Annotation> AnnotationModel::Committed() const noexcept {
    return {annotations_.data(), committedCount_};
}
const Annotation* AnnotationModel::Preview() const noexcept {
    return preview_ ? &*preview_ : nullptr;
}

RECT AnnotationModel::PointerDown(POINT screenPoint, bool shift, unsigned dpi) {
    static_cast<void>(shift);
    if (tool_ == AnnotationTool::None || tool_ == AnnotationTool::Text || !PtInRect(&selection_, screenPoint)) {
        return {};
    }
    Annotation annotation;
    annotation.tool = tool_;
    annotation.color = style_.color;
    annotation.strokePixels = AnnotationPixels(style_.strokeWidth, dpi);
    annotation.mosaicPixels = AnnotationPixels(style_.mosaicSize, dpi);
    annotation.points.push_back(screenPoint);
    if (tool_ != AnnotationTool::Pen) { annotation.points.push_back(screenPoint); }
    annotation.bounds = PointBounds(annotation.points, annotation.strokePixels * 4 + 3);
    const RECT dirty = Join(CancelPreview(), annotation.bounds);
    preview_ = std::move(annotation);
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::PointerMove(POINT screenPoint, bool shift) {
    if (!preview_) { return {}; }
    auto& annotation = *preview_;
    const auto previousBounds = annotation.bounds;
    const auto point = ConstrainWithin(annotation.tool, annotation.points.front(), screenPoint, shift, selection_);
    if (annotation.tool == AnnotationTool::Pen) {
        if (point.x == annotation.points.back().x && point.y == annotation.points.back().y) { return {}; }
        annotation.points.push_back(point);
    } else {
        annotation.points.back() = point;
    }
    annotation.bounds = PointBounds(annotation.points, annotation.strokePixels * 4 + 3);
    if (annotation.tool == AnnotationTool::Pen) {
        const auto recent =
            std::span<const POINT>(annotation.points).last(std::min<std::size_t>(4, annotation.points.size()));
        return Intersect(PointBounds(recent, annotation.strokePixels * 4 + 3), desktopBounds_);
    }
    return Intersect(Join(previousBounds, annotation.bounds), desktopBounds_);
}

RECT AnnotationModel::PointerUp(POINT screenPoint, bool shift) {
    if (!preview_) { return {}; }
    const RECT movement = PointerMove(screenPoint, shift);
    auto annotation = std::move(*preview_);
    preview_.reset();
    const RECT dirty = Intersect(Join(movement, annotation.bounds), desktopBounds_);
    const auto start = annotation.points.front();
    const auto end = annotation.points.back();
    if (annotation.tool != AnnotationTool::Pen && start.x == end.x && start.y == end.y) { return dirty; }
    if (annotation.tool == AnnotationTool::Mosaic) {
        annotation.bounds = GeometryBounds(start, end);
        annotation.mosaic = std::make_shared<const Bitmap>(MosaicFrozen(
            frozen_, {desktopBounds_.left, desktopBounds_.top}, annotation.bounds, annotation.mosaicPixels));
    }
    Commit(std::move(annotation));
    return dirty;
}

void AnnotationModel::Commit(Annotation annotation) {
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(committedCount_), annotations_.end());
    annotations_.push_back(std::move(annotation));
    committedCount_ = annotations_.size();
    ++revision_;
}

RECT AnnotationModel::CancelPreview() noexcept {
    const RECT dirty = preview_ ? preview_->bounds : RECT{};
    preview_.reset();
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::Undo() noexcept {
    const RECT previewBounds = CancelPreview();
    if (!CanUndo()) { return previewBounds; }
    --committedCount_;
    ++revision_;
    return Intersect(Join(previewBounds, annotations_[committedCount_].bounds), desktopBounds_);
}

RECT AnnotationModel::Redo() noexcept {
    const RECT previewBounds = CancelPreview();
    if (!CanRedo()) { return previewBounds; }
    const RECT bounds = annotations_[committedCount_++].bounds;
    ++revision_;
    return Intersect(Join(previewBounds, bounds), desktopBounds_);
}

RECT AnnotationModel::AddText(RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                              std::vector<AnnotationTextLine> lines) {
    if (std::none_of(lines.begin(), lines.end(), [](const AnnotationTextLine& line) { return !line.text.empty(); })) {
        return {};
    }
    const auto bounds = Intersect(screenBounds, desktopBounds_);
    if (IsRectEmpty(&bounds)) { return {}; }
    if (bounds.left != screenBounds.left || bounds.top != screenBounds.top || color > 0xFFFFFF) {
        throw std::invalid_argument("Invalid text annotation origin or color");
    }
    Annotation annotation;
    annotation.tool = AnnotationTool::Text;
    annotation.bounds = bounds;
    annotation.color = color;
    annotation.font = actualFont;
    annotation.textLines = std::move(lines);
    Commit(std::move(annotation));
    return bounds;
}

} // namespace vispick
