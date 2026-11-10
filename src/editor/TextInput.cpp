#include "editor/TextInput.h"
#include "platform/Gdi.h"

#include <imm.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace vispick {
namespace {

[[noreturn]] void TextError(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

class InputContext {
public:
    explicit InputContext(HWND window) : window_(window), context_(ImmGetContext(window)) {}
    ~InputContext() {
        if (context_ != nullptr) { ImmReleaseContext(window_, context_); }
    }
    InputContext(const InputContext&) = delete;
    InputContext& operator=(const InputContext&) = delete;
    void Complete() const noexcept {
        if (context_ != nullptr) { ImmNotifyIME(context_, NI_COMPOSITIONSTR, CPS_COMPLETE, 0); }
    }

private:
    HWND window_;
    HIMC context_;
};

UINT_PTR NextEditGeneration() noexcept {
    static thread_local UINT_PTR generation = 0;
    if (++generation == 0) { ++generation; }
    return generation;
}

} // namespace

struct TextInput::State {
    State(AnnotationModel& source, EditorRenderer& sourceRenderer, HWND owner, UINT message)
        : model(source), renderer(sourceRenderer), parent(owner), commitMessage(message) {}
    ~State() { DestroyEdit(); }

    void DestroyEdit() noexcept {
        if (edit != nullptr) {
            const HWND closing = edit;
            edit = nullptr;
            if (originalProcedure != nullptr) {
                SetWindowLongPtrW(closing, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProcedure));
            }
            SetWindowLongPtrW(closing, GWLP_USERDATA, 0);
            DestroyWindow(closing);
        }
        originalProcedure = nullptr;
        editGeneration = 0;
        composing = false;
        commitAfterComposition = false;
        backgroundBrush.Reset();
        background.reset();
        font.Reset();
    }

    void PostCommit() const noexcept { PostMessageW(parent, commitMessage, editGeneration, 0); }

    void SetBackground(RECT rectangle) {
        const auto pixels = renderer.Flatten(rectangle);
        auto surface = std::make_unique<DibSection>(pixels.Width(), pixels.Height());
        std::copy(pixels.Pixels().begin(), pixels.Pixels().end(), surface->Pixels().begin());
        UniqueGdiObject<HBRUSH> brush(CreatePatternBrush(surface->Handle()));
        if (brush.Get() == nullptr) { TextError("Create text input background brush"); }
        GdiFlush();
        backgroundBrush = std::move(brush);
        background = std::move(surface);
        bounds = rectangle;
    }

