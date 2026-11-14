#include "overlay/Selection.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>

namespace vispick {
namespace {

LONG ClampCoordinate(const LONG value, const LONG minimum, const LONG maximum) noexcept {
    return std::clamp(value, minimum, maximum);
}

LONG ClampDelta(const LONG delta, const LONG start, const LONG end, const LONG minimum, const LONG maximum) noexcept {
    return static_cast<LONG>(std::clamp<std::int64_t>(delta, static_cast<std::int64_t>(minimum) - start,
                                                      static_cast<std::int64_t>(maximum) - end));
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
    return {selection.left + horizontal, selection.top + vertical, selection.right + horizontal,
            selection.bottom + vertical};
}

RECT ResizeSelection(RECT selection, const SelectionHandle handle, POINT point, const RECT desktop) noexcept {
    point.x = ClampCoordinate(point.x, desktop.left, desktop.right);
    point.y = ClampCoordinate(point.y, desktop.top, desktop.bottom);
    const bool left =
        handle == SelectionHandle::TopLeft || handle == SelectionHandle::Left || handle == SelectionHandle::BottomLeft;
    const bool right = handle == SelectionHandle::TopRight || handle == SelectionHandle::Right ||
                       handle == SelectionHandle::BottomRight;
    const bool top =
        handle == SelectionHandle::TopLeft || handle == SelectionHandle::Top || handle == SelectionHandle::TopRight;
    const bool bottom = handle == SelectionHandle::BottomLeft || handle == SelectionHandle::Bottom ||
                        handle == SelectionHandle::BottomRight;
    if (left) { selection.left = std::min(point.x, selection.right - 1); }
    if (right) { selection.right = std::max(point.x, selection.left + 1); }
    if (top) { selection.top = std::min(point.y, selection.bottom - 1); }
    if (bottom) { selection.bottom = std::max(point.y, selection.top + 1); }
    return selection;
}

std::array<POINT, 8> SelectionHandles(const RECT selection) noexcept {
    const LONG middleX = selection.left + (selection.right - selection.left) / 2;
    const LONG middleY = selection.top + (selection.bottom - selection.top) / 2;
    return {{{selection.left, selection.top},
             {middleX, selection.top},
             {selection.right, selection.top},
             {selection.right, middleY},
             {selection.right, selection.bottom},
             {middleX, selection.bottom},
             {selection.left, selection.bottom},
             {selection.left, middleY}}};
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

RECT PlaceToolbar(const RECT selection, const SIZE size, const RECT monitor, const int gap,
                  const RECT excluded) noexcept {
    LONG left = selection.right - size.cx;
    LONG top = selection.bottom + gap;
    if (top + size.cy > monitor.bottom) {
        top = selection.top - size.cy - gap;
        if (top < monitor.top) { top = selection.bottom - size.cy - gap; }
    }
    left = std::clamp(left, monitor.left, std::max(monitor.left, monitor.right - size.cx));
    top = std::clamp(top, monitor.top, std::max(monitor.top, monitor.bottom - size.cy));
    const RECT preferred{left, top, left + size.cx, top + size.cy};
    RECT overlap{};
    if (!IntersectRect(&overlap, &preferred, &excluded)) { return preferred; }
    // Native EDIT children cover the parent-painted toolbar. Keep the usual
    // position when possible, otherwise find room around the current editor.
    const std::array<POINT, 4> alternatives{{{left, excluded.top - size.cy - gap},
                                             {left, excluded.bottom + gap},
                                             {excluded.left - size.cx - gap, top},
                                             {excluded.right + gap, top}}};
    for (auto origin : alternatives) {
        origin.x = std::clamp(origin.x, monitor.left, std::max(monitor.left, monitor.right - size.cx));
        origin.y = std::clamp(origin.y, monitor.top, std::max(monitor.top, monitor.bottom - size.cy));
        const RECT candidate{origin.x, origin.y, origin.x + size.cx, origin.y + size.cy};
        if (!IntersectRect(&overlap, &candidate, &excluded)) { return candidate; }
    }
    // Dock below the editor before the caller limits its viewport. The preferred
    // position can be above the text origin after the screenshot crop changes.
    const LONG dockedTop = std::max(monitor.top, monitor.bottom - size.cy - gap);
    return {left, dockedTop, left + size.cx, dockedTop + size.cy};
}

std::optional<RECT> CycleWindowSelection(const std::span<const RECT> candidates, const RECT current,
                                         const bool reverse) noexcept {
    if (candidates.empty()) { return std::nullopt; }
    const auto selected = std::find_if(candidates.begin(), candidates.end(),
                                       [&](const RECT& candidate) { return EqualRect(&candidate, &current) != FALSE; });
    std::size_t position = selected == candidates.end() ? (reverse ? 0 : candidates.size() - 1)
                                                        : static_cast<std::size_t>(selected - candidates.begin());
    for (std::size_t checked = 0; checked < candidates.size(); ++checked) {
        position =
            reverse ? (position == 0 ? candidates.size() - 1 : position - 1) : (position + 1) % candidates.size();
        const RECT candidate = candidates[position];
        if (!IsRectEmpty(&candidate) && !EqualRect(&candidate, &current)) { return candidate; }
    }
    return selected != candidates.end() && !IsRectEmpty(&current) ? std::optional<RECT>{current} : std::nullopt;
}

MagnifierLayout LayoutMagnifier(const POINT pointer, const RECT monitor, const UINT dpi) noexcept {
    const auto scale = [dpi](const int logical) { return MulDiv(logical, static_cast<int>(dpi), 96); };
    const LONG cell = std::max(1, scale(8));
    const LONG imageSide = cell * 15;
    const LONG padding = std::max(1, scale(2));
    const LONG width = std::max(imageSide + padding * 2, static_cast<LONG>(scale(156)));
    const LONG height = imageSide + padding * 2 + scale(48);
    LONG left = pointer.x + scale(24);
    LONG top = pointer.y + scale(24);
    if (left + width > monitor.right) { left = pointer.x - width - scale(24); }
    if (top + height > monitor.bottom) { top = pointer.y - height - scale(24); }
    left = std::clamp(left, monitor.left, std::max(monitor.left, monitor.right - width));
    top = std::clamp(top, monitor.top, std::max(monitor.top, monitor.bottom - height));
    const LONG imageLeft = left + (width - imageSide) / 2;
    const LONG imageTop = top + padding;
    return {{left, top, left + width, top + height},
            {imageLeft, imageTop, imageLeft + imageSide, imageTop + imageSide},
            {imageLeft + cell * 7, imageTop + cell * 7, imageLeft + cell * 8, imageTop + cell * 8},
            {left + padding, imageTop + imageSide + padding, left + width - padding, top + height}};
}

std::wstring FormatSampledColor(const std::uint32_t color, const bool rgb) {
    wchar_t text[32]{};
    if (rgb) {
        swprintf_s(text, L"RGB(%u, %u, %u)", (color >> 16U) & 255U, (color >> 8U) & 255U, color & 255U);
    } else {
        swprintf_s(text, L"#%06X", color & 0xFFFFFFU);
    }
    return text;
}

void Selection::Reset(const RECT desktop) noexcept {
    desktop_ = desktop;
    Clear();
}
void Selection::Clear() noexcept {
    rectangle_ = {};
    initial_ = {};
    dragging_ = false;
    handle_ = SelectionHandle::None;
}
void Selection::Set(const RECT rectangle) noexcept {
    rectangle_ = NormalizeSelection({rectangle.left, rectangle.top}, {rectangle.right, rectangle.bottom}, desktop_);
}
bool Selection::HasSelection() const noexcept {
    return !IsRectEmpty(&rectangle_);
}

void Selection::Begin(const POINT point, const int handleRadius) noexcept {
    handle_ = HitSelection(rectangle_, point, handleRadius);
    initial_ = rectangle_;
    anchor_ = point;
    dragging_ = true;
    if (handle_ == SelectionHandle::None) { rectangle_ = {point.x, point.y, point.x, point.y}; }
}

void Selection::Drag(const POINT point) noexcept {
    if (!dragging_) { return; }
    if (handle_ == SelectionHandle::None) {
        rectangle_ = NormalizeSelection(anchor_, point, desktop_);
    } else if (handle_ == SelectionHandle::Move) {
        rectangle_ = MoveSelection(initial_, point.x - anchor_.x, point.y - anchor_.y, desktop_);
    } else {
        rectangle_ = ResizeSelection(initial_, handle_, point, desktop_);
    }
}

void Selection::End(const POINT point, const RECT clickTarget) noexcept {
    if (!dragging_) { return; }
    Drag(point);
    if (handle_ == SelectionHandle::None && (IsRectEmpty(&rectangle_) || (rectangle_.right - rectangle_.left < 4 &&
                                                                          rectangle_.bottom - rectangle_.top < 4))) {
        Set(clickTarget);
    }
    dragging_ = false;
    handle_ = SelectionHandle::None;
}

void Selection::Nudge(const LONG deltaX, const LONG deltaY) noexcept {
    if (!dragging_ && HasSelection()) { rectangle_ = MoveSelection(rectangle_, deltaX, deltaY, desktop_); }
}

void Selection::ResizeBy(const LONG widthDelta, const LONG heightDelta) noexcept {
    if (dragging_ || !HasSelection()) { return; }
    rectangle_.right =
        static_cast<LONG>(std::clamp<std::int64_t>(static_cast<std::int64_t>(rectangle_.right) + widthDelta,
                                                   static_cast<std::int64_t>(rectangle_.left) + 1, desktop_.right));
    rectangle_.bottom =
        static_cast<LONG>(std::clamp<std::int64_t>(static_cast<std::int64_t>(rectangle_.bottom) + heightDelta,
                                                   static_cast<std::int64_t>(rectangle_.top) + 1, desktop_.bottom));
}

void Selection::CancelDrag() noexcept {
    if (!dragging_) { return; }
    rectangle_ = initial_;
    dragging_ = false;
    handle_ = SelectionHandle::None;
}

} // namespace vispick
