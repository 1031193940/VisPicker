#include "editor/Annotation.h"
#include "editor/Renderer.h"
#include "editor/TextInput.h"
#include "platform/Compat.h"
#include "platform/Gdi.h"
#include "platform/Window.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr UINT CommitMessage = WM_APP + 71;
constexpr UINT LayoutMessage = WM_APP + 72;
constexpr POINT DesktopOrigin{-160, -90};

void Require(bool condition, const char* description) {
    if (!condition) { throw std::runtime_error(description); }
}

class EditorWindow final : public vispick::Window {
public:
    EditorWindow() {
        // No ShowWindow call: the native child is exercised through WM_PRINTCLIENT,
        // and cannot cover, activate or read any existing desktop window.
        Create({L"VisPick.Test.EditorIntegration", 0, IDC_ARROW}, L"VisPick native editor integration",
               WS_POPUP | WS_CLIPCHILDREN, WS_EX_TOOLWINDOW, {-160, -90, 640, 510}, vispick::WindowActivation::Passive);
    }
    ~EditorWindow() override { Destroy(); }
    vispick::TextInput* input = nullptr;
    int commitMessages = 0;
    int layoutMessages = 0;

protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override {
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) {
            if (input != nullptr) {
                if (const auto brush =
                        input->ControlColor(reinterpret_cast<HDC>(parameter), reinterpret_cast<HWND>(data))) {
                    return reinterpret_cast<LRESULT>(brush);
                }
            }
        }
        if (message == CommitMessage) {
            ++commitMessages;
            if (input != nullptr) { input->CommitRequested(parameter); }
            return 0;
        }
        if (message == LayoutMessage) {
            ++layoutMessages;
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
};

vispick::Bitmap Pattern() {
    vispick::Bitmap image(800, 600);
    for (int row = 0; row < image.Height(); ++row) {
        for (int column = 0; column < image.Width(); ++column) {
            const auto red = static_cast<unsigned>((column * 17 + row * 7) & 0xFF);
            const auto green = static_cast<unsigned>((column * 3 + row * 11) & 0xFF);
            const auto blue = static_cast<unsigned>(((column / 11 ^ row / 13) * 29) & 0xFF);
            image.Row(row)[column] = 0xFF000000U | red << 16 | green << 8 | blue;
        }
    }
    return image;
}

class Session {
public:
    Session(const vispick::Bitmap& frozen, unsigned dpi, int fontSize)
        : model(frozen.View(), DesktopOrigin), renderer(model) {
        auto style = model.Style();
        style.fontSize = fontSize;
        style.color = 0xF03040;
        style.fontName = L"Microsoft YaHei";
        model.SetStyle(style);
        model.SetSelection({-140, -70, 600, 490});
        model.SetTool(vispick::AnnotationTool::Text);
        input = std::make_unique<vispick::TextInput>(model, renderer, window.Handle(), CommitMessage, LayoutMessage);
        window.input = input.get();
        input->Begin({-120, -50}, dpi);
        Require(input->Active(), "TextInput did not create native EDIT");
        Require(!IsWindowVisible(window.Handle()) && !IsWindowVisible(input->Handle()),
                "Headless editor test must remain hidden");
    }
    ~Session() {
        window.input = nullptr;
        input.reset();
    }
    vispick::AnnotationModel model;
    vispick::EditorRenderer renderer;
    EditorWindow window;
    std::unique_ptr<vispick::TextInput> input;
};

vispick::Bitmap PrintNativeEdit(HWND edit) {
    RECT client{};
    Require(GetClientRect(edit, &client) != FALSE, "Get native EDIT client rectangle failed");
    vispick::DibSection surface(client.right, client.bottom);
    std::fill(surface.Pixels().begin(), surface.Pixels().end(), 0xFF00FF00);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    HideCaret(edit);
    SendMessageW(edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.DeviceContext()), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    const vispick::BitmapView view(surface.Width(), surface.Height(), surface.Pixels());
    return view.CropOpaque(0, 0, view.Width(), view.Height());
}

struct Comparison {
    std::uint64_t comparedPixels = 0;
    std::uint64_t mismatchedPixels = 0;
    std::uint64_t nativeChangedPixels = 0;
    std::uint64_t committedChangedPixels = 0;
    std::size_t visualLines = 0;
};

Comparison CompareNative(const vispick::Bitmap& frozen, unsigned dpi, int fontSize, const wchar_t* text,
                         bool escapeCommit) {
    Session session(frozen, dpi, fontSize);
    const HWND edit = session.input->Handle();
    Require(SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)) != FALSE,
            "Set native EDIT Unicode text failed");
    RECT bounds{};
    Require(GetWindowRect(edit, &bounds) != FALSE, "Get native EDIT screen bounds failed");
    const auto background = session.renderer.Flatten(bounds);
    const auto native = PrintNativeEdit(edit);
    const auto nativeLines = static_cast<std::size_t>(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
    if (escapeCommit) {
        SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
        vispick::PumpWindowMessages();
        Require(session.window.commitMessages == 1, "Escape did not request exactly one text commit");
        Require(IsWindow(session.window.Handle()) != FALSE, "Escape closed the editor instead of committing text");
    } else {
        session.input->Commit();
    }
    Require(!session.input->Active(), "Text commit did not destroy native EDIT");
    Require(session.model.Committed().size() == 1, "Text commit did not create one annotation");
    const auto& annotation = session.model.Committed().front();
    Require(annotation.textLines.size() == nativeLines, "Committed text lost native EDIT line wrapping");
    Require(annotation.font.lfHeight == -vispick::AnnotationPixels(fontSize, dpi),
            "Text commit changed physical font size");
    const auto committed = session.renderer.Flatten(bounds);
    Require(native.Width() == committed.Width() && native.Height() == committed.Height(),
            "Text commit changed native EDIT output dimensions");
    Comparison comparison;
    comparison.comparedPixels = native.Pixels().size();
    comparison.visualLines = nativeLines;
    for (std::size_t pixel = 0; pixel < native.Pixels().size(); ++pixel) {
        comparison.mismatchedPixels += native.Pixels()[pixel] != committed.Pixels()[pixel];
        if (native.Pixels()[pixel] != committed.Pixels()[pixel] && comparison.mismatchedPixels <= 6) {
            std::cout << "text_difference x=" << pixel % static_cast<std::size_t>(native.Width())
                      << " y=" << pixel / static_cast<std::size_t>(native.Width()) << " native=" << std::hex
                      << native.Pixels()[pixel] << " committed=" << committed.Pixels()[pixel]
                      << " background=" << background.Pixels()[pixel] << std::dec << '\n';
        }
        comparison.nativeChangedPixels += native.Pixels()[pixel] != background.Pixels()[pixel];
        comparison.committedChangedPixels += committed.Pixels()[pixel] != background.Pixels()[pixel];
    }
    Require(comparison.nativeChangedPixels > 0 && comparison.committedChangedPixels > 0,
            "Text pixel comparison contained no actual glyphs");
    return comparison;
}

