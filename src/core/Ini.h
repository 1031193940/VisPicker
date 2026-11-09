#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vispick {

struct ConfigDiagnostic {
    std::size_t line = 0; // 0 means a file-level I/O or encoding error.
    std::wstring message;
};

struct IniEntry {
    std::wstring section; // ASCII lowercase; values retain their original case.
    std::wstring key;
    std::wstring value;
    std::size_t line = 0;
};

struct IniSection {
    std::wstring name;
    std::size_t line = 0;
};

struct IniDocument {
    std::vector<IniEntry> entries;
    std::vector<IniSection> sections;
    std::vector<ConfigDiagnostic> diagnostics;
    bool valid = true;
};

// BOMs select their encoding. An optional legacy code page is used only for unmarked input.
[[nodiscard]] IniDocument ParseIni(std::span<const std::uint8_t> bytes,
    std::optional<unsigned> legacyCodePage = std::nullopt);
[[nodiscard]] std::wstring LowerAscii(std::wstring_view value);
[[nodiscard]] std::wstring_view TrimIniWhitespace(std::wstring_view value) noexcept;

} // namespace vispick
