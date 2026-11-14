#include "editor/Annotation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

RECT EditBounds(const Annotation& annotation) noexcept {
    return annotation.tool == AnnotationTool::Text || annotation.tool == AnnotationTool::Mosaic
               ? annotation.bounds
               : PointBounds(annotation.points, 0);
}

bool SamePoint(POINT first, POINT second) noexcept {
    return first.x == second.x && first.y == second.y;
}

bool SameAnnotation(const Annotation& first, const Annotation& second) noexcept {
    return first.tool == second.tool && first.color == second.color && first.strokePixels == second.strokePixels &&
           first.mosaicPixels == second.mosaicPixels && first.dpi == second.dpi && first.text == second.text &&
           EqualRect(&first.bounds, &second.bounds) && std::memcmp(&first.font, &second.font, sizeof(LOGFONTW)) == 0 &&
           std::equal(first.points.begin(), first.points.end(), second.points.begin(), second.points.end(),
                      SamePoint) &&
           std::equal(first.textLines.begin(), first.textLines.end(), second.textLines.begin(), second.textLines.end(),
                      [](const AnnotationTextLine& left, const AnnotationTextLine& right) {
                          return left.text == right.text && SamePoint(left.offset, right.offset) &&
                                 left.tabOffsets == right.tabOffsets;
                      });
}

double SegmentDistance(POINT point, POINT start, POINT end) noexcept {
    const double horizontal = static_cast<double>(end.x) - start.x;
    const double vertical = static_cast<double>(end.y) - start.y;
    const double lengthSquared = horizontal * horizontal + vertical * vertical;
    const double fraction = lengthSquared == 0 ? 0
                                               : std::clamp(((static_cast<double>(point.x) - start.x) * horizontal +
                                                             (static_cast<double>(point.y) - start.y) * vertical) /
                                                                lengthSquared,
                                                            0.0, 1.0);
    return std::hypot(static_cast<double>(point.x) - start.x - fraction * horizontal,
                      static_cast<double>(point.y) - start.y - fraction * vertical);
}

bool HitGeometry(const Annotation& annotation, POINT point, int tolerance) noexcept {
    const double radius = std::max(0, tolerance) + annotation.strokePixels * 0.5;
    const auto bounds = EditBounds(annotation);
    if (point.x < bounds.left - radius || point.x > bounds.right - 1.0 + radius || point.y < bounds.top - radius ||
        point.y > bounds.bottom - 1.0 + radius) {
        return false;
    }
    if (annotation.tool == AnnotationTool::Text || annotation.tool == AnnotationTool::Mosaic) {
        return PtInRect(&bounds, point) != FALSE;
    }
    if (annotation.points.empty()) { return false; }
    const POINT first = annotation.points.front();
    const POINT last = annotation.points.back();
    if (annotation.tool == AnnotationTool::Rectangle) {
        const POINT topLeft{bounds.left, bounds.top}, topRight{bounds.right - 1, bounds.top};
        const POINT bottomLeft{bounds.left, bounds.bottom - 1}, bottomRight{bounds.right - 1, bounds.bottom - 1};
        return SegmentDistance(point, topLeft, topRight) <= radius ||
               SegmentDistance(point, topRight, bottomRight) <= radius ||
               SegmentDistance(point, bottomRight, bottomLeft) <= radius ||
               SegmentDistance(point, bottomLeft, topLeft) <= radius;
    }
    if (annotation.tool == AnnotationTool::Ellipse) {
        const double horizontalRadius = (bounds.right - 1.0 - bounds.left) * 0.5;
        const double verticalRadius = (bounds.bottom - 1.0 - bounds.top) * 0.5;
        if (horizontalRadius == 0 || verticalRadius == 0) { return SegmentDistance(point, first, last) <= radius; }
        const double horizontal = point.x - (bounds.left + horizontalRadius);
        const double vertical = point.y - (bounds.top + verticalRadius);
        const double normalizedHorizontal = horizontal / horizontalRadius;
        const double normalizedVertical = vertical / verticalRadius;
        const double implicit =
            normalizedHorizontal * normalizedHorizontal + normalizedVertical * normalizedVertical - 1;
        const double gradient =
            2 * std::hypot(normalizedHorizontal / horizontalRadius, normalizedVertical / verticalRadius);
        return gradient != 0 && std::abs(implicit) <= radius * gradient;
    }
    if (annotation.tool == AnnotationTool::Arrow) {
        if (SegmentDistance(point, first, last) <= radius) { return true; }
        const double horizontal = static_cast<double>(last.x) - first.x;
        const double vertical = static_cast<double>(last.y) - first.y;
        const double length = std::hypot(horizontal, vertical);
        if (length == 0) { return false; }
        const double headLength = std::min(length * 0.65, std::max(8.0, annotation.strokePixels * 4.0));
        const double along = ((static_cast<double>(last.x) - point.x) * horizontal +
                              (static_cast<double>(last.y) - point.y) * vertical) /
                             length;
        const double across = std::abs((static_cast<double>(last.x) - point.x) * vertical -
                                       (static_cast<double>(last.y) - point.y) * horizontal) /
                              length;
        return along >= -tolerance && along <= headLength + tolerance && across <= along * 0.5 + radius;
    }
    if (annotation.points.size() == 1) { return SegmentDistance(point, first, first) <= radius; }
    for (std::size_t pointIndex = 1; pointIndex < annotation.points.size(); ++pointIndex) {
        if (SegmentDistance(point, annotation.points[pointIndex - 1], annotation.points[pointIndex]) <= radius) {
            return true;
        }
    }
    return false;
}