    void FitText() {
        const auto lineCount = SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
        const auto desired = std::min<LRESULT>(maximumBounds.bottom - maximumBounds.top,
                                               std::max<LRESULT>(2, lineCount + 1) * lineHeight);
        if (desired == bounds.bottom - bounds.top) { return; }
        POINT oldOrigin{bounds.left, bounds.top};
        ScreenToClient(parent, &oldOrigin);
        const RECT oldClient{oldOrigin.x, oldOrigin.y, oldOrigin.x + bounds.right - bounds.left,
                             oldOrigin.y + bounds.bottom - bounds.top};
        auto resized = bounds;
        resized.bottom = resized.top + static_cast<LONG>(desired);
        SetBackground(resized);
        SetWindowPos(edit, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT formatting{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
        SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
        if (desired < maximumBounds.bottom - maximumBounds.top) {
            const auto first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            if (first != 0) { SendMessageW(edit, EM_LINESCROLL, 0, -first); }
        }
        InvalidateRect(parent, &oldClient, FALSE);
        InvalidateRect(edit, nullptr, TRUE);
    }

    static LRESULT CALLBACK EditProcedure(HWND window, UINT message, WPARAM parameter, LPARAM data) noexcept {
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr || state->originalProcedure == nullptr) {
            return DefWindowProcW(window, message, parameter, data);
        }
        if (message == WM_KEYDOWN && parameter == VK_ESCAPE) {
            state->PostCommit();
            return 0;
        }
        if (message == WM_KEYDOWN && parameter == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (message == WM_KILLFOCUS) { state->PostCommit(); }
        if (message == WM_IME_STARTCOMPOSITION || (message == WM_IME_COMPOSITION && (data & GCS_COMPSTR) != 0)) {
            state->composing = true;
        } else if (message == WM_IME_ENDCOMPOSITION) {
            state->composing = false;
        }
        const LRESULT result = CallWindowProcW(state->originalProcedure, window, message, parameter, data);
        if (message == WM_IME_ENDCOMPOSITION && state->commitAfterComposition) { state->PostCommit(); }
        if (message == WM_CHAR || message == WM_PASTE || message == WM_CUT || message == WM_CLEAR ||
            message == WM_SETTEXT || message == EM_REPLACESEL || message == WM_IME_COMPOSITION ||
            message == WM_IME_CHAR || message == WM_UNDO) {
            try {
                state->FitText();
            } catch (...) {
                OutputDebugStringW(L"VisPick: native text input resize failed; committing existing text.\n");
                state->PostCommit();
            }
        }
        return result;
    }

    AnnotationModel& model;
    EditorRenderer& renderer;
    HWND parent = nullptr;
    UINT commitMessage = 0;
    HWND edit = nullptr;
    UINT_PTR editGeneration = 0;
    bool composing = false;
    bool commitAfterComposition = false;
    WNDPROC originalProcedure = nullptr;
    RECT bounds{};
    RECT maximumBounds{};
    LONG lineHeight = 1;
    std::uint32_t color = 0;
    LOGFONTW actualFont{};
    UniqueGdiObject<HFONT> font;
    std::unique_ptr<DibSection> background;
    UniqueGdiObject<HBRUSH> backgroundBrush;
};

TextInput::TextInput(AnnotationModel& model, EditorRenderer& renderer, HWND parent, UINT commitMessage)
    : state_(std::make_unique<State>(model, renderer, parent, commitMessage)) {
    if (parent == nullptr || commitMessage < WM_APP || commitMessage > 0xBFFF) {
        throw std::invalid_argument("TextInput requires a parent and WM_APP commit message");
    }
}

TextInput::~TextInput() = default;
bool TextInput::Active() const noexcept {
    return state_->edit != nullptr;
}
HWND TextInput::Handle() const noexcept {
    return state_->edit;
}

void TextInput::Begin(POINT screenPoint, unsigned dpi) {
    if (Active()) {
        Commit();
        if (Active()) { return; }
    }
    const auto selection = state_->model.Selection();
    if (!PtInRect(&selection, screenPoint)) { return; }
    const auto& style = state_->model.Style();
    const auto width = std::min(selection.right - screenPoint.x, static_cast<LONG>(AnnotationPixels(400, dpi)));
    state_->maximumBounds = {screenPoint.x, screenPoint.y, screenPoint.x + width, selection.bottom};
    state_->color = style.color;
    state_->actualFont = {};
    state_->actualFont.lfHeight = -AnnotationPixels(style.fontSize, dpi);
    state_->actualFont.lfWeight = FW_NORMAL;
    state_->actualFont.lfCharSet = DEFAULT_CHARSET;
    // Grayscale antialiasing avoids ClearType RGB fringes in captured/exported pixels.
    state_->actualFont.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(state_->actualFont.lfFaceName, style.fontName.c_str());
    state_->font.Reset(CreateFontIndirectW(&state_->actualFont));
    if (state_->font.Get() == nullptr) { TextError("Create text input font"); }
    {
        const MemoryDc context;
        const SelectObjectGuard selected(context.Get(), state_->font.Get());
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(context.Get(), &metrics)) { TextError("Read native font metrics"); }
        state_->lineHeight = std::max(1L, metrics.tmHeight);
    }
    const LONG height = std::min(selection.bottom - screenPoint.y, state_->lineHeight * 2);
    state_->SetBackground({screenPoint.x, screenPoint.y, screenPoint.x + width, screenPoint.y + height});
    POINT client = screenPoint;
    if (!ScreenToClient(state_->parent, &client)) { TextError("Map text input origin"); }
    state_->edit =
        CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL | ES_AUTOVSCROLL, client.x,
                        client.y, width, height, state_->parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (state_->edit == nullptr) { TextError("Create native text input"); }
    SendMessageW(state_->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state_->font.Get()), FALSE);
    SendMessageW(state_->edit, EM_SETLIMITTEXT, 65535, 0);
    SendMessageW(state_->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    RECT formatting{0, 0, width, height};
    SendMessageW(state_->edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
    const int tabDialogUnits = 32; // Native EDIT default: eight average character widths.
    SendMessageW(state_->edit, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabDialogUnits));
    SetWindowLongPtrW(state_->edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state_.get()));
    SetLastError(ERROR_SUCCESS);
    state_->originalProcedure = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(state_->edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&State::EditProcedure)));
    if (state_->originalProcedure == nullptr) {
        const DWORD error = GetLastError();
        state_->DestroyEdit();
        throw std::system_error(static_cast<int>(error), std::system_category(), "Subclass text input");
    }
    state_->editGeneration = NextEditGeneration();
    if (IsWindowVisible(state_->parent)) { SetFocus(state_->edit); }
    InvalidateRect(state_->edit, nullptr, TRUE);
}

