#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vispick {

struct Hotkey {
    unsigned modifiers = 0; // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN; no MOD_NOREPEAT.
    unsigned virtualKey = 0;
    [[nodiscard]] bool Enabled() const noexcept { return virtualKey != 0; }
    bool operator==(const Hotkey&) const = default;
};

struct HotkeyParseResult {
    std::optional<Hotkey> value;
    std::wstring error;
};

// Pure parsing: does not register a hotkey or consult the active keyboard layout.
// Punctuation uses Windows OEM virtual keys; Ctrl++ / Ctrl+Plus denote VK_OEM_PLUS.
// Ctrl+ is invalid because it has no main key.
[[nodiscard]] HotkeyParseResult ParseHotkey(std::wstring_view text);

} // namespace vispick
