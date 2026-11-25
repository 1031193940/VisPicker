#include "editor/TextInput.h"
#include "platform/Gdi.h"

#include <imm.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cstring>
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

class ScopedFlag {
public:
    ScopedFlag(bool& target, bool value) noexcept : target_(target), previous_(target) { target_ = value; }
    ~ScopedFlag() { target_ = previous_; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;

private:
    bool& target_;
    bool previous_;
};

} // namespace

struct TextInput::State {
    State(AnnotationModel& source, EditorRenderer& sourceRenderer, HWND owner, UINT message, UINT geometryMessage)
        : model(source), renderer(sourceRenderer), parent(owner), commitMessage(message),
          layoutMessage(geometryMessage) {}
    ~State() { DestroyEdit(); }

    void InvalidateBounds(RECT rectangle) const noexcept {
        if (IsRectEmpty(&rectangle)) { return; }
        POINT origin{rectangle.left, rectangle.top};
        if (!ScreenToClient(parent, &origin)) { return; }
        const RECT client{origin.x, origin.y, origin.x + rectangle.right - rectangle.left,
                          origin.y + rectangle.bottom - rectangle.top};
        InvalidateRect(parent, &client, FALSE);
    }

    void DestroyEdit() noexcept {
        RECT dirty{};
        UnionRect(&dirty, &bounds, &originalBounds);
        renderer.SetHiddenAnnotation(std::nullopt);
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
        paintBuffer.reset();
        painting = false;
        printing = false;
        suppressDirectPaint = false;
        redrawDisabled = false;
        font.Reset();
        editingIndex.reset();
        originalText.clear();
        originalBounds = {};
        originalFontSize = 0;
        logicalFontSize = 0;
        bounds = {};
        viewportHeightLimit = 0;
        InvalidateBounds(dirty);
    }

    void Open(RECT maximum, RECT initial, const LOGFONTW& selectedFont, std::uint32_t selectedColor,
              unsigned selectedDpi, int selectedFontSize, const std::wstring& initialText);

