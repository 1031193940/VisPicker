#include <doctest/doctest.h>
#include "core/Paths.h"
#include "core/Config.h"

#include <atomic>
#include <fstream>
#include <stdexcept>

namespace {
const vispick::FolderTemplateContext folders{L"C:\\Users\\测试\\Pictures", L"C:\\Users\\测试\\Desktop",
                                             L"D:\\Portable\\VisPick"};
const SYSTEMTIME timestamp{2026, 3, 0, 4, 5, 6, 7, 8};

class TestDirectory {
public:
    TestDirectory() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ =
                std::filesystem::temp_directory_path() / (L"VisPick-Paths-" + std::to_wstring(GetCurrentProcessId()) +
                                                          L"-" + std::to_wstring(serial.fetch_add(1)));
            if (std::filesystem::create_directory(path_)) { return; }
        }
        throw std::runtime_error("Cannot create unique test directory");
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> TemplateBytes() {
    constexpr std::string_view text = "; 模板\n[Pin]\nOpacity=85\n";
    return {text.begin(), text.end()};
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("Paths expands Pictures Desktop and executable placeholders") {
    CHECK(vispick::ExpandSaveFolder(L"{pictures}", folders) == folders.pictures);
    CHECK(vispick::ExpandSaveFolder(L"{desktop}", folders) == folders.desktop);
    CHECK(vispick::ExpandSaveFolder(L"{exe_dir}", folders) == folders.executableDirectory);
}
TEST_CASE("Paths combines a known folder with Unicode subdirectories") {
    CHECK(vispick::ExpandSaveFolder(L"{pictures}\\截图 空间", folders) == folders.pictures / L"截图 空间");
}
TEST_CASE("Paths normalizes folder dot segments") {
    CHECK(vispick::ExpandSaveFolder(L"{exe_dir}\\one\\..\\two", folders) == folders.executableDirectory / L"two");
}
TEST_CASE("Paths expands environment variables") {
    REQUIRE(SetEnvironmentVariableW(L"VISPICK_UNIT_FOLDER", L"C:\\测试目录") != FALSE);
    CHECK(vispick::ExpandSaveFolder(L"%VISPICK_UNIT_FOLDER%\\shots", folders) ==
          std::filesystem::path(L"C:\\测试目录\\shots"));
    CHECK(SetEnvironmentVariableW(L"VISPICK_UNIT_FOLDER", nullptr) != FALSE);
}
TEST_CASE("Paths rejects undefined and unclosed environment variables") {
    CHECK_THROWS_AS(static_cast<void>(vispick::ExpandSaveFolder(L"%VISPICK_UNDEFINED_UNIT_VAR%", folders)),
                    std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::ExpandSaveFolder(L"C:\\%bad", folders)), std::invalid_argument);
}
TEST_CASE("Paths rejects unknown and malformed folder placeholders") {
    for (const auto* text : {L"{unknown}", L"{pictures", L"C:\\bad}", L"{}", L"{{pictures}}"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandSaveFolder(text, folders)), std::invalid_argument);
    }
}
TEST_CASE("Paths rejects relative and drive relative save folders") {
    for (const auto* text : {L"", L"shots", L"..\\shots", L"C:shots", L"\\shots"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandSaveFolder(text, folders)), std::invalid_argument);
    }
}
TEST_CASE("Paths accepts absolute drive and UNC folders") {
    CHECK(vispick::ExpandSaveFolder(L"D:\\shots", folders) == std::filesystem::path(L"D:\\shots"));
    CHECK(vispick::ExpandSaveFolder(L"\\\\server\\share\\shots", folders) ==
          std::filesystem::path(L"\\\\server\\share\\shots"));
}
TEST_CASE("Paths rejects illegal folder characters and alternate streams") {
    for (const auto* text : {L"C:\\bad*", L"C:\\bad?", L"C:\\bad|", L"C:\\bad:stream", L"C:\\bad\nname"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandSaveFolder(text, folders)), std::invalid_argument);
    }
}
TEST_CASE("Paths formats date time milliseconds and image dimensions deterministically") {
    CHECK(vispick::ExpandFileName(L"VisPick_{date}_{time}_{ms}_{w}x{h}", timestamp, 1920, 1080) ==
          L"VisPick_20260304_050607_008_1920x1080");
}
TEST_CASE("Paths filename permits repeated tokens and Unicode literal text") {
    CHECK(vispick::ExpandFileName(L"截图_{w}_{w}_😀", timestamp, 3, 4) == L"截图_3_3_😀");
}
TEST_CASE("Paths filename preserves legal semicolon and equals characters") {
    CHECK(vispick::ExpandFileName(L"a;b=c", timestamp, 1, 1) == L"a;b=c");
}
TEST_CASE("Paths filename rejects unknown and unmatched tokens") {
    for (const auto* text : {L"{unknown}", L"{date", L"date}", L"{}"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(text, timestamp, 1, 1)), std::invalid_argument);
    }
}
TEST_CASE("Paths filename rejects path separators and Windows forbidden characters") {
    for (const auto* text : {L"a/b", L"a\\b", L"a:b", L"a*b", L"a?b", L"a|b", L"a<b", L"a>b", L"a\"b", L"a\nb"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(text, timestamp, 1, 1)), std::invalid_argument);
    }
}
TEST_CASE("Paths filename rejects empty trailing space and trailing dot") {
    for (const auto* text : {L"", L"shot ", L"shot.", L".", L".."}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(text, timestamp, 1, 1)), std::invalid_argument);
    }
}
TEST_CASE("Paths filename rejects case insensitive Windows device names including extensions") {
    for (const auto* text : {L"con", L"NUL", L"aux.png", L"prn", L"COM1", L"LPT9.txt", L"COM¹", L"LPT²"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(text, timestamp, 1, 1)), std::invalid_argument);
    }
    CHECK(vispick::ExpandFileName(L"COM10", timestamp, 1, 1) == L"COM10");
}
TEST_CASE("Paths filename length reserves room for extension and uniqueness suffix") {
    CHECK(vispick::ExpandFileName(std::wstring(240, L'a'), timestamp, 1, 1).size() == 240);
    CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(std::wstring(241, L'a'), timestamp, 1, 1)),
                    std::invalid_argument);
}
TEST_CASE("Paths filename requires positive image dimensions") {
    CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(L"shot", timestamp, 0, 1)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(vispick::ExpandFileName(L"shot", timestamp, 1, -1)), std::invalid_argument);
}
TEST_CASE("Paths creates a default config beside the executable exactly once") {
    TestDirectory directory;
    const auto first = vispick::ResolveConfigPathAt(directory.Path(), directory.Path() / L"fallback", TemplateBytes());
    CHECK(first.created);
    CHECK_FALSE(first.usedFallback);
    CHECK(first.path == directory.Path() / L"VisPick.ini");
    std::ofstream(first.path) << "; keep comments\n[Pin]\nOpacity=31\n";
    const auto before = ReadAll(first.path);
    const auto second = vispick::ResolveConfigPathAt(directory.Path(), directory.Path() / L"fallback", TemplateBytes());
    CHECK_FALSE(second.created);
    CHECK(ReadAll(first.path) == before);
    CHECK(vispick::LoadConfig(first.path).config.pin.opacity == 31);
}
TEST_CASE("Paths default directory failure falls back with a visible notice") {
    TestDirectory directory;
    const auto obstructed = directory.Path() / L"not-a-directory";
    std::ofstream(obstructed) << "occupied";
    const auto result = vispick::ResolveConfigPathAt(obstructed, directory.Path() / L"fallback", TemplateBytes());
    CHECK(result.created);
    CHECK(result.usedFallback);
    CHECK_FALSE(result.notice.empty());
    CHECK(std::filesystem::exists(result.path));
}
TEST_CASE("Paths explicit config creates the template at the requested existing parent") {
    TestDirectory directory;
    const auto requested = directory.Path() / L"自定义.ini";
    const auto result = vispick::ResolveConfigPathAt({}, {}, TemplateBytes(), requested);
    CHECK(result.created);
    CHECK_FALSE(result.usedFallback);
    CHECK(result.path == requested);
    CHECK(vispick::LoadConfig(requested).config.pin.opacity == 85);
}
TEST_CASE("Paths explicit existing config remains byte for byte unchanged") {
    TestDirectory directory;
    const auto requested = directory.Path() / L"自定义.ini";
    std::ofstream(requested) << "; do not overwrite\n[Pin]\nOpacity=33\n";
    const auto before = ReadAll(requested);
    const auto result = vispick::ResolveConfigPathAt({}, {}, TemplateBytes(), requested);
    CHECK_FALSE(result.created);
    CHECK(ReadAll(requested) == before);
}
TEST_CASE("Paths explicit missing parent fails without creating fallback") {
    TestDirectory directory;
    const auto fallback = directory.Path() / L"fallback";
    CHECK_THROWS_AS(static_cast<void>(vispick::ResolveConfigPathAt(directory.Path(), fallback, TemplateBytes(),
                                                                   directory.Path() / L"missing" / L"config.ini")),
                    std::system_error);
    CHECK_FALSE(std::filesystem::exists(fallback));
}
TEST_CASE("Paths reads an existing readonly config without overwriting or falling back") {
    TestDirectory directory;
    const auto path = directory.Path() / L"VisPick.ini";
    std::ofstream(path) << "; readonly settings\n[Pin]\nOpacity=42\n";
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    try {
        const auto result =
            vispick::ResolveConfigPathAt(directory.Path(), directory.Path() / L"fallback", TemplateBytes());
        CHECK_FALSE(result.created);
        CHECK_FALSE(result.usedFallback);
        CHECK(result.path == path);
        CHECK(vispick::LoadConfig(path).config.pin.opacity == 42);
    } catch (...) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        throw;
    }
    CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
}
TEST_CASE("Paths explicit directory or empty path never silently selects defaults") {
    TestDirectory directory;
    CHECK_THROWS_AS(static_cast<void>(vispick::ResolveConfigPathAt({}, {}, TemplateBytes(), directory.Path())),
                    std::system_error);
    CHECK_THROWS_AS(static_cast<void>(vispick::ResolveConfigPathAt({}, {}, TemplateBytes(), std::filesystem::path{})),
                    std::invalid_argument);
}
TEST_CASE("Paths generated UTF8 template retains its Chinese comments") {
    TestDirectory directory;
    const auto result = vispick::ResolveConfigPathAt(directory.Path(), {}, TemplateBytes());
    CHECK(ReadAll(result.path).find("模板") != std::string::npos);
}
TEST_CASE("Paths resolves the current executable and diagnostics directories") {
    CHECK(vispick::ExecutablePath().is_absolute());
    CHECK(std::filesystem::is_regular_file(vispick::ExecutablePath()));
    const auto directories = vispick::DiagnosticDirectories();
    REQUIRE_FALSE(directories.empty());
    CHECK(directories.front() == vispick::ExecutablePath().parent_path());
}