std::size_t FillHandles(const Annotation& annotation, std::array<AnnotationHandlePoint, 8>& handles) noexcept {
    if (annotation.tool == AnnotationTool::Arrow && annotation.points.size() >= 2) {
        handles[0] = {AnnotationHandle::Start, annotation.points.front()};
        handles[1] = {AnnotationHandle::End, annotation.points.back()};
        return 2;
    }
    if (annotation.tool != AnnotationTool::Rectangle && annotation.tool != AnnotationTool::Ellipse &&
        annotation.tool != AnnotationTool::Mosaic) {
        return 0;
    }
    const auto bounds = EditBounds(annotation);
    const LONG middleX = bounds.left + (bounds.right - bounds.left - 1) / 2;
    const LONG middleY = bounds.top + (bounds.bottom - bounds.top - 1) / 2;
    handles = {{{AnnotationHandle::TopLeft, {bounds.left, bounds.top}},
                {AnnotationHandle::Top, {middleX, bounds.top}},
                {AnnotationHandle::TopRight, {bounds.right - 1, bounds.top}},
                {AnnotationHandle::Right, {bounds.right - 1, middleY}},
                {AnnotationHandle::BottomRight, {bounds.right - 1, bounds.bottom - 1}},
                {AnnotationHandle::Bottom, {middleX, bounds.bottom - 1}},
                {AnnotationHandle::BottomLeft, {bounds.left, bounds.bottom - 1}},
                {AnnotationHandle::Left, {bounds.left, middleY}}}};
    return handles.size();
}

void ValidateStyle(const AnnotationStyle& style) {
    if ((style.strokeWidth != 2 && style.strokeWidth != 4 && style.strokeWidth != 6) ||
        (style.fontSize != 14 && style.fontSize != 20 && style.fontSize != 28) || style.mosaicSize < 1 ||
        style.mosaicSize > 256 || style.fontName.empty() || style.fontName.size() >= LF_FACESIZE ||
        style.color > 0xFFFFFF) {
        throw std::invalid_argument("Invalid annotation style");
    }
}

bool HasText(const std::vector<AnnotationTextLine>& lines) noexcept {
    return std::any_of(lines.begin(), lines.end(), [](const AnnotationTextLine& line) { return !line.text.empty(); });
}

Annotation MakeText(RECT bounds, const LOGFONTW& font, std::uint32_t color, std::vector<AnnotationTextLine> lines,
                    std::wstring text, unsigned dpi) {
    if (color > 0xFFFFFF || dpi < 48 || dpi > 960) { throw std::invalid_argument("Invalid text style"); }
    if (text.empty()) {
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            if (lineIndex != 0) { text += L"\r\n"; }
            text += lines[lineIndex].text;
        }
    }
    Annotation annotation;
    annotation.tool = AnnotationTool::Text;
    annotation.bounds = bounds;
    annotation.color = color;
    annotation.font = font;
    annotation.textLines = std::move(lines);
    annotation.text = std::move(text);
    annotation.dpi = dpi;
    return annotation;
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
    CancelEdit();
    selection_ = Intersect(screenSelection, desktopBounds_);
}

