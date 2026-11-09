#pragma once

#include "editor/Annotation.h"
#include <memory>

namespace vispick {

// Caches committed pixels only for a bounded annotation region.
class EditorRenderer {
public:
    explicit EditorRenderer(const AnnotationModel& model);
    ~EditorRenderer();
    EditorRenderer(const EditorRenderer&) = delete;
    EditorRenderer& operator=(const EditorRenderer&) = delete;
    // Replaces screenClip with frozen+committed pixels, then paints the active gesture.
    // destinationScreenOrigin is the screen point represented by destination DC coordinate (0,0).
    void Draw(HDC destination, POINT destinationScreenOrigin, RECT screenClip, bool preview = true);
    [[nodiscard]] Bitmap Flatten(RECT screenSelection);
    [[nodiscard]] std::uint64_t CacheBuildCount() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vispick
