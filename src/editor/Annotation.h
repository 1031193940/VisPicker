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
    [[nodiscard]] bool HasAnnotations() const noexcept { return committedCount_ != 0; }
    [[nodiscard]] bool CanUndo() const noexcept { return committedCount_ != 0; }
    [[nodiscard]] bool CanRedo() const noexcept { return committedCount_ < annotations_.size(); }

    // Returns the screen-space dirty rectangle. Text is handled by TextInput instead.
    RECT PointerDown(POINT screenPoint, bool shift = false, unsigned dpi = 96);
    RECT PointerMove(POINT screenPoint, bool shift = false);
    RECT PointerUp(POINT screenPoint, bool shift = false);
    RECT CancelPreview() noexcept;
    RECT Undo() noexcept;
    RECT Redo() noexcept;
    RECT AddText(RECT screenBounds, const LOGFONTW& actualFont, std::uint32_t color,
                 std::vector<AnnotationTextLine> lines);

private:
    void Commit(Annotation annotation);
    BitmapView frozen_;
    RECT desktopBounds_{};
    RECT selection_{};
    AnnotationTool tool_ = AnnotationTool::None;
    AnnotationStyle style_;
    std::optional<Annotation> preview_;
    std::vector<Annotation> annotations_;
    std::size_t committedCount_ = 0;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] int AnnotationPixels(int logicalPixels, unsigned dpi);
[[nodiscard]] POINT ConstrainAnnotation(AnnotationTool tool, POINT start, POINT end, bool shift) noexcept;
[[nodiscard]] Bitmap MosaicFrozen(BitmapView source, POINT desktopOrigin, RECT screenRectangle, int blockPixels);

} // namespace vispick