void CheckDeferredCommits(const vispick::Bitmap& frozen) {
    Session session(frozen, 96, 20);
    const HWND edit = session.input->Handle();
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    session.input->Commit();
    Require(session.input->Active() && !session.model.HasAnnotations(), "Commit closed an unfinished composition");
    Require(PostMessageW(edit, WM_CHAR, L'尾', 1) != FALSE, "Queue synthetic conversion result failed");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    vispick::PumpWindowMessages();
    const auto annotations = session.model.Committed();
    Require(!session.input->Active() && annotations.size() == 1 && !annotations.front().textLines.empty() &&
                annotations.front().textLines.front().text == L"尾",
            "Deferred commit omitted queued conversion text");

    session.input->Begin({-120, -50});
    SendMessageW(session.input->Handle(), WM_KEYDOWN, VK_ESCAPE, 0);
    session.input->Cancel();
    session.input->Begin({-100, -30});
    vispick::PumpWindowMessages();
    Require(session.input->Active() && session.model.Committed().size() == 1,
            "An old EDIT committed the replacement control");
}

std::wstring ReadControlText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    return text;
}

bool SamePixels(const vispick::Bitmap& first, const vispick::Bitmap& second) {
    return first.Width() == second.Width() && first.Height() == second.Height() &&
           std::equal(first.Pixels().begin(), first.Pixels().end(), second.Pixels().begin());
}