    [[nodiscard]] std::wstring ReadText() const {
        const LRESULT length = SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
        if (length < 0 || length > 65535) { throw std::length_error("Native text exceeds limit"); }
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        const LRESULT copied =
            SendMessageW(edit, WM_GETTEXT, static_cast<WPARAM>(text.size()), reinterpret_cast<LPARAM>(text.data()));
        if (copied < 0 || copied > length) { throw std::runtime_error("Invalid native text length"); }
        text.resize(static_cast<std::size_t>(copied));
        return text;
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

    [[nodiscard]] LONG ViewportHeight() const noexcept {
        const LONG height = bounds.bottom - bounds.top;
        return viewportHeightLimit > 0 ? std::min(height, viewportHeightLimit) : height;
    }

    void ResizeViewport() {
        RECT current{};
        if (!GetClientRect(edit, &current)) { TextError("Read text input viewport"); }
        const LONG width = bounds.right - bounds.left;
        const LONG height = ViewportHeight();
        if (current.right == width && current.bottom == height) { return; }
        if (!SetWindowPos(edit, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            TextError("Resize text input viewport");
        }
        RECT formatting{0, 0, width, height};
        SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
        if (layoutMessage != 0) { PostMessageW(parent, layoutMessage, editGeneration, 0); }
    }

    void RestoreFullLayout() {
        if (ViewportHeight() == bounds.bottom - bounds.top) { return; }
        // The smaller editing viewport must not affect native line/tab positions at export.
        // Commit destroys the HWND immediately afterwards, so no repaint is needed.
        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
        const LONG width = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
        if (!SetWindowPos(edit, nullptr, 0, 0, width, height,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW)) {
            TextError("Restore text layout for commit");
        }
        RECT formatting{0, 0, width, height};
        SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
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
        ResizeViewport();
        if (desired <= ViewportHeight() && desired < maximumBounds.bottom - maximumBounds.top) {
            const auto first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            if (first != 0) { SendMessageW(edit, EM_LINESCROLL, 0, -first); }
        }
        InvalidateRect(parent, &oldClient, FALSE);
        InvalidateRect(edit, nullptr, FALSE);
    }

    void BlitBackground(HDC target, LONG width, LONG height) const noexcept {
        if (target == nullptr || !background || width <= 0 || height <= 0) { return; }
        const LONG copyWidth = std::min<LONG>(width, background->Width());
        const LONG copyHeight = std::min<LONG>(height, background->Height());
        if (copyWidth > 0 && copyHeight > 0) {
            BitBlt(target, 0, 0, static_cast<int>(copyWidth), static_cast<int>(copyHeight), background->DeviceContext(),
                   0, 0, SRCCOPY);
        }
    }

    void RefreshDisplay() noexcept {
        if (edit == nullptr || redrawDisabled || painting || suppressDirectPaint || !IsWindowVisible(edit)) { return; }
        InvalidateRect(edit, nullptr, FALSE);
        UpdateWindow(edit);
    }

    LRESULT PaintEdit(HWND window) noexcept {
        RECT client{};
        if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0 || !background || painting) {
            return CallWindowProcW(originalProcedure, window, WM_PAINT, 0, 0);
        }
        try {
            if (!paintBuffer || paintBuffer->Width() != client.right || paintBuffer->Height() != client.bottom) {
                paintBuffer.reset();
                paintBuffer =
                    std::make_unique<DibSection>(static_cast<int>(client.right), static_cast<int>(client.bottom));
            }
        } catch (...) { return CallWindowProcW(originalProcedure, window, WM_PAINT, 0, 0); }
        const ScopedFlag paintingGuard(painting, true);
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(window, &paint);
        if (context != nullptr && !IsRectEmpty(&paint.rcPaint)) {
            BlitBackground(paintBuffer->DeviceContext(), client.right, client.bottom);
            CallWindowProcW(originalProcedure, window, WM_PRINTCLIENT,
                            reinterpret_cast<WPARAM>(paintBuffer->DeviceContext()), PRF_CLIENT);
            BitBlt(context, static_cast<int>(paint.rcPaint.left), static_cast<int>(paint.rcPaint.top),
                   static_cast<int>(paint.rcPaint.right - paint.rcPaint.left),
                   static_cast<int>(paint.rcPaint.bottom - paint.rcPaint.top), paintBuffer->DeviceContext(),
                   static_cast<int>(paint.rcPaint.left), static_cast<int>(paint.rcPaint.top), SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }

    static LRESULT CALLBACK EditProcedure(HWND window, UINT message, WPARAM parameter, LPARAM data) noexcept {
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr || state->originalProcedure == nullptr) {
            return DefWindowProcW(window, message, parameter, data);
        }
        if (message == WM_ERASEBKGND) { return 1; }
        if (message == WM_PAINT && parameter == 0) { return state->PaintEdit(window); }
        if (message == WM_PRINTCLIENT) {
            const ScopedFlag printingGuard(state->printing, true);
            if ((data & PRF_ERASEBKGND) != 0 && parameter != 0) {
                RECT client{};
                if (GetClientRect(window, &client)) {
                    state->BlitBackground(reinterpret_cast<HDC>(parameter), client.right, client.bottom);
                }
            }
            return CallWindowProcW(state->originalProcedure, window, message, parameter, data);
        }
        if (message == WM_SETREDRAW) {
            state->redrawDisabled = parameter == 0;
            const LRESULT result = CallWindowProcW(state->originalProcedure, window, message, parameter, data);
            if (!state->redrawDisabled && state->edit == window) { state->RefreshDisplay(); }
            return result;
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
        const bool textChanged = message == WM_CHAR || message == WM_UNICHAR || message == WM_KEYDOWN ||
                                 message == WM_SYSKEYDOWN || message == WM_PASTE || message == WM_CUT ||
                                 message == WM_CLEAR || message == WM_SETTEXT || message == EM_REPLACESEL ||
                                 message == WM_IME_COMPOSITION || message == WM_IME_CHAR || message == WM_UNDO ||
                                 message == EM_UNDO;
        const bool visualChanged =
            textChanged || message == WM_IME_STARTCOMPOSITION || message == WM_IME_ENDCOMPOSITION ||
            message == EM_SETSEL || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
            message == WM_LBUTTONDBLCLK || message == WM_RBUTTONUP || message == WM_CONTEXTMENU ||
            (message == WM_MOUSEMOVE && ((parameter & MK_LBUTTON) != 0 || GetCapture() == window)) ||
            message == WM_CAPTURECHANGED || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            (message == WM_TIMER && GetCapture() == window) || message == WM_VSCROLL || message == WM_HSCROLL ||
            message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == EM_SCROLL || message == EM_LINESCROLL ||
            message == EM_SCROLLCARET || message == EM_SETRECT || message == EM_SETRECTNP || message == EM_SETMARGINS ||
            message == EM_SETTABSTOPS || message == WM_SETFONT || message == WM_SIZE;
        const bool suppressPaint = visualChanged && message != WM_CONTEXTMENU && !state->redrawDisabled &&
                                   !state->painting && IsWindowVisible(window);
        LRESULT result = 0;
        {
            const ScopedFlag suppress(state->suppressDirectPaint, state->suppressDirectPaint || suppressPaint);
            result = CallWindowProcW(state->originalProcedure, window, message, parameter, data);
        }
        if (state->edit != window) { return result; }
        if (message == WM_IME_ENDCOMPOSITION && state->commitAfterComposition) { state->PostCommit(); }
        if (textChanged) {
            try {
                state->FitText();
            } catch (...) {
                OutputDebugStringW(L"VisPick: native text input resize failed; committing existing text.\n");
                state->PostCommit();
            }
        }
        if (visualChanged && state->edit == window) { state->RefreshDisplay(); }
        return result;
    }

    AnnotationModel& model;
    EditorRenderer& renderer;
    HWND parent = nullptr;
    UINT commitMessage = 0;
    UINT layoutMessage = 0;
    HWND edit = nullptr;
    UINT_PTR editGeneration = 0;
    bool composing = false;
    bool commitAfterComposition = false;
    bool painting = false;
    bool printing = false;
    bool suppressDirectPaint = false;
    bool redrawDisabled = false;
    WNDPROC originalProcedure = nullptr;
    RECT bounds{};
    RECT maximumBounds{};
    LONG viewportHeightLimit = 0;
    LONG lineHeight = 1;
    unsigned dpi = 96;
    std::optional<std::size_t> editingIndex;
    std::wstring originalText;
    RECT originalBounds{};
    LOGFONTW originalFont{};
    int originalFontSize = 0;
    std::uint32_t originalColor = 0;
    std::uint32_t color = 0;
    int logicalFontSize = 0;
    LOGFONTW actualFont{};
    UniqueGdiObject<HFONT> font;
    std::unique_ptr<DibSection> background;
    std::unique_ptr<DibSection> paintBuffer;
    UniqueGdiObject<HBRUSH> backgroundBrush;
};

TextInput::TextInput(AnnotationModel& model, EditorRenderer& renderer, HWND parent, UINT commitMessage,
                     UINT layoutMessage)
    : state_(std::make_unique<State>(model, renderer, parent, commitMessage, layoutMessage)) {
    if (parent == nullptr || commitMessage < WM_APP || commitMessage > 0xBFFF ||
        (layoutMessage != 0 && (layoutMessage < WM_APP || layoutMessage > 0xBFFF || layoutMessage == commitMessage))) {
        throw std::invalid_argument("TextInput requires a parent and distinct WM_APP messages");
    }
}

TextInput::~TextInput() = default;
bool TextInput::Active() const noexcept {
    return state_->edit != nullptr;
}
HWND TextInput::Handle() const noexcept {
    return state_->edit;
}

std::optional<std::size_t> TextInput::EditingIndex() const noexcept {
    return state_->editingIndex;
}

void TextInput::State::Open(RECT maximum, RECT initial, const LOGFONTW& selectedFont, std::uint32_t selectedColor,
                            unsigned selectedDpi, int selectedFontSize, const std::wstring& initialText) {
    maximumBounds = maximum;
    actualFont = selectedFont;
    color = selectedColor;
    dpi = selectedDpi;
    logicalFontSize = selectedFontSize;
    try {
        font.Reset(CreateFontIndirectW(&actualFont));
        if (font.Get() == nullptr) { TextError("Create text input font"); }
        {
            const MemoryDc context;
            const SelectObjectGuard selected(context.Get(), font.Get());
            TEXTMETRICW metrics{};
            if (!GetTextMetricsW(context.Get(), &metrics)) { TextError("Read native font metrics"); }
            lineHeight = std::max(1L, metrics.tmHeight);
        }
        if (initial.bottom <= initial.top) {
            initial.bottom = std::min(maximumBounds.bottom, initial.top + lineHeight * 2);
        }
        SetBackground(initial);
        POINT client{bounds.left, bounds.top};
        if (!ScreenToClient(parent, &client)) { TextError("Map text input origin"); }
        const LONG width = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
        edit = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL | ES_AUTOVSCROLL,
                               client.x, client.y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (edit == nullptr) { TextError("Create native text input"); }
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
        SendMessageW(edit, EM_SETLIMITTEXT, 65535, 0);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        RECT formatting{0, 0, width, height};
        SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
        const int tabDialogUnits = 32; // Native EDIT default: eight average character widths.
        SendMessageW(edit, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabDialogUnits));
        SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetLastError(ERROR_SUCCESS);
        originalProcedure = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&State::EditProcedure)));
        if (originalProcedure == nullptr) { TextError("Subclass text input"); }
        editGeneration = NextEditGeneration();
        if (initialText.size() > 65535) { throw std::length_error("Native text exceeds limit"); }
        if (!initialText.empty() &&
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(initialText.c_str())) == FALSE) {
            TextError("Restore native text input content");
        }
        if (IsWindowVisible(parent)) { SetFocus(edit); }
        InvalidateBounds(originalBounds);
        InvalidateRect(edit, nullptr, FALSE);
        RefreshDisplay();
    } catch (...) {
        DestroyEdit();
        throw;
    }
}