void AnnotationModel::SetTool(AnnotationTool tool) {
    CancelPreview();
    ClearSelected();
    tool_ = tool;
}

void AnnotationModel::SetStyle(const AnnotationStyle& style) {
    ValidateStyle(style);
    style_ = style;
}

std::span<const Annotation> AnnotationModel::Committed() const noexcept {
    return annotations_;
}
const Annotation* AnnotationModel::Preview() const noexcept {
    return preview_ ? &*preview_ : nullptr;
}

const Annotation* AnnotationModel::Selected() const noexcept {
    return selected_ && *selected_ < annotations_.size() ? &annotations_[*selected_] : nullptr;
}

AnnotationStyle AnnotationModel::SelectedStyle() const {
    auto result = style_;
    if (const auto* annotation = Selected()) {
        result.color = annotation->color;
        if (annotation->tool == AnnotationTool::Mosaic) {
            result.mosaicSize = std::max(1, MulDiv(annotation->mosaicPixels, 96, static_cast<int>(annotation->dpi)));
        } else if (annotation->tool == AnnotationTool::Text) {
            result.fontSize =
                std::max(1, MulDiv(std::abs(annotation->font.lfHeight), 96, static_cast<int>(annotation->dpi)));
            if (*annotation->font.lfFaceName) { result.fontName = annotation->font.lfFaceName; }
        } else {
            result.strokeWidth = std::max(1, MulDiv(annotation->strokePixels, 96, static_cast<int>(annotation->dpi)));
        }
    }
    return result;
}

RECT AnnotationModel::SelectedBounds() const noexcept {
    const auto* annotation = Selected();
    return annotation ? EditBounds(*annotation) : RECT{};
}

