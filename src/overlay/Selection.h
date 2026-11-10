#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vispick {

enum class SelectionHandle { None, Move, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

// All rectangles use physical screen coordinates and exclusive right/bottom.
[[nodiscard]] RECT NormalizeSelection(POINT first, POINT second, RECT desktop) noexcept;
[[nodiscard]] RECT MoveSelection(RECT selection, LONG deltaX, LONG deltaY, RECT desktop) noexcept;
[[nodiscard]] RECT ResizeSelection(RECT selection, SelectionHandle handle, POINT point, RECT desktop) noexcept;
[[nodiscard]] std::array<POINT, 8> SelectionHandles(RECT selection) noexcept;
[[nodiscard]] SelectionHandle HitSelection(RECT selection, POINT point, int radius) noexcept;
[[nodiscard]] RECT PlaceToolbar(RECT selection, SIZE size, RECT monitor, int gap) noexcept;
// Candidates retain their frozen top-to-bottom Z order. Empty entries are ignored.
[[nodiscard]] std::optional<RECT> CycleWindowSelection(std::span<const RECT> candidates,
                                                     RECT current, bool reverse) noexcept;

struct MagnifierLayout {
    RECT bounds{};
    RECT image{};
    RECT center{};
    RECT label{};
};
// Fifteen source pixels remain fifteen cells at every DPI.
[[nodiscard]] MagnifierLayout LayoutMagnifier(POINT pointer, RECT monitor, UINT dpi) noexcept;
[[nodiscard]] std::wstring FormatSampledColor(std::uint32_t color, bool rgb);

class Selection {
public:
    void Reset(RECT desktop) noexcept;
    void Clear() noexcept;
    void Set(RECT rectangle) noexcept;
    void Begin(POINT point, int handleRadius) noexcept;
    void Drag(POINT point) noexcept;
    void End(POINT point, RECT clickTarget) noexcept;
    void CancelDrag() noexcept;
    void Nudge(LONG deltaX, LONG deltaY) noexcept;
    void ResizeBy(LONG widthDelta, LONG heightDelta) noexcept;
    [[nodiscard]] bool HasSelection() const noexcept;
    [[nodiscard]] bool Dragging() const noexcept { return dragging_; }
    [[nodiscard]] bool Creating() const noexcept { return dragging_ && handle_ == SelectionHandle::None; }
    [[nodiscard]] RECT Bounds() const noexcept { return rectangle_; }
    [[nodiscard]] SelectionHandle Handle() const noexcept { return handle_; }
private:
    RECT desktop_{};
    RECT rectangle_{};
    RECT initial_{};
    POINT anchor_{};
    SelectionHandle handle_ = SelectionHandle::None;
    bool dragging_ = false;
};

} // namespace vispick