void TextInput::Begin(POINT screenPoint, unsigned dpi) {
    if (Active()) {
        Commit();
        if (Active()) { return; }
    }
    const auto selection = state_->model.Selection();
    if (!PtInRect(&selection, screenPoint)) { return; }
    const auto& style = state_->model.Style();
    const LONG desiredWidth = static_cast<LONG>(AnnotationPixels(400, dpi));
    const LONG minimumWidth = static_cast<LONG>(AnnotationPixels(64, dpi));
    const LONG selectionWidth = selection.right - selection.left;
    const LONG availableRight = selection.right - screenPoint.x;
    LONG left = screenPoint.x;
    if (availableRight < minimumWidth && selectionWidth > availableRight) {
        left = std::max(selection.left, selection.right - std::min(selectionWidth, minimumWidth));
    }
    const LONG width = std::min(selection.right - left, desiredWidth);
    const LONG minimumHeight = static_cast<LONG>(AnnotationPixels(style.fontSize, dpi)) * 2;
    const LONG selectionHeight = selection.bottom - selection.top;
    const LONG availableBottom = selection.bottom - screenPoint.y;
    LONG top = screenPoint.y;
    if (availableBottom < minimumHeight && selectionHeight > availableBottom) {
        top = std::max(selection.top, selection.bottom - std::min(selectionHeight, minimumHeight));
    }
    const RECT maximum{left, top, left + width, selection.bottom};
    LOGFONTW font{};
    font.lfHeight = -AnnotationPixels(style.fontSize, dpi);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    // Grayscale antialiasing avoids ClearType RGB fringes in captured/exported pixels.
    font.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(font.lfFaceName, style.fontName.c_str());
    state_->Open(maximum, {maximum.left, maximum.top, maximum.right, maximum.top}, font, style.color, dpi,
                 style.fontSize, L"");
}

