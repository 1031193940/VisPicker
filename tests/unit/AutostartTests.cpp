#include <doctest/doctest.h>
#include "platform/Autostart.h"

// See Autostart.cpp for the SDK's RegOpenKeyExW SAL annotation defect.
#pragma warning(push)
#pragma warning(disable: 6553)
#include <windows.h>
#pragma warning(pop)
#include <shellapi.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class TestRegistry {
public:
    TestRegistry() {
        static unsigned sequence = 0;
        path = L"Software\\VisPick.UnitTests." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    }
    ~TestRegistry() { RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(), KEY_WOW64_64KEY, 0); }
    TestRegistry(const TestRegistry&) = delete;
    TestRegistry& operator=(const TestRegistry&) = delete;
    std::wstring path;
    void Write(const DWORD type, const void* data, const DWORD bytes) const {
        HKEY key = nullptr;
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS);
        const auto status = RegSetValueExW(key, L"VisPick", 0, type, static_cast<const BYTE*>(data), bytes);
        RegCloseKey(key);
        REQUIRE(status == ERROR_SUCCESS);
    }
    [[nodiscard]] DWORD StoredBytes() const {
        HKEY key = nullptr;
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS);
        DWORD bytes = 0;
        const auto status = RegQueryValueExW(key, L"VisPick", nullptr, nullptr, nullptr, &bytes);
        RegCloseKey(key);
        REQUIRE(status == ERROR_SUCCESS);
        return bytes;
    }
    [[nodiscard]] bool Exists() const {
        HKEY key = nullptr;
        const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (key) { RegCloseKey(key); }
        return status == ERROR_SUCCESS;
    }
};

struct ArgumentDeleter {
    void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
};

std::vector<std::wstring> ParseCommand(const std::wstring& command) {
    int count = 0;
    const std::unique_ptr<wchar_t*, ArgumentDeleter> arguments(CommandLineToArgvW(command.c_str(), &count));
    if (!arguments) { throw std::runtime_error("CommandLineToArgvW failed"); }
    return {arguments.get(), arguments.get() + count};
}

} // namespace

TEST_CASE("Autostart read disable and repair do not opt in or create a missing key") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    CHECK_FALSE(autostart.Command());
    CHECK_FALSE(autostart.Enabled());
    CHECK_FALSE(autostart.RepairIfEnabled(L"C:\\VisPick.exe"));
    autostart.SetEnabled(false, {});
    CHECK_FALSE(registry.Exists());
}

TEST_CASE("Autostart enable and disable affect only the configured value") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    const vispick::Autostart unrelated(registry.path, L"Unrelated");
    unrelated.SetEnabled(true, L"C:\\Other.exe");
    autostart.SetEnabled(true, L"C:\\Program Files\\VisPick\\VisPick.exe", L"C:\\配置\\user.ini");
    CHECK(autostart.Enabled());
    CHECK(autostart.Command() == vispick::Autostart::BuildCommand(L"C:\\Program Files\\VisPick\\VisPick.exe", L"C:\\配置\\user.ini"));
    autostart.SetEnabled(false, {});
    CHECK_FALSE(autostart.Enabled());
    CHECK_FALSE(autostart.Command());
    CHECK(unrelated.Enabled());
    CHECK_NOTHROW(autostart.SetEnabled(false, {}));
}

TEST_CASE("Autostart repair updates a missing executable and preserves its saved config") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    autostart.SetEnabled(true, L"C:\\Old\\VisPick.exe", L"C:\\old.ini");
    CHECK(autostart.RepairIfEnabled(L"D:\\New\\VisPick.exe"));
    CHECK(autostart.Command() == L"\"D:\\New\\VisPick.exe\" --config \"C:\\old.ini\"");
    CHECK_FALSE(autostart.RepairIfEnabled(L"D:\\New\\VisPick.exe"));
    autostart.SetEnabled(false, {});
    CHECK_FALSE(autostart.RepairIfEnabled(L"D:\\New\\VisPick.exe"));
    CHECK_FALSE(autostart.Command());
}
TEST_CASE("Autostart repair leaves a working installation intact") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    // Use the running test executable to avoid depending on an optional OS app.
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0); REQUIRE(length < executable.size()); executable.resize(length);
    const auto oldPath = std::filesystem::path(executable);
    autostart.SetEnabled(true, oldPath);
    const auto previous = autostart.Command();
    CHECK_FALSE(autostart.RepairIfEnabled(oldPath.parent_path() / L"different-copy" / oldPath.filename()));
    CHECK(autostart.Command() == previous);
}

TEST_CASE("Autostart treats an existing empty registry string as disabled") {
    TestRegistry registry;
    const wchar_t empty[] = L"";
    registry.Write(REG_SZ, empty, sizeof(empty));
    const vispick::Autostart autostart(registry.path);
    CHECK_FALSE(autostart.Enabled());
    CHECK_FALSE(autostart.RepairIfEnabled(L"C:\\VisPick.exe"));
    REQUIRE(autostart.Command());
    CHECK(autostart.Command()->empty());
}