void CheckTextReediting(const vispick::Bitmap& frozen) {
    Session session(frozen, 144, 20);
    const std::wstring originalText =
        L"首行\r\nTab\t中文\tAligned\r\nThis long line must remain soft wrapped when its font size changes: "
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 中文软换行不会变为手动换行。";
    SetWindowTextW(session.input->Handle(), originalText.c_str());
    session.input->Commit();
    Require(session.model.Committed().size() == 1, "Initial editable text was not committed");
    const auto originalAnnotation = session.model.Committed().front();
    const auto originalPixels = session.renderer.Flatten(session.model.Selection());
    const auto originalRevision = session.model.Revision();
    Require(originalAnnotation.text == originalText && originalAnnotation.dpi == 144,
            "Text commit did not retain original text and creation DPI");

    session.input->BeginEdit(0);
    Require(session.input->Active() && session.input->EditingIndex() == std::size_t{0},
            "Existing text did not reopen at its original index");
    Require(ReadControlText(session.input->Handle()) == originalText,
            "Reopening text converted soft wraps into hard line breaks");
    const auto hidden = session.renderer.Flatten(originalAnnotation.bounds);
    const auto background = frozen.View().CropOpaque(originalAnnotation.bounds.left - DesktopOrigin.x,
                                                     originalAnnotation.bounds.top - DesktopOrigin.y,
                                                     originalAnnotation.bounds.right - originalAnnotation.bounds.left,
                                                     originalAnnotation.bounds.bottom - originalAnnotation.bounds.top);
    Require(SamePixels(hidden, background), "Original glyphs remained behind the native EDIT");
    session.input->Commit();
    Require(session.model.Revision() == originalRevision && session.model.Committed().size() == 1,
            "Unchanged text created an undo entry or a duplicate annotation");
    Require(SamePixels(originalPixels, session.renderer.Flatten(session.model.Selection())),
            "Reopening unchanged text altered its rendered pixels");

    session.input->BeginEdit(0);
    const std::wstring replacement = originalText + L"\r\n二次编辑后的最后一行";
    SetWindowTextW(session.input->Handle(), replacement.c_str());
    auto style = session.model.Style();
    style.color = 0x1677DD;
    style.fontSize = 28;
    session.input->ApplyStyle(style);
    Require(ReadControlText(session.input->Handle()) == replacement,
            "Changing text size lost its original Unicode content");
    RECT updatedBounds{};
    Require(GetWindowRect(session.input->Handle(), &updatedBounds) != FALSE, "Read resized text bounds failed");
    const auto native = PrintNativeEdit(session.input->Handle());
    session.input->Commit();
    Require(session.model.Committed().size() == 1 && session.model.Revision() == originalRevision + 1,
            "Content, color and font edit must commit as one replacement");
    const auto& updated = session.model.Committed().front();
    Require(updated.text == replacement && updated.color == style.color && updated.dpi == 144 &&
                updated.font.lfHeight == -vispick::AnnotationPixels(28, 144),
            "Edited text lost its content, color or original DPI");
    Require(SamePixels(native, session.renderer.Flatten(updatedBounds)),
            "Reflowed text pixels differ from the native EDIT");
    const auto updatedPixels = session.renderer.Flatten(session.model.Selection());
    session.model.Undo();
    Require(session.model.Committed().size() == 1 && session.model.Committed().front().text == originalText &&
                SamePixels(originalPixels, session.renderer.Flatten(session.model.Selection())),
            "Undo did not restore text content, layout and color together");
    session.model.Redo();
    Require(SamePixels(updatedPixels, session.renderer.Flatten(session.model.Selection())),
            "Redo did not restore edited text pixels");

    const auto beforeCancel = session.model.Revision();
    session.input->BeginEdit(0);
    SetWindowTextW(session.input->Handle(), L"This edit is cancelled");
    session.input->Cancel();
    Require(session.model.Revision() == beforeCancel &&
                SamePixels(updatedPixels, session.renderer.Flatten(session.model.Selection())),
            "Cancel did not restore the hidden original text without history changes");

    session.input->BeginEdit(0);
    SetWindowTextW(session.input->Handle(), L"");
    session.input->Commit();
    Require(session.model.Committed().empty(), "Empty text edit did not remove its annotation");
    session.model.Undo();
    Require(session.model.Committed().size() == 1 && session.model.Committed().front().text == replacement,
            "Undo could not restore deleted text");

    session.input->BeginEdit(0);
    SetWindowTextW(session.input->Handle(), L"转换");
    const int textLength = GetWindowTextLengthW(session.input->Handle());
    SendMessageW(session.input->Handle(), EM_SETSEL, static_cast<WPARAM>(textLength), textLength);
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(session.input->Handle(), EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart),
                 reinterpret_cast<LPARAM>(&selectionEnd));
    Require(selectionStart == static_cast<DWORD>(textLength) && selectionEnd == static_cast<DWORD>(textLength),
            "Re-edit IME fixture did not position the caret after the existing text");
    SendMessageW(session.input->Handle(), WM_IME_STARTCOMPOSITION, 0, 0);
    session.input->Commit();
    Require(session.input->Active() && session.model.Committed().front().text == replacement,
            "Re-edit committed an unfinished IME composition");
    Require(PostMessageW(session.input->Handle(), WM_CHAR, L'尾', 1) != FALSE,
            "Queue re-edit conversion result failed");
    SendMessageW(session.input->Handle(), WM_IME_ENDCOMPOSITION, 0, 0);
    vispick::PumpWindowMessages();
    Require(!session.input->Active() && session.model.Committed().size() == 1 &&
                session.model.Committed().front().text == L"转换尾",
            "Re-edit IME commit lost queued characters or duplicated the annotation");
}