RECT TextInput::Commit() {
    if (!Active()) { return {}; }
    const HWND edit = state_->edit;
    if (state_->composing) {
        if (!state_->commitAfterComposition) {
            state_->commitAfterComposition = true;
            const InputContext input(edit);
            input.Complete();
        }
        // Let the native control finish consuming its IME result messages.
        return {};
    }
    const bool restoreFocus = GetFocus() == edit;
    const RECT dirty = state_->bounds;
    try {
        const LRESULT firstVisible = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (firstVisible != 0) { SendMessageW(edit, EM_LINESCROLL, 0, -firstVisible); }
        const ScreenDc context(edit);
        const SelectObjectGuard selected(context.Get(), state_->font.Get());
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(context.Get(), &metrics)) { TextError("Read actual text metrics"); }
        // Remember the mapped face selected by GDI, including the system's fallback choice.
        GetTextFaceW(context.Get(), LF_FACESIZE, state_->actualFont.lfFaceName);
        RECT formatting{};
        SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&formatting));
        std::vector<AnnotationTextLine> lines;
        const LRESULT lineCount = SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
        for (LRESULT line = 0; line < lineCount; ++line) {
            const LRESULT character = SendMessageW(edit, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
            if (character < 0) { continue; }
            const LRESULT length = SendMessageW(edit, EM_LINELENGTH, static_cast<WPARAM>(character), 0);
            if (length < 0 || length > 65535) { throw std::length_error("Native text line exceeds limit"); }
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            text[0] = static_cast<wchar_t>(std::max<LRESULT>(1, length));
            const LRESULT copied =
                SendMessageW(edit, EM_GETLINE, static_cast<WPARAM>(line), reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(std::clamp<LRESULT>(copied, 0, length)));
            const LRESULT position = SendMessageW(edit, EM_POSFROMCHAR, static_cast<WPARAM>(character), 0);
            POINT offset{formatting.left, formatting.top + static_cast<LONG>(line) * metrics.tmHeight};
            if (position != -1) {
                offset.x = static_cast<short>(LOWORD(position));
                offset.y = static_cast<short>(HIWORD(position));
            }
            std::vector<LONG> tabOffsets;
            for (std::size_t characterOffset = 0; characterOffset < text.size(); ++characterOffset) {
                if (text[characterOffset] != L'\t') { continue; }
                const auto afterTab =
                    SendMessageW(edit, EM_POSFROMCHAR, static_cast<WPARAM>(character) + characterOffset + 1, 0);
                tabOffsets.push_back(afterTab == -1 ? offset.x : static_cast<short>(LOWORD(afterTab)));
            }
            lines.push_back({std::move(text), offset, std::move(tabOffsets)});
        }
        state_->model.AddText(state_->bounds, state_->actualFont, state_->color, std::move(lines));
    } catch (...) {
        state_->DestroyEdit();
        if (restoreFocus && GetForegroundWindow() == state_->parent) { SetFocus(state_->parent); }
        throw;
    }
    // The DC and selected font guards have already left scope before HWND/font destruction.
    state_->DestroyEdit();
    if (restoreFocus && GetForegroundWindow() == state_->parent) { SetFocus(state_->parent); }
    return dirty;
}

RECT TextInput::CommitRequested(WPARAM generation) {
    return generation != 0 && generation == state_->editGeneration ? Commit() : RECT{};
}

void TextInput::Cancel() noexcept {
    state_->DestroyEdit();
}

HBRUSH TextInput::ControlColor(HDC deviceContext, HWND child) noexcept {
    if (child != state_->edit || child == nullptr) { return nullptr; }
    SetTextColor(deviceContext, RGB((state_->color >> 16) & 0xFF, (state_->color >> 8) & 0xFF, state_->color & 0xFF));
    SetBkMode(deviceContext, TRANSPARENT);
    SetBrushOrgEx(deviceContext, 0, 0, nullptr);
    return state_->backgroundBrush.Get();
}

} // namespace vispick