void TextInput::BeginEdit(std::size_t annotationIndex) {
    if (Active()) {
        Commit();
        if (Active()) { return; }
    }
    const auto annotations = state_->model.Committed();
    if (annotationIndex >= annotations.size() || annotations[annotationIndex].tool != AnnotationTool::Text) { return; }
    const auto& annotation = annotations[annotationIndex];
    state_->editingIndex = annotationIndex;
    state_->originalBounds = annotation.bounds;
    state_->originalText = annotation.text;
    // Older programmatic callers may only have a measured line layout.
    if (state_->originalText.empty()) {
        for (std::size_t lineIndex = 0; lineIndex < annotation.textLines.size(); ++lineIndex) {
            if (lineIndex != 0) { state_->originalText += L"\r\n"; }
            state_->originalText += annotation.textLines[lineIndex].text;
        }
    }
    state_->originalFont = annotation.font;
    state_->originalFontSize =
        annotation.fontSize > 0
            ? annotation.fontSize
            : std::max(1, MulDiv(std::abs(annotation.font.lfHeight), 96, static_cast<int>(annotation.dpi)));
    state_->originalColor = annotation.color;
    const auto selection = state_->model.Selection();
    auto maximum = annotation.bounds;
    maximum.bottom = std::max(maximum.bottom, selection.bottom);
    maximum.bottom = std::min(maximum.bottom, state_->model.DesktopBounds().bottom);
    state_->renderer.SetHiddenAnnotation(annotationIndex);
    state_->Open(maximum, annotation.bounds, annotation.font, annotation.color, annotation.dpi,
                 state_->originalFontSize, state_->originalText);
}

