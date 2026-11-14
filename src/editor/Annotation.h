#pragma once

#include "imaging/Bitmap.h"
#include <windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vispick {

enum class AnnotationTool { None, Rectangle, Ellipse, Arrow, Pen, Text, Mosaic };

enum class AnnotationHandle {
    None,
    Move,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Start,
    End
};

struct AnnotationHandlePoint {
    AnnotationHandle handle = AnnotationHandle::None;
    POINT point{};
};

struct AnnotationStyle {
    std::uint32_t color = 0xFF3B30; // 0xRRGGBB.
    int strokeWidth = 4;            // Logical pixels; converted once when the gesture starts.
    int fontSize = 20;
    std::wstring fontName = L"Microsoft YaHei";
    int mosaicSize = 10;
};

struct AnnotationTextLine {
    std::wstring text;
    POINT offset{};               // Relative to the text box, from the native EDIT's actual layout.
    std::vector<LONG> tabOffsets; // Native x position after each tab; EDIT uses dialog font units.
};

struct Annotation {
    AnnotationTool tool = AnnotationTool::None;
    std::uint32_t color = 0;
    int strokePixels = 1;
    int mosaicPixels = 1;
    std::vector<POINT> points; // Screen physical coordinates, independent of selection changes.
    RECT bounds{};             // Conservative paint bounds; text/mosaic also use this as their clip.
    LOGFONTW font{};
    std::vector<AnnotationTextLine> textLines;
    std::wstring text; // Original EDIT contents; visual wrapping does not insert hard line breaks.
    unsigned dpi = 96;
    std::shared_ptr<const Bitmap> mosaic;
};

class AnnotationModel {
public:
    // Frozen pixels must remain alive and unchanged for the complete editing session.
    AnnotationModel(BitmapView frozen, POINT desktopOrigin);
    void SetSelection(RECT screenSelection);
    void SetTool(AnnotationTool tool);
    void SetStyle(const AnnotationStyle& style);
    [[nodiscard]] AnnotationTool Tool() const noexcept { return tool_; }
    [[nodiscard]] const AnnotationStyle& Style() const noexcept { return style_; }
    [[nodiscard]] RECT Selection() const noexcept { return selection_; }
    [[nodiscard]] RECT DesktopBounds() const noexcept { return desktopBounds_; }
    [[nodiscard]] BitmapView Frozen() const noexcept { return frozen_; }
    [[nodiscard]] std::span<const Annotation> Committed() const noexcept;
    [[nodiscard]] const Annotation* Preview() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] bool HasAnnotations() const noexcept { return !annotations_.empty(); }
    [[nodiscard]] bool CanUndo() const noexcept { return historyPosition_ != 0; }
    [[nodiscard]] bool CanRedo() const noexcept { return historyPosition_ < history_.size(); }

    [[nodiscard]] const Annotation* Selected() const noexcept;
    [[nodiscard]] std::optional<std::size_t> SelectedIndex() const noexcept { return selected_; }
    [[nodiscard]] AnnotationStyle SelectedStyle() const;
    [[nodiscard]] RECT SelectedBounds() const noexcept;
    [[nodiscard]] std::vector<AnnotationHandlePoint> Handles() const;
    [[nodiscard]] std::optional<std::size_t> HitTest(POINT screenPoint, int tolerance) const noexcept;
    [[nodiscard]] AnnotationHandle HitSelected(POINT screenPoint, int tolerance) const noexcept;
    // Changing selection/tool cancels any unfinished transform without creating history.
    void Select(std::optional<std::size_t> index);
    bool SelectAt(POINT screenPoint, int tolerance);
    void ClearSelected() noexcept;
    bool BeginEdit(POINT screenPoint, int tolerance);
    RECT UpdateEdit(POINT screenPoint, bool shift = false);
    RECT CommitEdit();
    RECT CancelEdit() noexcept;
    [[nodiscard]] bool Editing() const noexcept { return edit_.has_value(); }
    // Text font changes require native EDIT reflow and ReplaceText; this applies its color only.
    RECT ApplySelectedStyle(const AnnotationStyle& style);
    RECT NudgeSelected(LONG deltaX, LONG deltaY);
    RECT RemoveSelected();

    // Returns the screen-space dirty rectangle. Text is handled by TextInput instead.
    RECT PointerDown(POINT screenPoint, bool shift = false, unsigned dpi = 96);
    RECT PointerMove(POINT screenPoint, bool shift = false);
    RECT PointerUp(POINT screenPoint, bool shift = false);
    RECT CancelPreview() noexcept;
    RECT Undo() noexcept;
    RECT Redo() noexcept;
    RECT AddText(RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                 std::vector<AnnotationTextLine> lines, std::wstring text = {}, unsigned dpi = 96);
    RECT ReplaceText(std::size_t index, RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                     std::vector<AnnotationTextLine> lines, std::wstring text, unsigned dpi);

private:
    enum class ChangeKind { Add, Replace, Remove };
    struct HistoryEntry {
        ChangeKind kind = ChangeKind::Add;
        std::size_t index = 0;
        Annotation alternate;
    };
    struct EditState {
        std::size_t index = 0;
        Annotation original;
        POINT pointer{};
        AnnotationHandle handle = AnnotationHandle::None;
    };
    void Commit(Annotation annotation);
    RECT Replace(std::size_t index, Annotation annotation);
    RECT Remove(std::size_t index);
    void PrepareHistory();
    void RefreshGeometry(Annotation& annotation) const;
    BitmapView frozen_;
    RECT desktopBounds_{};
    RECT selection_{};
    AnnotationTool tool_ = AnnotationTool::None;
    AnnotationStyle style_;
    std::optional<Annotation> preview_;
    std::vector<Annotation> annotations_;
    std::optional<std::size_t> selected_;
    std::optional<EditState> edit_;
    std::vector<HistoryEntry> history_;
    std::size_t historyPosition_ = 0;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] int AnnotationPixels(int logicalPixels, unsigned dpi);
[[nodiscard]] POINT ConstrainAnnotation(AnnotationTool tool, POINT start, POINT end, bool shift) noexcept;
[[nodiscard]] Bitmap MosaicFrozen(BitmapView source, POINT desktopOrigin, RECT screenRectangle, int blockPixels);

} // namespace vispick
