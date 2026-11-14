#pragma once

#include "editor/Renderer.h"
#include <memory>

namespace vispick {

class TextInput {
public:
    // Escape and loss of focus post a generation-tagged commitMessage to the parent.
    TextInput(AnnotationModel& model, EditorRenderer& renderer, HWND parent, UINT commitMessage);
    ~TextInput();
    TextInput(const TextInput&) = delete;
    TextInput& operator=(const TextInput&) = delete;
    void Begin(POINT screenPoint, unsigned dpi = 96);
    void BeginEdit(std::size_t annotationIndex);
    // Uses the current text's creation DPI so color changes do not change its size.
    void ApplyStyle(const AnnotationStyle& style);
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] std::optional<std::size_t> EditingIndex() const noexcept;
    RECT Commit(); // May leave EDIT active while waiting for IME completion.
    RECT CommitRequested(WPARAM generation);
    void Cancel() noexcept;
    // Forward parent's WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC here; null means not our control.
    [[nodiscard]] HBRUSH ControlColor(HDC deviceContext, HWND child) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vispick
