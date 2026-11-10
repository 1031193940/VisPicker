#include <doctest/doctest.h>
#include "platform/Console.h"

#include <stdexcept>
#include <filesystem>

TEST_CASE("Console text conversions preserve Chinese paths and supplementary characters") {
    const std::wstring original = L"无法打开 D:\\参考图\\image-\U0001F3A8.png\n";
    const auto encoded = vispick::WideToUtf8(original);
    CHECK(vispick::Utf8ToWide(encoded) == original);
    CHECK(encoded.find("参考图") != std::string::npos);
}

TEST_CASE("Text conversion rejects malformed Unicode instead of replacing silently") {
    CHECK_THROWS_AS(static_cast<void>(vispick::Utf8ToWide(std::string("\xC0\xAF", 2))), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800)))),
                    std::invalid_argument);
    CHECK(vispick::Utf8ToWide("").empty());
    CHECK(vispick::WideToUtf8(L"").empty());
}

TEST_CASE("Text conversion respects explicit lengths including embedded zero") {
    const std::wstring original{L'A', L'\0', L'中'};
    CHECK(vispick::Utf8ToWide(vispick::WideToUtf8(original)) == original);
}

TEST_CASE("Exception messages use the current Windows ANSI code page") {
    char message[256]{};
    // This tests the documented input boundary, independently of UTF-8 output.
    REQUIRE(WideCharToMultiByte(CP_ACP, 0, L"Error: 文件不存在", -1, message, static_cast<int>(sizeof(message)),
                                nullptr, nullptr) != 0);
    wchar_t expected[256]{};
    REQUIRE(MultiByteToWideChar(CP_ACP, 0, message, -1, expected, 256) != 0);
    CHECK(vispick::ExceptionMessage(std::runtime_error(message)) == expected);
}

TEST_CASE("Filesystem diagnostics retain native Unicode paths on any ANSI code page") {
    const std::filesystem::filesystem_error error("open failed", L"D:\\参考图\\错误.png",
                                                  std::make_error_code(std::errc::permission_denied));
    CHECK(vispick::ExceptionMessage(error).find(L"D:\\参考图\\错误.png") != std::wstring::npos);
}