std::vector<AnnotationHandlePoint> AnnotationModel::Handles() const {
    const auto* annotation = Selected();
    if (!annotation) { return {}; }
    std::array<AnnotationHandlePoint, 8> handles{};
    const auto count = FillHandles(*annotation, handles);
    return {handles.begin(), handles.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::optional<std::size_t> AnnotationModel::HitTest(POINT screenPoint, int tolerance) const noexcept {
    if (!PtInRect(&selection_, screenPoint)) { return std::nullopt; }
    for (std::size_t index = annotations_.size(); index != 0; --index) {
        if (HitGeometry(annotations_[index - 1], screenPoint, tolerance)) { return index - 1; }
    }
    return std::nullopt;
}

AnnotationHandle AnnotationModel::HitSelected(POINT screenPoint, int tolerance) const noexcept {
    const auto* annotation = Selected();
    if (!annotation) { return AnnotationHandle::None; }
    std::array<AnnotationHandlePoint, 8> handles{};
    const auto count = FillHandles(*annotation, handles);
    for (std::size_t index = 0; index < count; ++index) {
        const POINT point = handles[index].point;
        if (std::abs(static_cast<double>(point.x) - screenPoint.x) <= tolerance &&
            std::abs(static_cast<double>(point.y) - screenPoint.y) <= tolerance) {
            return handles[index].handle;
        }
    }
    return PtInRect(&selection_, screenPoint) && HitGeometry(*annotation, screenPoint, tolerance)
               ? AnnotationHandle::Move
               : AnnotationHandle::None;
}

void AnnotationModel::Select(std::optional<std::size_t> index) {
    if (index && *index >= annotations_.size()) { throw std::out_of_range("Annotation selection index"); }
    CancelEdit();
    selected_ = index;
}

bool AnnotationModel::SelectAt(POINT screenPoint, int tolerance) {
    Select(HitTest(screenPoint, tolerance));
    return selected_.has_value();
}

void AnnotationModel::ClearSelected() noexcept {
    CancelEdit();
    selected_.reset();
}

bool AnnotationModel::BeginEdit(POINT screenPoint, int tolerance) {
    CancelPreview();
    CancelEdit();
    auto handle = HitSelected(screenPoint, tolerance);
    // Current handles take priority; otherwise use visual stacking order for the body hit.
    if (handle == AnnotationHandle::None || handle == AnnotationHandle::Move) {
        if (!SelectAt(screenPoint, tolerance)) { return false; }
        handle = AnnotationHandle::Move;
    }
    edit_ = EditState{*selected_, annotations_[*selected_], screenPoint, handle};
    return true;
}

void AnnotationModel::RefreshGeometry(Annotation& annotation) const {
    if (annotation.tool == AnnotationTool::Text) { return; }
    if (annotation.tool == AnnotationTool::Mosaic) {
        annotation.bounds = GeometryBounds(annotation.points.front(), annotation.points.back());
        annotation.mosaic = std::make_shared<const Bitmap>(MosaicFrozen(
            frozen_, {desktopBounds_.left, desktopBounds_.top}, annotation.bounds, annotation.mosaicPixels));
    } else {
        annotation.bounds = PointBounds(annotation.points, annotation.strokePixels * 4 + 3);
    }
}

RECT AnnotationModel::UpdateEdit(POINT screenPoint, bool shift) {
    if (!edit_ || IsRectEmpty(&selection_)) { return {}; }
    auto changed = edit_->original;
    const auto originalBounds = EditBounds(changed);
    const auto handle = edit_->handle;
    if (handle == AnnotationHandle::Move) {
        // Include zero when a crop change has left an existing annotation partly outside the selection.
        const double horizontal =
            std::clamp(static_cast<double>(screenPoint.x) - edit_->pointer.x,
                       std::min(0.0, static_cast<double>(selection_.left) - originalBounds.left),
                       std::max(0.0, static_cast<double>(selection_.right) - originalBounds.right));
        const double vertical =
            std::clamp(static_cast<double>(screenPoint.y) - edit_->pointer.y,
                       std::min(0.0, static_cast<double>(selection_.top) - originalBounds.top),
                       std::max(0.0, static_cast<double>(selection_.bottom) - originalBounds.bottom));
        for (auto& point : changed.points) {
            point = {BoundedCoordinate(point.x + horizontal), BoundedCoordinate(point.y + vertical)};
        }
        if (changed.tool == AnnotationTool::Text) {
            changed.bounds = {BoundedCoordinate(changed.bounds.left + horizontal),
                              BoundedCoordinate(changed.bounds.top + vertical),
                              BoundedCoordinate(changed.bounds.right + horizontal),
                              BoundedCoordinate(changed.bounds.bottom + vertical)};
        }
    } else if (handle == AnnotationHandle::Start || handle == AnnotationHandle::End) {
        const bool start = handle == AnnotationHandle::Start;
        const POINT anchor = start ? changed.points.back() : changed.points.front();
        const auto point = ConstrainWithin(AnnotationTool::Arrow, anchor, screenPoint, shift, selection_);
        (start ? changed.points.front() : changed.points.back()) = point;
    } else {
        POINT first{originalBounds.left, originalBounds.top};
        POINT last{originalBounds.right - 1, originalBounds.bottom - 1};
        const auto point = ClampPoint(screenPoint, selection_);
        const bool left = handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::Left ||
                          handle == AnnotationHandle::BottomLeft;
        const bool right = handle == AnnotationHandle::TopRight || handle == AnnotationHandle::Right ||
                           handle == AnnotationHandle::BottomRight;
        const bool top = handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::Top ||
                         handle == AnnotationHandle::TopRight;
        const bool bottom = handle == AnnotationHandle::BottomLeft || handle == AnnotationHandle::Bottom ||
                            handle == AnnotationHandle::BottomRight;
        if (left) { first.x = point.x; }
        if (right) { last.x = point.x; }
        if (top) { first.y = point.y; }
        if (bottom) { last.y = point.y; }
        if (shift && (left || right) && (top || bottom) && changed.tool != AnnotationTool::Mosaic) {
            const POINT anchor{left ? originalBounds.right - 1 : originalBounds.left,
                               top ? originalBounds.bottom - 1 : originalBounds.top};
            const auto constrained = ConstrainWithin(changed.tool, anchor, point, true, selection_);
            if (left) {
                first.x = constrained.x;
            } else {
                last.x = constrained.x;
            }
            if (top) {
                first.y = constrained.y;
            } else {
                last.y = constrained.y;
            }
        }
        changed.points.front() = {std::min(first.x, last.x), std::min(first.y, last.y)};
        changed.points.back() = {std::max(first.x, last.x), std::max(first.y, last.y)};
    }
    const auto& previous = annotations_[edit_->index];
    // Compare geometry before recomputing a mosaic, whose shared bitmap identity otherwise changes every tick.
    if (std::equal(changed.points.begin(), changed.points.end(), previous.points.begin(), previous.points.end(),
                   SamePoint) &&
        (changed.tool != AnnotationTool::Text || EqualRect(&changed.bounds, &previous.bounds))) {
        return {};
    }
    RefreshGeometry(changed);
    const auto dirty = Join(previous.bounds, changed.bounds);
    annotations_[edit_->index] = std::move(changed);
    ++revision_;
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::CommitEdit() {
    if (!edit_) { return {}; }
    const auto index = edit_->index;
    const auto dirty = Join(edit_->original.bounds, annotations_[index].bounds);
    if (!SameAnnotation(edit_->original, annotations_[index])) {
        PrepareHistory();
        history_.push_back({ChangeKind::Replace, index, std::move(edit_->original)});
        historyPosition_ = history_.size();
    }
    edit_.reset();
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::CancelEdit() noexcept {
    if (!edit_) { return {}; }
    const auto dirty = Join(edit_->original.bounds, annotations_[edit_->index].bounds);
    if (!SameAnnotation(edit_->original, annotations_[edit_->index])) {
        annotations_[edit_->index] = std::move(edit_->original);
        ++revision_;
    }
    edit_.reset();
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::NudgeSelected(LONG deltaX, LONG deltaY) {
    CancelEdit();
    if (!Selected()) { return {}; }
    edit_ = EditState{*selected_, *Selected(), {}, AnnotationHandle::Move};
    const RECT dirty = UpdateEdit({deltaX, deltaY});
    return Join(dirty, CommitEdit());
}

RECT AnnotationModel::ApplySelectedStyle(const AnnotationStyle& style) {
    ValidateStyle(style);
    CancelEdit();
    if (!Selected()) { return {}; }
    auto changed = *Selected();
    if (changed.tool == AnnotationTool::Mosaic) {
        const int pixels = AnnotationPixels(style.mosaicSize, changed.dpi);
        if (pixels != changed.mosaicPixels) {
            changed.mosaicPixels = pixels;
            RefreshGeometry(changed);
        }
    } else {
        changed.color = style.color;
        if (changed.tool != AnnotationTool::Text) {
            changed.strokePixels = AnnotationPixels(style.strokeWidth, changed.dpi);
            RefreshGeometry(changed);
        }
    }
    return Replace(*selected_, std::move(changed));
}

RECT AnnotationModel::RemoveSelected() {
    CancelEdit();
    return selected_ ? Remove(*selected_) : RECT{};
}

RECT AnnotationModel::PointerDown(POINT screenPoint, bool shift, unsigned dpi) {
    static_cast<void>(shift);
    if (tool_ == AnnotationTool::None || tool_ == AnnotationTool::Text || !PtInRect(&selection_, screenPoint)) {
        return {};
    }
    Annotation annotation;
    annotation.tool = tool_;
    annotation.dpi = dpi;
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
    CancelEdit();
    if (annotations_.size() == annotations_.capacity()) {
        annotations_.reserve(std::max<std::size_t>(8, annotations_.capacity() * 2));
    }
    PrepareHistory();
    history_.push_back({ChangeKind::Add, annotations_.size(), {}});
    annotations_.push_back(std::move(annotation));
    selected_ = annotations_.size() - 1;
    historyPosition_ = history_.size();
    ++revision_;
}

void AnnotationModel::PrepareHistory() {
    if (historyPosition_ == history_.capacity()) {
        history_.reserve(std::max<std::size_t>(8, history_.capacity() * 2));
    }
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(historyPosition_), history_.end());
}

RECT AnnotationModel::Replace(std::size_t index, Annotation annotation) {
    if (index >= annotations_.size()) { throw std::out_of_range("Annotation replacement index"); }
    if (SameAnnotation(annotations_[index], annotation)) { return {}; }
    const auto dirty = Join(annotations_[index].bounds, annotation.bounds);
    PrepareHistory();
    history_.push_back({ChangeKind::Replace, index, std::move(annotations_[index])});
    annotations_[index] = std::move(annotation);
    selected_ = index;
    historyPosition_ = history_.size();
    ++revision_;
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::Remove(std::size_t index) {
    if (index >= annotations_.size()) { throw std::out_of_range("Annotation removal index"); }
    const auto dirty = annotations_[index].bounds;
    PrepareHistory();
    history_.push_back({ChangeKind::Remove, index, std::move(annotations_[index])});
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(index));
    selected_.reset();
    historyPosition_ = history_.size();
    ++revision_;
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::CancelPreview() noexcept {
    const RECT dirty = preview_ ? preview_->bounds : RECT{};
    preview_.reset();
    return Intersect(dirty, desktopBounds_);
}

RECT AnnotationModel::Undo() noexcept {
    const RECT previewBounds = Join(CancelPreview(), CancelEdit());
    if (!CanUndo()) { return previewBounds; }
    auto& entry = history_[--historyPosition_];
    RECT dirty = entry.alternate.bounds;
    if (entry.kind == ChangeKind::Add) {
        dirty = annotations_[entry.index].bounds;
        entry.alternate = std::move(annotations_[entry.index]);
        annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(entry.index));
        selected_.reset();
    } else if (entry.kind == ChangeKind::Remove) {
        // Erasing never shrinks capacity, so restoring a removed element cannot allocate.
        annotations_.insert(annotations_.begin() + static_cast<std::ptrdiff_t>(entry.index),
                            std::move(entry.alternate));
        selected_ = entry.index;
    } else {
        dirty = Join(dirty, annotations_[entry.index].bounds);
        std::swap(annotations_[entry.index], entry.alternate);
        selected_ = entry.index;
    }
    ++revision_;
    return Intersect(Join(previewBounds, dirty), desktopBounds_);
}

RECT AnnotationModel::Redo() noexcept {
    const RECT previewBounds = Join(CancelPreview(), CancelEdit());
    if (!CanRedo()) { return previewBounds; }
    auto& entry = history_[historyPosition_++];
    RECT dirty = entry.alternate.bounds;
    if (entry.kind == ChangeKind::Add) {
        annotations_.insert(annotations_.begin() + static_cast<std::ptrdiff_t>(entry.index),
                            std::move(entry.alternate));
        selected_ = entry.index;
    } else if (entry.kind == ChangeKind::Remove) {
        dirty = annotations_[entry.index].bounds;
        entry.alternate = std::move(annotations_[entry.index]);
        annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(entry.index));
        selected_.reset();
    } else {
        dirty = Join(dirty, annotations_[entry.index].bounds);
        std::swap(annotations_[entry.index], entry.alternate);
        selected_ = entry.index;
    }
    ++revision_;
    return Intersect(Join(previewBounds, dirty), desktopBounds_);
}

RECT AnnotationModel::AddText(RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                              std::vector<AnnotationTextLine> lines, std::wstring text, unsigned dpi) {
    if (!HasText(lines)) { return {}; }
    const auto bounds = Intersect(screenBounds, desktopBounds_);
    if (IsRectEmpty(&bounds)) { return {}; }
    if (bounds.left != screenBounds.left || bounds.top != screenBounds.top || color > 0xFFFFFF) {
        throw std::invalid_argument("Invalid text annotation origin or color");
    }
    Commit(MakeText(bounds, actualFont, color, std::move(lines), std::move(text), dpi));
    return bounds;
}

RECT AnnotationModel::ReplaceText(std::size_t index, RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                                  std::vector<AnnotationTextLine> lines, std::wstring text, unsigned dpi) {
    CancelEdit();
    if (index >= annotations_.size() || annotations_[index].tool != AnnotationTool::Text) {
        throw std::out_of_range("Text annotation replacement index");
    }
    if (!HasText(lines)) { return Remove(index); }
    const auto bounds = Intersect(screenBounds, desktopBounds_);
    if (IsRectEmpty(&bounds) || bounds.left != screenBounds.left || bounds.top != screenBounds.top) {
        throw std::invalid_argument("Invalid replacement text bounds");
    }
    return Replace(index, MakeText(bounds, actualFont, color, std::move(lines), std::move(text), dpi));
}

} // namespace vispick
