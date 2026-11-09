#include "core/Ini.h"

#include <windows.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick {
namespace {

std::wstring Decode(std::span<const std::uint8_t> bytes, std::optional<unsigned> legacyCodePage) {
    std::wstring text;
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
        (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        const bool littleEndian = bytes[0] == 0xFF;
        bytes = bytes.subspan(2);
        if (bytes.size() % 2 != 0) { throw std::invalid_argument("Odd UTF-16 byte count"); }
        text.reserve(bytes.size() / 2);
        for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
            const unsigned first = bytes[offset];
            const unsigned second = bytes[offset + 1];
            text.push_back(static_cast<wchar_t>(littleEndian ? first | (second << 8) : (first << 8) | second));
        }
    } else {
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            bytes = bytes.subspan(3);
            legacyCodePage.reset();
        }
        if (bytes.empty()) { return {}; }
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Configuration too large");
        }
        const auto* data = reinterpret_cast<const char*>(bytes.data());
        const int length = static_cast<int>(bytes.size());
        unsigned codePage = CP_UTF8;
        int required = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
        if (required == 0 && legacyCodePage) {
            codePage = *legacyCodePage;
            required = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
        }
        if (required == 0) { throw std::invalid_argument("Invalid INI encoding"); }
        text.resize(static_cast<std::size_t>(required));
        if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, data, length, text.data(), required) != required) {
            throw std::invalid_argument("Invalid INI encoding");
        }
    }
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const wchar_t character = text[offset];
        if ((character < 32 && character != L'\t' && character != L'\r' && character != L'\n') ||
            character == 0xFFFE || character == 0xFFFF) {
            throw std::invalid_argument("Invalid text character");
        }
        if (character >= 0xD800 && character <= 0xDBFF) {
            if (++offset == text.size() || text[offset] < 0xDC00 || text[offset] > 0xDFFF) {
                throw std::invalid_argument("Invalid UTF-16 surrogate pair");
            }
        } else if (character >= 0xDC00 && character <= 0xDFFF) {
            throw std::invalid_argument("Unpaired UTF-16 surrogate");
        }
    }
    return text;
}

bool IsSpace(wchar_t character) noexcept {
    return character == L' ' || character == L'\t' || character == L'\u3000' || character == L'\u00A0';
}

} // namespace

std::wstring LowerAscii(std::wstring_view value) {
    std::wstring result(value);
    for (auto& character : result) {
        if (character >= L'A' && character <= L'Z') { character += L'a' - L'A'; }
    }
    return result;
}

std::wstring_view TrimIniWhitespace(std::wstring_view value) noexcept {
    while (!value.empty() && IsSpace(value.front())) { value.remove_prefix(1); }
    while (!value.empty() && IsSpace(value.back())) { value.remove_suffix(1); }
    return value;
}

IniDocument ParseIni(std::span<const std::uint8_t> bytes, std::optional<unsigned> legacyCodePage) {
    IniDocument result;
    std::wstring text;
    try { text = Decode(bytes, legacyCodePage); }
    catch (const std::invalid_argument&) {
        result.valid = false;
        result.diagnostics.push_back({0, L"文件编码无效；请保存为 UTF-8 或带 BOM 的 UTF-16。"});
        return result;
    }
    std::wstring section;
    std::size_t offset = 0;
    std::size_t lineNumber = 0;
    while (offset < text.size()) {
        ++lineNumber;
        const auto end = text.find_first_of(L"\r\n", offset);
        auto line = std::wstring_view(text).substr(offset, end == std::wstring::npos ? text.size() - offset : end - offset);
        offset = end == std::wstring::npos ? text.size() : end + 1;
        if (end != std::wstring::npos && text[end] == L'\r' && offset < text.size() && text[offset] == L'\n') { ++offset; }
        line = TrimIniWhitespace(line);
        if (line.empty() || line.front() == L';') { continue; }
        // A semicolon is a comment only when preceded by whitespace. '#RRGGBB' is data.
        for (std::size_t character = 1; character < line.size(); ++character) {
            if (line[character] == L';' && IsSpace(line[character - 1])) {
                line = TrimIniWhitespace(line.substr(0, character));
                break;
            }
        }
        bool validLine = true;
        if (line.front() == L'[') {
            if (line.size() < 3 || line.back() != L']' || line.find(L']') != line.size() - 1) {
                validLine = false;
            } else {
                const auto name = TrimIniWhitespace(line.substr(1, line.size() - 2));
                validLine = !name.empty() && name.find(L'[') == std::wstring_view::npos;
                if (validLine) {
                    section = LowerAscii(name);
                    result.sections.push_back({section, lineNumber});
                }
            }
        } else {
            const auto equals = line.find(L'=');
            if (section.empty() || equals == std::wstring_view::npos || TrimIniWhitespace(line.substr(0, equals)).empty()) {
                validLine = false;
            } else {
                const auto key = TrimIniWhitespace(line.substr(0, equals));
                if (key.find_first_of(L"[]") != std::wstring_view::npos) { validLine = false; }
                else { result.entries.push_back({section, LowerAscii(key), std::wstring(TrimIniWhitespace(line.substr(equals + 1))), lineNumber}); }
            }
        }
        if (!validLine) {
            result.valid = false;
            result.diagnostics.push_back({lineNumber, L"INI 语法无效；需要 [节名] 或 节内的 键 = 值。"});
        }
    }
    return result;
}

} // namespace vispick