void CheckLiveTextStyle(const vispick::Bitmap& frozen) {
    Session session(frozen, 192, 14);
    const std::wstring text = L"输入过程中调整颜色和字号\r\nLive text style";
    SetWindowTextW(session.input->Handle(), text.c_str());
    auto style = session.model.Style();
    style.color = 0x00AA44;
    style.fontSize = 28;
    session.input->ApplyStyle(style);
    RECT bounds{};
    Require(GetWindowRect(session.input->Handle(), &bounds) != FALSE, "Read live text bounds failed");
    const auto native = PrintNativeEdit(session.input->Handle());
    session.input->Commit();
    const auto& annotation = session.model.Committed().front();
    Require(annotation.text == text && annotation.color == style.color &&
                annotation.font.lfHeight == -vispick::AnnotationPixels(28, 192),
            "Changing the active text style did not update the committed text");
    Require(SamePixels(native, session.renderer.Flatten(bounds)),
            "Active text style pixels differ from committed output");
}

void CheckLimitedTextViewport(const vispick::Bitmap& frozen) {
    const std::wstring originalText =
        L"首行\tTabbed\t中文\r\n第二行 ABCDEFGHIJKLMNOPQRSTUVWXYZ 自动换行 continued text\r\n"
        L"第三行\t需要保留制表符位置\r\n第四行内容\r\n第五行\tTab\tPositions\r\n第六行";
    const std::wstring finalText = originalText + L"\r\n尾";
    Session full(frozen, 96, 20);
    SetWindowTextW(full.input->Handle(), finalText.c_str());
    full.input->Commit();
    const RECT fullBounds = full.model.Committed().front().bounds;
    const auto expected = full.renderer.Flatten(full.model.Selection());
    Require(fullBounds.bottom - fullBounds.top > 96, "Viewport fixture does not contain enough text");

    Session limited(frozen, 96, 20);
    SetWindowTextW(limited.input->Handle(), originalText.c_str());
    vispick::PumpWindowMessages();
    const int layoutMessages = limited.window.layoutMessages;
    limited.input->SetViewportHeightLimit(96);
    Require(limited.window.layoutMessages == layoutMessages, "Text layout notification was synchronous");
    RECT viewport{};
    Require(GetClientRect(limited.input->Handle(), &viewport) && viewport.bottom == 96,
            "Text input did not reserve the requested toolbar space");
    vispick::PumpWindowMessages();
    Require(limited.window.layoutMessages == layoutMessages + 1,
            "Text viewport resize did not post one layout notification");
    limited.input->SetViewportHeightLimit(500);
    vispick::PumpWindowMessages();
    Require(GetClientRect(limited.input->Handle(), &viewport) && viewport.bottom == 96 &&
                limited.window.layoutMessages == layoutMessages + 1,
            "Current text viewport limit expanded or repeatedly posted layout notifications");

    const int length = GetWindowTextLengthW(limited.input->Handle());
    SendMessageW(limited.input->Handle(), EM_SETSEL, static_cast<WPARAM>(length), length);
    SendMessageW(limited.input->Handle(), EM_SCROLLCARET, 0, 0);
    // The hidden parent cannot own a caret; set a real scroll offset explicitly.
    SendMessageW(limited.input->Handle(), EM_LINESCROLL, 0, 4);
    Require(SendMessageW(limited.input->Handle(), EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
            "Limited viewport fixture did not scroll to its lower lines");
    SendMessageW(limited.input->Handle(), WM_CHAR, L'\r', 1);
    SendMessageW(limited.input->Handle(), WM_CHAR, L'尾', 1);
    Require(ReadControlText(limited.input->Handle()) == finalText, "Limited text viewport changed the input contents");
    Require(SendMessageW(limited.input->Handle(), EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
            "Typing in a limited viewport unexpectedly scrolled back to the first line");
    Require(GetClientRect(limited.input->Handle(), &viewport) && viewport.bottom == 96,
            "Growing text expanded beyond its viewport limit");
    limited.input->Commit();
    const auto& committed = limited.model.Committed().front();
    Require(committed.bounds.bottom == fullBounds.bottom && committed.text == finalText,
            "The editing viewport clipped the committed text bounds or contents");
    Require(SamePixels(expected, limited.renderer.Flatten(limited.model.Selection())),
            "Scrolling a limited viewport changed exported line or tab positions");

    const std::wstring replacement = originalText + L"\r\n重编辑\t仍保留 Tab 和所有行";
    full.input->BeginEdit(0);
    SetWindowTextW(full.input->Handle(), replacement.c_str());
    full.input->Commit();
    limited.input->BeginEdit(0);
    Require(GetClientRect(limited.input->Handle(), &viewport) && viewport.bottom > 96,
            "Viewport limit leaked into a new text editing session");
    limited.input->SetViewportHeightLimit(64);
    SetWindowTextW(limited.input->Handle(), replacement.c_str());
    SendMessageW(limited.input->Handle(), EM_LINESCROLL, 0, 4);
    limited.input->Commit();
    Require(
        SamePixels(full.renderer.Flatten(full.model.Selection()), limited.renderer.Flatten(limited.model.Selection())),
        "Re-editing through a limited viewport changed exported pixels");
    limited.model.Undo();
    Require(limited.model.Committed().front().text == finalText &&
                SamePixels(expected, limited.renderer.Flatten(limited.model.Selection())),
            "Undo did not restore the full text after viewport-limited editing");
}

struct Resources {
    DWORD gdi = 0;
    DWORD user = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
};

Resources Measure() {
    GdiFlush();
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)) != FALSE,
            "Get editor process memory counters failed");
    return {GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
            counters.PrivateUsage, counters.WorkingSetSize};
}

} // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    try {
        const bool compareOnly = argumentCount > 1 && std::wstring_view(arguments[1]) == L"--compare-only";
        const int reportArgument = compareOnly ? 2 : 1;
        if (argumentCount > reportArgument + 1) {
            throw std::invalid_argument("Usage: VisPickEditorTests [--compare-only] [report.json]");
        }
        const auto frozen = Pattern();
        CheckDeferredCommits(frozen);
        CheckTextReediting(frozen);
        CheckLiveTextStyle(frozen);
        CheckLimitedTextViewport(frozen);
        constexpr const wchar_t* content =
            L"中文标注：位置与换行一致\r\nTab\tAligned\t测试\r\n自动换行 Mixed English 中文文字 0123456789 "
            L"ABCDEFGHIJKLMNOPQRSTUVWXYZ 自动换行继续，确保输入与提交使用同一字体。";
        std::uint64_t compared = 0;
        std::uint64_t mismatches = 0;
        for (const auto [dpi, size] : {std::pair{96U, 14}, std::pair{144U, 20}, std::pair{192U, 28}}) {
            const auto result = CompareNative(frozen, dpi, size, content, dpi == 144);
            compared += result.comparedPixels;
            mismatches += result.mismatchedPixels;
            std::cout << "native_edit dpi=" << dpi << " font=" << size << " lines=" << result.visualLines
                      << " pixels=" << result.comparedPixels << " mismatches=" << result.mismatchedPixels
                      << " native_glyph_pixels=" << result.nativeChangedPixels
                      << " committed_glyph_pixels=" << result.committedChangedPixels << '\n';
        }
        // Warm the class/font/GDI+ paths before checking ownership, then close every session.
        if (!compareOnly) {
            for (int cycle = 0; cycle < 5; ++cycle) {
                static_cast<void>(CompareNative(frozen, 96, 20, L"预热\r\nWarmup", false));
            }
        }
        const auto before = Measure();
        const int sessions = compareOnly ? 0 : 200;
        for (int cycle = 0; cycle < sessions; ++cycle) {
            Session session(frozen, 96, 20);
            SendMessageW(session.input->Handle(), WM_SETTEXT, 0,
                         reinterpret_cast<LPARAM>(L"资源生命周期\r\nNative EDIT 200 cycles"));
            session.input->Commit();
            static_cast<void>(session.renderer.Flatten({-120, -50, 280, 50}));
        }
        const auto after = Measure();
        const bool resourcesReturned = before.gdi == after.gdi && before.user == after.user;
        const bool passed = mismatches == 0 && resourcesReturned;
        const auto report =
            std::string("{\n  \"passed\": ") + (passed ? "true" : "false") +
            ",\n  \"native_edit_pixels\": " + std::to_string(compared) +
            ",\n  \"mismatched_pixels\": " + std::to_string(mismatches) +
            ",\n  \"sessions\": " + std::to_string(sessions) + ",\n  \"gdi_before\": " + std::to_string(before.gdi) +
            ",\n  \"gdi_after\": " + std::to_string(after.gdi) +
            ",\n  \"user_before\": " + std::to_string(before.user) +
            ",\n  \"user_after\": " + std::to_string(after.user) +
            ",\n  \"private_bytes_before\": " + std::to_string(before.privateBytes) +
            ",\n  \"private_bytes_after\": " + std::to_string(after.privateBytes) +
            ",\n  \"working_set_before\": " + std::to_string(before.workingSetBytes) +
            ",\n  \"working_set_after\": " + std::to_string(after.workingSetBytes) +
            ",\n  \"synthetic_ime_completion_tested\": true,\n  \"real_ime_input_tested\": false,\n  "
            "\"text_reediting_and_style_tested\": true,\n  \"limited_viewport_tested\": true,\n  \"method\": \"hidden native EDIT WM_SETTEXT and WM_PRINTCLIENT; exact RGB comparison; no "
            "external windows or clipboard\"\n}\n";
        std::cout << report;
        if (argumentCount > reportArgument) {
            const std::filesystem::path path(arguments[reportArgument]);
            if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path()); }
            std::ofstream output(path, std::ios::binary);
            output << report;
            Require(static_cast<bool>(output), "Write editor test report failed");
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Editor integration failed: " << error.what() << '\n';
        return 2;
    }
}