TEST_CASE("Autostart quotes unicode whitespace and backslashes with Windows argv semantics") {
    const std::filesystem::path executable = L"C:\\Program Files\\截图工具\\VisPick.exe";
    for (const std::filesystem::path config : {std::filesystem::path(L"C:\\config folder\\user.ini"),
         std::filesystem::path(L"C:\\trailing slash\\"), std::filesystem::path(L"C:\\a\"b\\user.ini")}) {
        const auto arguments = ParseCommand(vispick::Autostart::BuildCommand(executable, config));
        REQUIRE(arguments.size() == 3);
        CHECK(arguments[0] == executable.native());
        CHECK(arguments[1] == L"--config");
        CHECK(arguments[2] == config.native());
    }
    const auto arguments = ParseCommand(vispick::Autostart::BuildCommand(executable));
    REQUIRE(arguments.size() == 1);
    CHECK(arguments[0] == executable.native());
}

TEST_CASE("Autostart resolves relative paths before sign-in working directory changes") {
    const auto arguments = ParseCommand(vispick::Autostart::BuildCommand(L".\\VisPick.exe", L"settings\\..\\user.ini"));
    REQUIRE(arguments.size() == 3);
    CHECK(arguments[0] == (std::filesystem::current_path() / L"VisPick.exe").native());
    CHECK(arguments[2] == (std::filesystem::current_path() / L"user.ini").native());
}

TEST_CASE("Autostart enforces Run command length without damaging existing value") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    const std::filesystem::path boundary = L"C:\\" + std::wstring(255, L'x');
    REQUIRE(vispick::Autostart::BuildCommand(boundary).size() == 260);
    autostart.SetEnabled(true, boundary);
    const auto previous = autostart.Command();
    const std::filesystem::path tooLong = boundary.native() + L"x";
    CHECK_THROWS_AS(static_cast<void>(autostart.SetEnabled(true, tooLong)), std::length_error);
    CHECK(autostart.Command() == previous);
    CHECK_FALSE(autostart.RepairIfEnabled(tooLong));
    CHECK(autostart.Command() == previous);
    TestRegistry missing;
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart(missing.path).SetEnabled(true, tooLong)), std::length_error);
    CHECK_FALSE(missing.Exists());
}

TEST_CASE("Autostart rejects invalid registry strings without reading out of bounds") {
    TestRegistry registry;
    const vispick::Autostart autostart(registry.path);
    // Keep the next code unit explicitly nonzero: RegSetValueExW may include an
    // adjacent NUL as compatibility behavior even when cbData excludes it.
    const wchar_t unterminated[] = {L'a', L'b', L'c'};
    registry.Write(REG_SZ, unterminated, 2 * sizeof(wchar_t));
    REQUIRE(registry.StoredBytes() == 2 * sizeof(wchar_t));
    CHECK_THROWS_AS(static_cast<void>(autostart.Command()), std::runtime_error);
    const wchar_t embeddedNull[] = {L'a', L'\0', L'b', L'\0'};
    registry.Write(REG_SZ, embeddedNull, sizeof(embeddedNull));
    CHECK_THROWS_AS(static_cast<void>(autostart.Command()), std::runtime_error);
    const DWORD wrongType = 1;
    registry.Write(REG_DWORD, &wrongType, sizeof(wrongType));
    CHECK_THROWS_AS(static_cast<void>(autostart.Command()), std::runtime_error);
    registry.Write(REG_SZ, unterminated, 3);
    CHECK_THROWS_AS(static_cast<void>(autostart.Command()), std::runtime_error);
    const std::wstring oversized(32769, L'x');
    registry.Write(REG_SZ, oversized.data(), static_cast<DWORD>(oversized.size() * sizeof(wchar_t)));
    CHECK_THROWS_AS(static_cast<void>(autostart.Command()), std::runtime_error);
}

TEST_CASE("Autostart preserves registry commands with environment expansion") {
    TestRegistry registry;
    const wchar_t command[] = L"\"%LOCALAPPDATA%\\VisPick.exe\"";
    registry.Write(REG_EXPAND_SZ, command, sizeof(command));
    const vispick::Autostart autostart(registry.path);
    CHECK(autostart.Enabled());
    CHECK_FALSE(autostart.RepairIfEnabled(L"C:\\Users\\test\\VisPick.exe"));
    CHECK(autostart.Command() == command);
}

TEST_CASE("Autostart rejects empty executable and embedded null arguments") {
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart::BuildCommand({})), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart::BuildCommand(std::wstring(L"a\0b", 3))), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart::BuildCommand(L"C:\\VisPick.exe", std::wstring(L"a\0b", 3))), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart(L"")), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::Autostart(L"Software\\Tests", L"")), std::invalid_argument);
}
