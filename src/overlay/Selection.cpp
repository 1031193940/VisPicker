#include "overlay/Selection.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace vispick {
namespace {

LONG ClampCoordinate(const LONG value, const LONG minimum, const LONG maximum) noexcept {
    return std::clamp(value, minimum, maximum);
}

LONG ClampDelta(const LONG delta, const LONG start, const LONG end, const LONG minimum, const LONG maximum) noexcept {
    return static_cast<LONG>(std::clamp<std::int64_t>(delta,
        static_cast<std::int64_t>(minimum) - start, static_cast<std::int64_t>(maximum) - end));
}

} // namespace

RECT NormalizeSelection(const POINT first, const POINT second, const RECT desktop) noexcept {
    return {ClampCoordinate(std::min(first.x, second.x), desktop.left, desktop.right),
            ClampCoordinate(std::min(first.y, second.y), desktop.top, desktop.bottom),
            ClampCoordinate(std::max(first.x, second.x), desktop.left, desktop.right),
            ClampCoordinate(std::max(first.y, second.y), desktop.top, desktop.bottom)};
}

RECT MoveSelection(const RECT selection, const LONG deltaX, const LONG deltaY, const RECT desktop) noexcept {
    const LONG horizontal = ClampDelta(deltaX, selection.left, selection.right, desktop.left, desktop.right);
    const LONG vertical = ClampDelta(deltaY, selection.top, selection.bottom, desktop.top, desktop.bottom);
    return {selection.left + horizontal, selection.top + vertical, selection.right + horizontal, selection.bottom + vertical};
}

RECT ResizeSelection(RECT selection, const SelectionHandle handle, POINT point, const RECT desktop) noexcept {
    point.x = ClampCoordinate(point.x, desktop.left, desktop.right);
    point.y = ClampCoordinate(point.y, desktop.top, desktop.bottom);
    const bool left = handle == SelectionHandle::TopLeft || handle == SelectionHandle::Left || handle == SelectionHandle::BottomLeft;
    const bool right = handle == SelectionHandle::TopRight || handle == SelectionHandle::Right || handle == SelectionHandle::BottomRight;
    const bool top = handle == SelectionHandle::TopLeft || handle == SelectionHandle::Top || handle == SelectionHandle::TopRight;
    const bool bottom = handle == SelectionHandle::BottomLeft || handle == SelectionHandle::Bottom || handle == SelectionHandle::BottomRight;
    if (left) { selection.left = std::min(point.x, selection.right - 1); }
    if (right) { selection.right = std::max(point.x, selection.left + 1); }
    if (top) { selection.top = std::min(point.y, selection.bottom - 1); }
    if (bottom) { selection.bottom = std::max(point.y, selection.top + 1); }
    return selection;
}

std::array<POINT, 8> SelectionHandles(const RECT selection) noexcept {
    const LONG middleX = selection.left + (selection.right - selection.left) / 2;
    const LONG middleY = selection.top + (selection.bottom - selection.top) / 2;
    return {{{selection.left, selection.top}, {middleX, selection.top}, {selection.right, selection.top},
             {selection.right, middleY}, {selection.right, selection.bottom}, {middleX, selection.bottom},
             {selection.left, selection.bottom}, {selection.left, middleY}}};
}

SelectionHandle HitSelection(const RECT selection, const POINT point, const int radius) noexcept {
    if (IsRectEmpty(&selection)) { return SelectionHandle::None; }
    const auto handles = SelectionHandles(selection);
    for (std::size_t index = 0; index < handles.size(); ++index) {
        if (std::abs(static_cast<std::int64_t>(point.x) - handles[index].x) <= radius &&
            std::abs(static_cast<std::int64_t>(point.y) - handles[index].y) <= radius) {
            return static_cast<SelectionHandle>(static_cast<unsigned>(SelectionHandle::TopLeft) + index);
        }
    }
    return PtInRect(&selection, point) ? SelectionHandle::Move : SelectionHandle::None;
}

RECT PlaceToolbar(const RECT selection, const SIZE size, const RECT monitor, const int gap) noexcept {
    LONG left = selection.right - size.cx;
    LONG top = selection.bottom + gap;
    if (top + size.cy > monitor.bottom) {
        top = selection.top - size.cy - gap;
        if (top < monitor.top) { top = selection.bottom - size.cy - gap; }
    }
    left = std::clamp(left, monitor.left, std::max(monitor.left, monitor.right - size.cx));
    top = std::clamp(top, monitor.top, std::max(monitor.top, monitor.bottom - size.cy));
    return {left, top, left + size.cx, top + size.cy};
}

void Selection::Reset(const RECT desktop) noexcept { desktop_ = desktop; Clear(); }
void Selection::Clear() noexcept { rectangle_ = {}; initial_ = {}; dragging_ = false; handle_ = SelectionHandle::None; }
void Selection::Set(const RECT rectangle) noexcept { rectangle_ = NormalizeSelection({rectangle.left, rectangle.top}, {rectangle.right, rectangle.bottom}, desktop_); }
bool Selection::HasSelection() const noexcept { return !IsRectEmpty(&rectangle_); }

void Selection::Begin(const POINT point, const int handleRadius) noexcept {
    handle_ = HitSelection(rectangle_, point, handleRadius);
    initial_ = rectangle_;
    anchor_ = point;
    dragging_ = true;
    if (handle_ == SelectionHandle::None) { rectangle_ = {point.x, point.y, point.x, point.y}; }
}

void Selection::Drag(const POINT point) noexcept {
    if (!dragging_) { return; }
    if (handle_ == SelectionHandle::None) { rectangle_ = NormalizeSelection(anchor_, point, desktop_); }
    else if (handle_ == SelectionHandle::Move) { rectangle_ = MoveSelection(initial_, point.x - anchor_.x, point.y - anchor_.y, desktop_); }
    else { rectangle_ = ResizeSelection(initial_, handle_, point, desktop_); }
}

void Selection::End(const POINT point, const RECT clickTarget) noexcept {
    if (!dragging_) { return; }
    Drag(point);
    if (handle_ == SelectionHandle::None &&
        (IsRectEmpty(&rectangle_) || (rectangle_.right - rectangle_.left < 4 && rectangle_.bottom - rectangle_.top < 4))) {
        Set(clickTarget);
    }
    dragging_ = false;
    handle_ = SelectionHandle::None;
}

void Selection::Nudge(const LONG deltaX, const LONG deltaY) noexcept {
    if (!dragging_ && HasSelection()) { rectangle_ = MoveSelection(rectangle_, deltaX, deltaY, desktop_); }
}

void Selection::CancelDrag() noexcept {
    if (!dragging_) { return; }
    rectangle_ = initial_;
    dragging_ = false;
    handle_ = SelectionHandle::None;
}

} // namespace vispick
