#include <doctest/doctest.h>
#include "core/Ini.h"

#include <string>

namespace {
vispick::IniDocument ParseText(std::string_view text) {
    return vispick::ParseIni({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
std::vector<std::uint8_t> Utf16(std::wstring_view text, bool littleEndian) {
    std::vector<std::uint8_t> bytes = littleEndian ? std::vector<std::uint8_t>{0xFF, 0xFE} : std::vector<std::uint8_t>{0xFE, 0xFF};
    for (const wchar_t character : text) {
        const auto low = static_cast<std::uint8_t>(character & 0xFF);
        const auto high = static_cast<std::uint8_t>(character >> 8);
        bytes.push_back(littleEndian ? low : high);
        bytes.push_back(littleEndian ? high : low);
    }
    return bytes;
}
}

TEST_CASE("INI accepts an empty document") { CHECK(ParseText("").valid); CHECK(ParseText("").entries.empty()); }
TEST_CASE("INI reads UTF8 Chinese without a BOM") {
    const auto result = ParseText("[Editor]\nFontName=微软雅黑\n");
    REQUIRE(result.valid); REQUIRE(result.entries.size() == 1); CHECK(result.entries[0].value == L"微软雅黑");
}
TEST_CASE("INI reads UTF8 BOM") {
    const auto result = ParseText("\xEF\xBB\xBF[Pin]\nOpacity=80");
    REQUIRE(result.valid); CHECK(result.entries[0].key == L"opacity");
}
TEST_CASE("INI reads UTF16 little endian with surrogate pairs") {
    const auto result = vispick::ParseIni(Utf16(L"[Save]\nFileName=截图😀", true));
    REQUIRE(result.valid); CHECK(result.entries[0].value == L"截图😀");
}
TEST_CASE("INI reads UTF16 big endian") {
    const auto result = vispick::ParseIni(Utf16(L"[Save]\nFileName=截图😀", false));
    REQUIRE(result.valid); CHECK(result.entries[0].value == L"截图😀");
}
TEST_CASE("INI rejects truncated UTF8") { CHECK_FALSE(ParseText("[x]\na=\xE4\xB8").valid); }
TEST_CASE("INI rejects overlong UTF8") { CHECK_FALSE(ParseText("[x]\na=\xC0\xAF").valid); }
TEST_CASE("INI rejects UTF8 encoded surrogates") { CHECK_FALSE(ParseText("[x]\na=\xED\xA0\x80").valid); }
TEST_CASE("INI rejects out of range Unicode") { CHECK_FALSE(ParseText("[x]\na=\xF4\x90\x80\x80").valid); }
TEST_CASE("INI legacy fallback decodes GBK only without a BOM") {
    const auto parse = [](std::string_view text) {
        return vispick::ParseIni({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, 936);
    };
    const auto document = parse("[Editor]\nFontName=\xD6\xD0\xCE\xC4");
    REQUIRE(document.valid); REQUIRE(document.entries.size() == 1);
    CHECK(document.entries.front().value == L"中文");
    CHECK_FALSE(parse("\xEF\xBB\xBF[Editor]\nFontName=\xD6\xD0\xCE\xC4").valid);
    CHECK(ParseText("　[Editor] \n　FontName　=　中文 ").entries.front().value == L"中文");
}
TEST_CASE("INI rejects UTF16 odd bytes") { const std::uint8_t bytes[]{0xFF, 0xFE, 0x41}; CHECK_FALSE(vispick::ParseIni(bytes).valid); }
TEST_CASE("INI rejects isolated high UTF16 surrogate") { const std::uint8_t bytes[]{0xFF, 0xFE, 0x00, 0xD8}; CHECK_FALSE(vispick::ParseIni(bytes).valid); }
TEST_CASE("INI rejects isolated low UTF16 surrogate") { const std::uint8_t bytes[]{0xFE, 0xFF, 0xDC, 0x00}; CHECK_FALSE(vispick::ParseIni(bytes).valid); }
TEST_CASE("INI rejects embedded NUL") { CHECK_FALSE(ParseText(std::string("[x]\na=a\0b", 10)).valid); }
TEST_CASE("INI rejects binary control bytes even inside otherwise valid values") { CHECK_FALSE(ParseText("[Editor]\nFontName=abc\x01").valid); }
TEST_CASE("INI folds section and key case while retaining value case") {
    const auto result = ParseText("[GeNeRaL]\nLaNgUaGe=zh-CN");
    REQUIRE(result.valid); CHECK(result.entries[0].section == L"general"); CHECK(result.entries[0].key == L"language"); CHECK(result.entries[0].value == L"zh-CN");
}
TEST_CASE("INI comments require whitespace before semicolon") {
    const auto result = ParseText(" ; comment\n[Editor] ; section\nColor=#FF3B30 ; red\nFontName=a;b");
    REQUIRE(result.valid); REQUIRE(result.entries.size() == 2); CHECK(result.entries[0].value == L"#FF3B30"); CHECK(result.entries[1].value == L"a;b");
}
TEST_CASE("INI trims tabs and preserves empty values") {
    const auto result = ParseText("\t[ Hotkeys ]\t\n\tPinClipboard\t=\t ; off");
    REQUIRE(result.valid); CHECK(result.entries[0].value.empty()); CHECK(result.entries[0].key == L"pinclipboard");
}
TEST_CASE("INI diagnoses actual lines across CRLF LF and CR") {
    const auto result = ParseText("; comment\r\n[Pin]\nOpacity=50\rbroken\r\n");
    CHECK_FALSE(result.valid); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 4);
}
TEST_CASE("INI rejects assignments outside sections") { CHECK_FALSE(ParseText("a=b").valid); }
TEST_CASE("INI rejects malformed sections") {
    for (const auto* text : {"[]", "[ ]", "[pin", "[pin] junk", "[[pin]]"}) { CHECK_FALSE(ParseText(text).valid); }
}
TEST_CASE("INI rejects missing equals and empty key") { CHECK_FALSE(ParseText("[Pin]\nOpacity").valid); CHECK_FALSE(ParseText("[Pin]\n =50").valid); }
TEST_CASE("INI keeps equals inside values") { const auto result = ParseText("[Save]\nFileName=a=b"); REQUIRE(result.valid); CHECK(result.entries[0].value == L"a=b"); }
TEST_CASE("INI retains duplicate entries with source lines") {
    const auto result = ParseText("[Pin]\nOpacity=10\nOpacity=20");
    REQUIRE(result.valid); REQUIRE(result.entries.size() == 2); CHECK(result.entries[1].line == 3);
}
