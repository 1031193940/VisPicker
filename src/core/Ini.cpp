#include "core/Ini.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <system_error>
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

void UpdateIniValue(const std::filesystem::path& path, const std::wstring_view sectionName,
    const std::wstring_view keyName, const std::wstring_view value) {
    if (sectionName.size() + keyName.size() + value.size() > 1024 * 1024 ||
        sectionName.find_first_of(L"\r\n") != std::wstring_view::npos ||
        keyName.find_first_of(L"\r\n") != std::wstring_view::npos ||
        value.find_first_of(L"\r\n") != std::wstring_view::npos) {
        throw std::invalid_argument("INI update exceeds a single bounded entry");
    }
    // Validate a complete synthetic entry before touching disk. This catches newlines,
    // embedded NULs, comment injection and whitespace that the parser would discard.
    const std::wstring proposed = L"[" + std::wstring(sectionName) + L"]\n" + std::wstring(keyName) + L"=" + std::wstring(value);
    std::vector<std::uint8_t> proposedBytes{0xFF, 0xFE};
    for (const wchar_t character : proposed) {
        proposedBytes.push_back(static_cast<std::uint8_t>(character & 255));
        proposedBytes.push_back(static_cast<std::uint8_t>(character >> 8));
    }
    const auto parsed = ParseIni(proposedBytes);
    const auto normalizedSection = LowerAscii(sectionName);
    const auto normalizedKey = LowerAscii(keyName);
    if (!parsed.valid || parsed.sections.size() != 1 || parsed.entries.size() != 1 ||
        parsed.sections.front().name != normalizedSection || parsed.entries.front().key != normalizedKey ||
        parsed.entries.front().value != value || sectionName.empty() || keyName.empty()) {
        throw std::invalid_argument("INI update requires one unambiguous section, key and value");
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = input ? static_cast<std::streamoff>(input.tellg()) : -1;
    if (size < 0 || size > 1024 * 1024) { throw std::runtime_error("Cannot read configuration to save configuration value"); }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (size && !input.read(reinterpret_cast<char*>(bytes.data()), size)) { throw std::runtime_error("Incomplete configuration read"); }
    input.close();
    const auto document = ParseIni(bytes, CP_ACP);
    if (!document.valid || document.sections.empty()) { throw std::runtime_error("Cannot modify an empty or malformed configuration"); }

    const bool utf16 = bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF));
    const bool littleEndian = utf16 && bytes[0] == 0xFF;
    const bool utf8Bom = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
    const std::size_t stride = utf16 ? 2 : 1;
    const std::size_t firstByte = utf16 ? 2 : utf8Bom ? 3 : 0;
    const UINT codePage = !utf16 && !utf8Bom && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0) ? CP_ACP : CP_UTF8;
    const auto encode = [&](const std::wstring_view text) {
        std::vector<std::uint8_t> result;
        if (utf16) {
            for (const wchar_t character : text) {
                result.push_back(static_cast<std::uint8_t>(littleEndian ? character & 255 : character >> 8));
                result.push_back(static_cast<std::uint8_t>(littleEndian ? character >> 8 : character & 255));
            }
        } else if (!text.empty()) {
            BOOL substituted = FALSE;
            BOOL* substitution = codePage == CP_UTF8 ? nullptr : &substituted;
            const DWORD flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
            const int count = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, substitution);
            if (!count || substituted) { throw std::runtime_error("Cannot encode configuration value"); }
            result.resize(static_cast<std::size_t>(count));
            if (WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
                reinterpret_cast<char*>(result.data()), count, nullptr, substitution) != count || substituted) {
                throw std::runtime_error("Cannot encode configuration value");
            }
        }
        return result;
    };
    const auto characterAt = [&](const std::size_t offset) -> unsigned {
        if (!utf16) { return bytes[offset]; }
        return littleEndian ? bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8) :
            (static_cast<unsigned>(bytes[offset]) << 8) | bytes[offset + 1];
    };
    struct Line { std::size_t start; std::size_t end; std::size_t next; };
    std::vector<Line> lines;
    std::wstring newline = L"\r\n";
    bool foundNewline = false;
    for (std::size_t start = firstByte; start < bytes.size();) {
        std::size_t end = start;
        while (end < bytes.size() && characterAt(end) != '\r' && characterAt(end) != '\n') { end += stride; }
        std::size_t next = end;
        if (next < bytes.size()) {
            const auto first = characterAt(next); next += stride;
            if (first == '\r' && next < bytes.size() && characterAt(next) == '\n') { next += stride; }
            if (!foundNewline) { newline = first == '\n' ? L"\n" : next - end == stride ? L"\r" : L"\r\n"; foundNewline = true; }
        }
        lines.push_back({start, end, next}); start = next;
    }
    std::size_t replaceStart = bytes.size(), replaceEnd = bytes.size();
    std::wstring replacement;
    const auto entry = std::find_if(document.entries.rbegin(), document.entries.rend(), [&](const IniEntry& item) {
        return item.section == normalizedSection && item.key == normalizedKey;
    });
    if (entry != document.entries.rend()) {
        const auto line = lines.at(entry->line - 1);
        replaceStart = line.start;
        while (replaceStart < line.end && characterAt(replaceStart) != '=') { replaceStart += stride; }
        replaceStart += stride;
        while (replaceStart < line.end && IsSpace(static_cast<wchar_t>(characterAt(replaceStart)))) { replaceStart += stride; }
        const auto oldValue = encode(entry->value);
        // Locate the parsed value in its original bytes so spacing, casing and
        // comments survive without transcoding the rest of the INI document.
        while (replaceStart + oldValue.size() <= line.end &&
            !std::equal(oldValue.begin(), oldValue.end(), bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart))) { replaceStart += stride; }
        if (replaceStart + oldValue.size() > line.end) { throw std::runtime_error("Cannot locate the existing configuration value"); }
        replaceEnd = replaceStart + oldValue.size();
        replacement = value;
        if (oldValue.empty() && replaceEnd < line.end && characterAt(replaceEnd) == ';') {
            replacement += L' '; // Preserve the whitespace that makes this an inline comment.
        }
    } else {
        const auto section = std::find_if(document.sections.rbegin(), document.sections.rend(), [&](const IniSection& item) { return item.name == normalizedSection; });
        if (section != document.sections.rend()) {
            const auto line = lines.at(section->line - 1);
            replaceStart = replaceEnd = line.next;
            if (line.end == line.next) { replacement += newline; }
        } else {
            if (bytes.size() > firstByte && characterAt(bytes.size() - stride) != '\r' && characterAt(bytes.size() - stride) != '\n') { replacement += newline; }
            replacement += L"[" + std::wstring(sectionName) + L"]" + newline;
        }
        replacement += std::wstring(keyName) + L"=" + std::wstring(value);
        replacement += newline;
    }
    const auto inserted = encode(replacement);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart), bytes.begin() + static_cast<std::ptrdiff_t>(replaceEnd));
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart), inserted.begin(), inserted.end());
    if (bytes.size() > 1024 * 1024) { throw std::length_error("Updated configuration exceeds 1 MiB"); }
    const auto updated = ParseIni(bytes, CP_ACP);
    const auto updatedEntry = std::find_if(updated.entries.rbegin(), updated.entries.rend(), [&](const IniEntry& item) {
        return item.section == normalizedSection && item.key == normalizedKey;
    });
    if (!updated.valid || updatedEntry == updated.entries.rend() || updatedEntry->value != value) {
        throw std::invalid_argument("INI value is ambiguous in the existing line format");
    }

    struct StagedFile {
        std::filesystem::path path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~StagedFile() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } if (!path.empty()) { DeleteFileW(path.c_str()); } }
    } staged;
    static std::atomic<unsigned> serial{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        staged.path = path.native() + L".save-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(serial.fetch_add(1));
        staged.handle = CreateFileW(staged.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (staged.handle != INVALID_HANDLE_VALUE) { break; }
        const DWORD error = GetLastError();
        staged.path.clear(); // Never remove an existing file whose name collided.
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) { throw std::system_error(static_cast<int>(error), std::system_category(), "Stage configuration value"); }
    }
    if (staged.handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot stage configuration value"); }
    DWORD written = 0;
    if (!WriteFile(staged.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() || !FlushFileBuffers(staged.handle)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Write configuration value");
    }
    CloseHandle(staged.handle); staged.handle = INVALID_HANDLE_VALUE;
    if (!ReplaceFileW(path.c_str(), staged.path.c_str(), nullptr, 0, nullptr, nullptr)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Save configuration value");
    }
    staged.path.clear();
}

} // namespace vispick