void TextInput::ApplyStyle(const AnnotationStyle& style) {
    if (!Active()) { return; }
    if (style.color > 0xFFFFFF || style.fontName.empty() || style.fontName.size() >= LF_FACESIZE) {
        throw std::invalid_argument("Invalid text style");
    }
    auto desiredFont = state_->actualFont;
    desiredFont.lfHeight = -AnnotationPixels(style.fontSize, state_->dpi);
    wcscpy_s(desiredFont.lfFaceName, style.fontName.c_str());
    if (std::memcmp(&desiredFont, &state_->actualFont, sizeof(LOGFONTW)) != 0) {
        UniqueGdiObject<HFONT> font(CreateFontIndirectW(&desiredFont));
        if (!font.Get()) { TextError("Create updated text font"); }
        LONG lineHeight = 1;
        {
            const MemoryDc context;
            const SelectObjectGuard selected(context.Get(), font.Get());
            TEXTMETRICW metrics{};
            if (!GetTextMetricsW(context.Get(), &metrics)) { TextError("Read updated text metrics"); }
            lineHeight = std::max(1L, metrics.tmHeight);
        }
        SendMessageW(state_->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
        state_->font = std::move(font);
        state_->actualFont = desiredFont;
        state_->lineHeight = lineHeight;
        state_->FitText();
    }
    state_->logicalFontSize = style.fontSize;
    state_->color = style.color;
    InvalidateRect(state_->edit, nullptr, FALSE);
    state_->RefreshDisplay();
}

void TextInput::SetViewportHeightLimit(LONG maximumHeight) {
    if (maximumHeight <= 0) { throw std::invalid_argument("Text viewport height must be positive"); }
    if (!Active()) { return; }
    state_->viewportHeightLimit =
        state_->viewportHeightLimit > 0 ? std::min(state_->viewportHeightLimit, maximumHeight) : maximumHeight;
    state_->ResizeViewport();
    state_->InvalidateBounds(state_->bounds);
    SendMessageW(state_->edit, EM_SCROLLCARET, 0, 0);
    state_->RefreshDisplay();
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
    RECT dirty{};
    UnionRect(&dirty, &state_->bounds, &state_->originalBounds);
    try {
        const auto rawText = state_->ReadText();
        if (state_->editingIndex && rawText == state_->originalText && state_->color == state_->originalColor &&
            state_->logicalFontSize == state_->originalFontSize &&
            std::memcmp(&state_->actualFont, &state_->originalFont, sizeof(LOGFONTW)) == 0) {
            state_->DestroyEdit();
            if (restoreFocus && GetForegroundWindow() == state_->parent) { SetFocus(state_->parent); }
            return dirty;
        }
        state_->RestoreFullLayout();
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
        RECT modelDirty{};
        if (state_->editingIndex) {
            modelDirty =
                state_->model.ReplaceText(*state_->editingIndex, state_->bounds, state_->actualFont, state_->color,
                                          std::move(lines), rawText, state_->dpi, state_->logicalFontSize);
        } else {
            modelDirty = state_->model.AddText(state_->bounds, state_->actualFont, state_->color, std::move(lines),
                                               rawText, state_->dpi, state_->logicalFontSize);
        }
        UnionRect(&dirty, &dirty, &modelDirty);
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
    if (state_->suppressDirectPaint && !state_->painting && !state_->printing) {
        IntersectClipRect(deviceContext, 0, 0, 0, 0);
    }
    return state_->backgroundBrush.Get();
}

} // namespace vispick
