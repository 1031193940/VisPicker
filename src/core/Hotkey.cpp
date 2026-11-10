#include "core/Hotkey.h"
#include "core/Ini.h"

#include <windows.h>
#include <array>

namespace vispick {
namespace {

struct NamedKey {
    std::wstring_view name;
    unsigned code;
};
constexpr NamedKey NamedKeys[] = {{L"printscreen", VK_SNAPSHOT},
                                  {L"left", VK_LEFT},
                                  {L"right", VK_RIGHT},
                                  {L"up", VK_UP},
                                  {L"down", VK_DOWN},
                                  {L"home", VK_HOME},
                                  {L"end", VK_END},
                                  {L"pageup", VK_PRIOR},
                                  {L"pagedown", VK_NEXT},
                                  {L"insert", VK_INSERT},
                                  {L"delete", VK_DELETE},
                                  {L"space", VK_SPACE},
                                  {L"tab", VK_TAB},
                                  {L";", VK_OEM_1},
                                  {L"semicolon", VK_OEM_1},
                                  {L"=", VK_OEM_PLUS},
                                  {L"+", VK_OEM_PLUS},
                                  {L"plus", VK_OEM_PLUS},
                                  {L"comma", VK_OEM_COMMA},
                                  {L",", VK_OEM_COMMA},
                                  {L"minus", VK_OEM_MINUS},
                                  {L"-", VK_OEM_MINUS},
                                  {L"period", VK_OEM_PERIOD},
                                  {L".", VK_OEM_PERIOD},
                                  {L"slash", VK_OEM_2},
                                  {L"/", VK_OEM_2},
                                  {L"backtick", VK_OEM_3},
                                  {L"`", VK_OEM_3},
                                  {L"[", VK_OEM_4},
                                  {L"leftbracket", VK_OEM_4},
                                  {L"\\", VK_OEM_5},
                                  {L"backslash", VK_OEM_5},
                                  {L"]", VK_OEM_6},
                                  {L"rightbracket", VK_OEM_6},
                                  {L"'", VK_OEM_7},
                                  {L"quote", VK_OEM_7}};

unsigned KeyCode(std::wstring_view key) {
    if (key.size() == 1) {
        const wchar_t character = key.front();
        if (character >= L'a' && character <= L'z') { return static_cast<unsigned>(character - L'a' + L'A'); }
        if (character >= L'0' && character <= L'9') { return static_cast<unsigned>(character); }
    }
    if (key.size() >= 2 && key.size() <= 3 && key.front() == L'f') {
        unsigned number = 0;
        for (const wchar_t character : key.substr(1)) {
            if (character < L'0' || character > L'9') { return 0; }
            number = number * 10 + static_cast<unsigned>(character - L'0');
        }
        if (number >= 1 && number <= 24 && key[1] != L'0') { return VK_F1 + number - 1; }
    }
    if (key.size() == 4 && key.substr(0, 3) == L"num" && key[3] >= L'0' && key[3] <= L'9') {
        return VK_NUMPAD0 + static_cast<unsigned>(key[3] - L'0');
    }
    for (const auto& named : NamedKeys) {
        if (key == named.name) { return named.code; }
    }
    return 0;
}

} // namespace

HotkeyParseResult ParseHotkey(std::wstring_view text) {
    const auto normalized = LowerAscii(TrimIniWhitespace(text));
    if (normalized.empty()) { return {Hotkey{}, {}}; }
    Hotkey hotkey;
    std::wstring_view remaining(normalized);
    while (!remaining.empty()) {
        const auto separator = remaining.find(L'+');
        // A literal plus is unambiguous only as the complete remaining main key.
        if (remaining == L"+") {
            hotkey.virtualKey = VK_OEM_PLUS;
            break;
        }
        const auto token = TrimIniWhitespace(remaining.substr(0, separator));
        unsigned modifier = 0;
        if (token == L"ctrl") {
            modifier = MOD_CONTROL;
        } else if (token == L"alt") {
            modifier = MOD_ALT;
        } else if (token == L"shift") {
            modifier = MOD_SHIFT;
        } else if (token == L"win") {
            modifier = MOD_WIN;
        }
        if (modifier != 0) {
            if ((hotkey.modifiers & modifier) != 0) { return {std::nullopt, L"热键修饰键不能重复。"}; }
            hotkey.modifiers |= modifier;
            if (separator == std::wstring_view::npos) { return {std::nullopt, L"热键缺少主键。"}; }
        } else {
            if (separator != std::wstring_view::npos || (hotkey.virtualKey = KeyCode(token)) == 0) {
                return {std::nullopt, L"未知热键；修饰键须放在一个受支持的主键之前。"};
            }
            break;
        }
        remaining = TrimIniWhitespace(remaining.substr(separator + 1));
    }
    if (!hotkey.Enabled()) { return {std::nullopt, L"热键缺少主键。"}; }
    return {hotkey, {}};
}

std::wstring FormatHotkey(Hotkey hotkey) {
    if (!hotkey.Enabled()) { return {}; }
    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) { text += L"Ctrl+"; }
    if (hotkey.modifiers & MOD_ALT) { text += L"Alt+"; }
    if (hotkey.modifiers & MOD_SHIFT) { text += L"Shift+"; }
    if (hotkey.modifiers & MOD_WIN) { text += L"Win+"; }
    const auto key = hotkey.virtualKey;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) { return text + static_cast<wchar_t>(key); }
    if (key >= VK_F1 && key <= VK_F24) { return text + L"F" + std::to_wstring(key - VK_F1 + 1); }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) { return text + L"Num" + std::to_wstring(key - VK_NUMPAD0); }
    for (const auto& named : NamedKeys) {
        if (key == named.code) { return text.append(named.name); }
    }
    return text + L"VK " + std::to_wstring(key);
}

} // namespace vispick
