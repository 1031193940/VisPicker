#include <doctest/doctest.h>
#include "core/Hotkey.h"
#include <windows.h>

namespace {
vispick::Hotkey ParseValid(std::wstring_view text) {
    const auto result = vispick::ParseHotkey(text);
    REQUIRE(result.value.has_value());
    return *result.value;
}
} // namespace

TEST_CASE("Hotkey empty input disables registration") {
    CHECK_FALSE(ParseValid(L"").Enabled());
    CHECK_FALSE(ParseValid(L"  \t").Enabled());
}
TEST_CASE("Hotkey defaults use Control and digits") {
    CHECK(ParseValid(L"Ctrl+1") == vispick::Hotkey{MOD_CONTROL, '1'});
}
TEST_CASE("Hotkey diagnostics format the configured chord") {
    CHECK(vispick::FormatHotkey(ParseValid(L"Ctrl+1")) == L"Ctrl+1");
    CHECK(vispick::FormatHotkey(ParseValid(L"Alt+Ctrl+F17")) == L"Ctrl+Alt+F17");
    for (const auto* chord : {L"", L"Win+Shift+Num1", L"Alt+PrintScreen", L"Ctrl++"}) {
        const auto hotkey = ParseValid(chord);
        CHECK(ParseValid(vispick::FormatHotkey(hotkey)) == hotkey);
    }
}
TEST_CASE("Hotkey modifiers are case insensitive and order independent") {
    CHECK(ParseValid(L"alt + WIN + shift + cTrL + a") ==
          vispick::Hotkey{MOD_ALT | MOD_WIN | MOD_SHIFT | MOD_CONTROL, 'A'});
}
TEST_CASE("Hotkey accepts all letters") {
    for (wchar_t key = L'A'; key <= L'Z'; ++key) {
        CHECK(ParseValid(std::wstring(1, key)).virtualKey == static_cast<unsigned>(key));
    }
}
TEST_CASE("Hotkey accepts all top row digits") {
    for (wchar_t key = L'0'; key <= L'9'; ++key) {
        CHECK(ParseValid(std::wstring(1, key)).virtualKey == static_cast<unsigned>(key));
    }
}
TEST_CASE("Hotkey accepts all function keys F1 through F24") {
    for (unsigned key = 1; key <= 24; ++key) {
        CHECK(ParseValid(L"F" + std::to_wstring(key)).virtualKey == VK_F1 + key - 1);
    }
}
TEST_CASE("Hotkey rejects function key overflow and malformed digits") {
    for (const auto* key : {L"F0", L"F25", L"F01", L"F99999999999", L"F1x"}) {
        CHECK_FALSE(vispick::ParseHotkey(key).value.has_value());
    }
}
TEST_CASE("Hotkey distinguishes numeric keypad from top row") {
    for (unsigned key = 0; key <= 9; ++key) {
        CHECK(ParseValid(L"Num" + std::to_wstring(key)).virtualKey == VK_NUMPAD0 + key);
    }
    CHECK_FALSE(vispick::ParseHotkey(L"Num10").value.has_value());
}
TEST_CASE("Hotkey supports PrintScreen") {
    CHECK(ParseValid(L"PrintScreen").virtualKey == VK_SNAPSHOT);
}
TEST_CASE("Hotkey supports navigation keys") {
    CHECK(ParseValid(L"Left").virtualKey == VK_LEFT);
    CHECK(ParseValid(L"Right").virtualKey == VK_RIGHT);
    CHECK(ParseValid(L"Up").virtualKey == VK_UP);
    CHECK(ParseValid(L"Down").virtualKey == VK_DOWN);
    CHECK(ParseValid(L"Home").virtualKey == VK_HOME);
    CHECK(ParseValid(L"End").virtualKey == VK_END);
}
TEST_CASE("Hotkey supports page editing space and tab keys") {
    CHECK(ParseValid(L"PageUp").virtualKey == VK_PRIOR);
    CHECK(ParseValid(L"PageDown").virtualKey == VK_NEXT);
    CHECK(ParseValid(L"Insert").virtualKey == VK_INSERT);
    CHECK(ParseValid(L"Delete").virtualKey == VK_DELETE);
    CHECK(ParseValid(L"Space").virtualKey == VK_SPACE);
    CHECK(ParseValid(L"Tab").virtualKey == VK_TAB);
}
TEST_CASE("Hotkey rejects Ctrl plus with a missing main key") {
    CHECK_FALSE(vispick::ParseHotkey(L"Ctrl+").value.has_value());
}
TEST_CASE("Hotkey accepts an explicit plus key") {
    CHECK(ParseValid(L"Ctrl++") == vispick::Hotkey{MOD_CONTROL, VK_OEM_PLUS});
    CHECK(ParseValid(L"Ctrl+Plus") == ParseValid(L"Ctrl++"));
    CHECK(ParseValid(L"+").virtualKey == VK_OEM_PLUS);
}
TEST_CASE("Hotkey supports common OEM punctuation without layout calls") {
    CHECK(ParseValid(L";").virtualKey == VK_OEM_1);
    CHECK(ParseValid(L"=").virtualKey == VK_OEM_PLUS);
    CHECK(ParseValid(L",").virtualKey == VK_OEM_COMMA);
    CHECK(ParseValid(L"-").virtualKey == VK_OEM_MINUS);
    CHECK(ParseValid(L".").virtualKey == VK_OEM_PERIOD);
    CHECK(ParseValid(L"/").virtualKey == VK_OEM_2);
    CHECK(ParseValid(L"`").virtualKey == VK_OEM_3);
    CHECK(ParseValid(L"[").virtualKey == VK_OEM_4);
    CHECK(ParseValid(L"\\").virtualKey == VK_OEM_5);
    CHECK(ParseValid(L"]").virtualKey == VK_OEM_6);
    CHECK(ParseValid(L"'").virtualKey == VK_OEM_7);
}
TEST_CASE("Hotkey semicolon alias works in INI comments") {
    CHECK(ParseValid(L"Ctrl+Semicolon") == vispick::Hotkey{MOD_CONTROL, VK_OEM_1});
}
TEST_CASE("Hotkey rejects duplicate modifiers") {
    CHECK_FALSE(vispick::ParseHotkey(L"Ctrl+Ctrl+A").value.has_value());
}
TEST_CASE("Hotkey rejects two main keys or reversed modifiers") {
    CHECK_FALSE(vispick::ParseHotkey(L"A+B").value.has_value());
    CHECK_FALSE(vispick::ParseHotkey(L"A+Ctrl").value.has_value());
}
TEST_CASE("Hotkey rejects unknown names and empty tokens") {
    for (const auto* key : {L"Control+A", L"Mouse1", L"Ctrl++A", L"Ctrl", L"Win+Alt", L"Ctrl+未知"}) {
        CHECK_FALSE(vispick::ParseHotkey(key).value.has_value());
    }
}
