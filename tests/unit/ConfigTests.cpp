#include <doctest/doctest.h>
#include "core/Config.h"
#include "core/Paths.h"

#include <atomic>
#include <fstream>
#include <stdexcept>

namespace {
vispick::ConfigLoadResult ParseSettings(std::string_view text, const vispick::Config* previous = nullptr) {
    return vispick::ParseConfig({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, previous);
}

class ConfigurationFile {
public:
    ConfigurationFile() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            directory_ = std::filesystem::temp_directory_path() /
                (L"VisPick-Config-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(serial.fetch_add(1)));
            if (std::filesystem::create_directory(directory_)) { path = directory_ / L"settings.ini"; return; }
        }
        throw std::runtime_error("Cannot create configuration test directory");
    }
    ~ConfigurationFile() { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); std::error_code ignored; std::filesystem::remove_all(directory_, ignored); }
    void Write(const std::string_view text) const {
        std::ofstream output(path, std::ios::binary); output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) { throw std::runtime_error("Cannot write configuration fixture"); }
    }
    [[nodiscard]] std::string Read() const {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] std::size_t FileCount() const {
        return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(directory_), std::filesystem::directory_iterator()));
    }
    std::filesystem::path path;
private:
    std::filesystem::path directory_;
};

std::string Utf16Bytes(const std::wstring_view text, const bool littleEndian) {
    std::string bytes = littleEndian ? "\xFF\xFE" : "\xFE\xFF";
    for (const wchar_t character : text) {
        bytes += static_cast<char>(littleEndian ? character & 255 : character >> 8);
        bytes += static_cast<char>(littleEndian ? character >> 8 : character & 255);
    }
    return bytes;
}
}

TEST_CASE("Config embedded default template is complete and valid") {
    const auto result = vispick::ParseConfig(vispick::LoadDefaultConfigTemplate());
    REQUIRE(result.accepted); CHECK(result.diagnostics.empty());
    CHECK(result.config.hotkeys.screenshot == vispick::Hotkey{2, '1'});
    CHECK(result.config.hotkeys.gif == vispick::Hotkey{2, '2'});
    CHECK_FALSE(result.config.hotkeys.pinClipboard.Enabled()); CHECK_FALSE(result.config.hotkeys.togglePins.Enabled());
    CHECK(result.config.capture.windowSnap); CHECK(result.config.capture.magnifier); CHECK(result.config.capture.maskOpacity == 45); CHECK_FALSE(result.config.capture.includeCursor);
    CHECK(result.config.save.folder == L"{pictures}"); CHECK(result.config.save.fileName == L"VisPick_{date}_{time}");
    CHECK(result.config.save.format == vispick::SaveFormat::Png); CHECK(result.config.save.jpgQuality == 92);
    CHECK(result.config.editor.color == 0xFF3B30); CHECK(result.config.editor.strokeWidth == 4); CHECK(result.config.editor.fontSize == 20);
    CHECK(result.config.editor.fontName == L"Microsoft YaHei"); CHECK(result.config.editor.mosaicSize == 10);
    CHECK_FALSE(result.config.pin.instant); CHECK(result.config.pin.topmost); CHECK(result.config.pin.border); CHECK(result.config.pin.borderColor == 0x3A7AFE);
    CHECK_FALSE(result.config.pin.acrossDesktops);
    CHECK(result.config.pin.shadow); CHECK(result.config.pin.opacity == 100); CHECK(result.config.pin.inactiveOpacity == 100);
    CHECK(result.config.pin.wheel == vispick::PinWheel::Zoom); CHECK(result.config.pin.zoomStep == 10); CHECK(result.config.pin.smooth);
    CHECK(result.config.pin.snapDistance == 10); CHECK(result.config.pin.dustBox == 5); CHECK(result.config.pin.compactMode == vispick::CompactMode::Crop);
    CHECK(result.config.pin.compactSize == 50); CHECK(result.config.pin.compactOpacity == 80); CHECK(result.config.pin.compactLineColor == 0x0000FF);
    CHECK(result.config.gif.fps == 30); CHECK(result.config.gif.maxSeconds == 300); CHECK(result.config.gif.showCursor);
    CHECK_FALSE(result.config.gif.highlightClicks); CHECK_FALSE(result.config.gif.dither); CHECK(result.config.gif.scale == 100);
    CHECK(result.config.general.trayClick == vispick::TrayClick::Screenshot); CHECK(result.config.general.language == vispick::Language::Auto); CHECK_FALSE(result.config.general.log);
}
TEST_CASE("Config reports unknown section at its declaration") {
    const auto result = ParseSettings("; comment\n[Future]\na=1");
    REQUIRE(result.accepted); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 2);
}
TEST_CASE("Config reports unknown key without suppressing valid fields") {
    const auto result = ParseSettings("[Capture]\nMagnifier=false\nWrong=1");
    REQUIRE(result.accepted); CHECK_FALSE(result.config.capture.magnifier); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 3);
}
TEST_CASE("Config invalid and missing fields retain the previous snapshot") {
    vispick::Config previous; previous.capture.maskOpacity = 72; previous.capture.magnifier = false;
    const auto result = ParseSettings("[Capture]\nMaskOpacity=101", &previous);
    CHECK(result.config.capture.maskOpacity == 72); CHECK_FALSE(result.config.capture.magnifier); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 2);
}
TEST_CASE("Config rejects a truncated file and preserves every hotkey") {
    vispick::Config previous;
    previous.hotkeys.screenshot = {MOD_ALT, 'A'};
    previous.hotkeys.gif = {};
    previous.general.log = true;
    for (const auto* truncated : {"", "\r\n  \t", "; editor has not saved values yet"}) {
        const auto result = ParseSettings(truncated, &previous);
        CHECK_FALSE(result.accepted); CHECK(result.retryable);
        CHECK(result.config.hotkeys == previous.hotkeys); CHECK(result.config.general.log);
    }
    const auto partial = ParseSettings("[Capture]\nMagnifier=false", &previous);
    CHECK(partial.accepted); CHECK(partial.config.hotkeys == previous.hotkeys);
    CHECK_FALSE(partial.config.capture.magnifier);
}
TEST_CASE("Layered-window capture is an explicit GIF opt-in") {
    CHECK_FALSE(vispick::Config{}.gif.captureLayeredWindows);
    const auto result = ParseSettings("[Advanced]\ncapture_layered_gif=true");
    REQUIRE(result.diagnostics.empty()); CHECK(result.config.gif.captureLayeredWindows);
    CHECK(ParseSettings("[Gif]\nFps=20", &result.config).config.gif.captureLayeredWindows);
}
TEST_CASE("Reference images stay on their desktop by default and accept an explicit opt-in") {
    CHECK_FALSE(vispick::Config{}.pin.acrossDesktops);
    const auto enabled = ParseSettings("[Pin]\nAcrossDesktops=TRUE");
    REQUIRE(enabled.accepted); REQUIRE(enabled.diagnostics.empty()); CHECK(enabled.config.pin.acrossDesktops);
    CHECK(ParseSettings("[Pin]\nOpacity=80", &enabled.config).config.pin.acrossDesktops);
    for (const auto* value : {"1", "yes", ""}) {
        const auto invalid = ParseSettings(std::string("[Pin]\nAcrossDesktops=") + value, &enabled.config);
        CHECK(invalid.config.pin.acrossDesktops); CHECK_FALSE(invalid.diagnostics.empty());
    }
    CHECK_FALSE(ParseSettings("[Pin]\nAcrossDesktops=false", &enabled.config).config.pin.acrossDesktops);
}

TEST_CASE("Saving the desktop preference changes only its last value and preserves comments") {
    const ConfigurationFile file;
    const std::string before = "\xEF\xBB\xBF; 用户配置\r\n[Pin]\r\nAcrossDesktops=false\r\n aCrOsSdEsKtOpS \t=  false  ; 保留说明\r\nOpacity=72\r\n[General]\r\nLog=true";
    file.Write(before);
    vispick::SavePinAcrossDesktops(file.path, true);
    auto expected = before; expected.replace(expected.find("false  ;"), 5, "true");
    CHECK(file.Read() == expected); CHECK(vispick::LoadConfig(file.path).config.pin.acrossDesktops);
    CHECK(vispick::LoadConfig(file.path).config.pin.opacity == 72);
    vispick::SavePinAcrossDesktops(file.path, false);
    CHECK(file.Read() == before); CHECK_FALSE(vispick::LoadConfig(file.path).config.pin.acrossDesktops);
    CHECK(file.FileCount() == 1);
}

TEST_CASE("Saving the desktop preference inserts missing keys and sections without replacing other settings") {
    const ConfigurationFile file;
    for (const auto* before : {"[General]\nLog=true", "[Pin]\nOpacity=72\n[General]\nLog=true", "[Pin]", "[Pin]\rOpacity=72\r", "[Pin]\nAcrossDesktops=  ; empty value\n"}) {
        file.Write(before);
        vispick::SavePinAcrossDesktops(file.path, true);
        const auto loaded = vispick::LoadConfig(file.path);
        REQUIRE(loaded.accepted); CHECK(loaded.diagnostics.empty()); CHECK(loaded.config.pin.acrossDesktops);
        CHECK(file.Read().find("[Pin]") != std::string::npos);
        if (std::string_view(before).find("Log=true") != std::string_view::npos) { CHECK(loaded.config.general.log); }
        if (std::string_view(before).find("Opacity=72") != std::string_view::npos) { CHECK(loaded.config.pin.opacity == 72); }
        if (std::string_view(before).find("; empty value") != std::string_view::npos) { CHECK(file.Read().find("; empty value") != std::string::npos); }
    }
}

TEST_CASE("Saving the desktop preference preserves both UTF16 byte orders and Unicode whitespace") {
    const ConfigurationFile file;
    for (const bool littleEndian : {true, false}) {
        const std::wstring before = L"; 中文说明\r\n[Pin]\r\nAcrossDesktops=\u3000false\u00A0; 注释\r\nOpacity=63";
        file.Write(Utf16Bytes(before, littleEndian));
        vispick::SavePinAcrossDesktops(file.path, true);
        auto expected = before; expected.replace(expected.find(L"false"), 5, L"true");
        CHECK(file.Read() == Utf16Bytes(expected, littleEndian));
        CHECK(vispick::LoadConfig(file.path).config.pin.acrossDesktops);
        file.Write(Utf16Bytes(L"[General]\nLog=true", littleEndian));
        vispick::SavePinAcrossDesktops(file.path, false);
        CHECK(file.Read() == Utf16Bytes(L"[General]\nLog=true\n[Pin]\nAcrossDesktops=false\n", littleEndian));
    }
}

TEST_CASE("Saving the desktop preference preserves legacy code page bytes") {
    const ConfigurationFile file;
    const std::wstring text = L"; 用户说明\n[Pin]\nAcrossDesktops=false ; 保留注释\n";
    const int length = WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(length > 0);
    std::string before(static_cast<std::size_t>(length), '\0');
    REQUIRE(WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), before.data(), length, nullptr, nullptr) == length);
    file.Write(before); vispick::SavePinAcrossDesktops(file.path, true);
    auto expected = before; expected.replace(expected.find("false"), 5, "true");
    CHECK(file.Read() == expected); CHECK(vispick::LoadConfig(file.path).config.pin.acrossDesktops);
}

TEST_CASE("Saving the desktop preference preserves malformed and read-only files on failure") {
    const ConfigurationFile file;
    CHECK_THROWS(vispick::SavePinAcrossDesktops(file.path, true));
    CHECK(file.FileCount() == 0);
    for (const auto* invalid : {"", "; unfinished", "[Pin]\n[broken", "\xEF\xBB\xBF[Pin]\n\xFF"}) {
        file.Write(invalid); CHECK_THROWS(vispick::SavePinAcrossDesktops(file.path, true)); CHECK(file.Read() == invalid);
    }
    const std::string original = "[Pin]\nAcrossDesktops=false\n";
    file.Write(original); REQUIRE(SetFileAttributesW(file.path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    CHECK_THROWS(vispick::SavePinAcrossDesktops(file.path, true));
    CHECK(file.Read() == original); CHECK(file.FileCount() == 1);
}
TEST_CASE("Config first invalid field retains its documented default") {
    const auto result = ParseSettings("[Capture]\nMaskOpacity=-1"); CHECK(result.config.capture.maskOpacity == 45); CHECK_FALSE(result.diagnostics.empty());
}
TEST_CASE("Config bad encoding preserves the entire previous snapshot") {
    vispick::Config previous; previous.capture.magnifier = false; previous.pin.opacity = 67;
    const auto result = ParseSettings("\xEF\xBB\xBF[Pin]\nOpacity=12\n\xFF", &previous);
    CHECK_FALSE(result.accepted); CHECK(result.config.pin.opacity == 67); CHECK_FALSE(result.config.capture.magnifier);
}
TEST_CASE("Config structural errors do not partially apply earlier fields") {
    vispick::Config previous; previous.pin.opacity = 67;
    const auto result = ParseSettings("[Pin]\nOpacity=12\n[broken", &previous);
    CHECK_FALSE(result.accepted); CHECK(result.config.pin.opacity == 67); CHECK(result.diagnostics[0].line == 3);
}
TEST_CASE("Config strict boolean values reject permissive numeric and yes aliases") {
    for (const auto* value : {"1", "yes", "on", ""}) {
        const auto result = ParseSettings(std::string("[Capture]\nWindowSnap=") + value);
        CHECK(result.config.capture.windowSnap); CHECK_FALSE(result.diagnostics.empty());
    }
    CHECK_FALSE(ParseSettings("[Capture]\nWindowSnap=FALSE").config.capture.windowSnap);
}
TEST_CASE("Config boolean fields update independently across all sections") {
    const auto result = ParseSettings("[Capture]\nWindowSnap=false\nMagnifier=false\nIncludeCursor=true\n[Pin]\nInstant=true\nTopmost=false\nBorder=false\nShadow=false\nSmooth=false\n[Gif]\nShowCursor=false\nHighlightClicks=true\nDither=true\n[General]\nLog=true");
    REQUIRE(result.diagnostics.empty());
    CHECK_FALSE(result.config.capture.windowSnap); CHECK_FALSE(result.config.capture.magnifier); CHECK(result.config.capture.includeCursor);
    CHECK(result.config.pin.instant); CHECK_FALSE(result.config.pin.topmost); CHECK_FALSE(result.config.pin.border); CHECK_FALSE(result.config.pin.shadow); CHECK_FALSE(result.config.pin.smooth);
    CHECK_FALSE(result.config.gif.showCursor); CHECK(result.config.gif.highlightClicks); CHECK(result.config.gif.dither); CHECK(result.config.general.log);
}
TEST_CASE("Config strict integers reject overflow signs suffixes and fractional input") {
    for (const auto* value : {"999999999999999999999999999999", "+50", "50%", "1.5", "0x10", "-0"}) {
        const auto result = ParseSettings(std::string("[Pin]\nOpacity=") + value);
        CHECK(result.config.pin.opacity == 100); CHECK_FALSE(result.diagnostics.empty());
    }
}
TEST_CASE("Pin opacity cannot hide the entire window and capture mask may be zero") {
    const auto result = ParseSettings("[Capture]\nMaskOpacity=0\n[Pin]\nOpacity=0\nInactiveOpacity=100\nCompactOpacity=0");
    CHECK(result.diagnostics.size() == 2); CHECK(result.config.capture.maskOpacity == 0); CHECK(result.config.pin.opacity == 100); CHECK(result.config.pin.compactOpacity == 80);
}
TEST_CASE("Config strict RGB colors preserve red green blue order") {
    const auto result = ParseSettings("[Editor]\nColor=#12aB34\n[Pin]\nBorderColor=#001122\nCompactLineColor=#FFFFFF");
    CHECK(result.diagnostics.empty()); CHECK(result.config.editor.color == 0x12AB34); CHECK(result.config.pin.borderColor == 0x001122); CHECK(result.config.pin.compactLineColor == 0xFFFFFF);
}
TEST_CASE("Config invalid colors retain the prior color") {
    vispick::Config previous; previous.editor.color = 0x123456;
    for (const auto* value : {"red", "#fff", "#FF0000FF", "#GG0000", "FF0000"}) {
        const auto result = ParseSettings(std::string("[Editor]\nColor=") + value, &previous);
        CHECK(result.config.editor.color == previous.editor.color); CHECK_FALSE(result.diagnostics.empty());
    }
}
TEST_CASE("Config editor discrete line widths and font sizes are validated") {
    CHECK(ParseSettings("[Editor]\nStrokeWidth=6\nFontSize=28").diagnostics.empty());
    CHECK(ParseSettings("[Editor]\nStrokeWidth=2\nFontSize=14").diagnostics.empty());
    const auto result = ParseSettings("[Editor]\nStrokeWidth=3\nFontSize=18");
    CHECK(result.diagnostics.size() == 2); CHECK(result.config.editor.strokeWidth == 4); CHECK(result.config.editor.fontSize == 20);
}
TEST_CASE("Config font names retain Unicode and reject empty or oversized faces") {
    CHECK(ParseSettings("[Editor]\nFontName=微软雅黑").config.editor.fontName == L"微软雅黑");
    CHECK_FALSE(ParseSettings("[Editor]\nFontName=").diagnostics.empty());
    CHECK_FALSE(ParseSettings("[Editor]\nFontName=" + std::string(32, 'a')).diagnostics.empty());
}
TEST_CASE("Config enum choices are case insensitive") {
    const auto result = ParseSettings("[Save]\nFormat=JPG\n[Pin]\nWheel=Opacity\nCompactMode=THUMBNAIL\n[General]\nTrayClick=GIF\nLanguage=zh-CN");
    CHECK(result.diagnostics.empty()); CHECK(result.config.save.format == vispick::SaveFormat::Jpg); CHECK(result.config.pin.wheel == vispick::PinWheel::Opacity);
    CHECK(result.config.pin.compactMode == vispick::CompactMode::Thumbnail); CHECK(result.config.general.trayClick == vispick::TrayClick::Gif); CHECK(result.config.general.language == vispick::Language::Chinese);
}
TEST_CASE("Config unsupported enum values retain previous settings") {
    vispick::Config previous; previous.save.format = vispick::SaveFormat::Bmp; previous.general.language = vispick::Language::English;
    const auto result = ParseSettings("[Save]\nFormat=gif\n[General]\nLanguage=de", &previous);
    CHECK(result.diagnostics.size() == 2); CHECK(result.config.save.format == vispick::SaveFormat::Bmp); CHECK(result.config.general.language == vispick::Language::English);
}
TEST_CASE("Config all alternate tray actions parse") {
    CHECK(ParseSettings("[General]\nTrayClick=pin").config.general.trayClick == vispick::TrayClick::Pin);
    CHECK(ParseSettings("[General]\nTrayClick=none").config.general.trayClick == vispick::TrayClick::None);
}
TEST_CASE("Config all hotkey fields parse and invalid Ctrl plus retains prior binding") {
    vispick::Config previous; previous.hotkeys.gif = {MOD_ALT, VK_F4};
    const auto result = ParseSettings("[Hotkeys]\nScreenshot=Alt+A\nGif=Ctrl+\nPinClipboard=F12\nTogglePins=Ctrl++", &previous);
    REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 3);
    CHECK(result.config.hotkeys.screenshot == vispick::Hotkey{MOD_ALT, 'A'}); CHECK(result.config.hotkeys.gif == previous.hotkeys.gif);
    CHECK(result.config.hotkeys.pinClipboard.virtualKey == VK_F12); CHECK(result.config.hotkeys.togglePins.virtualKey == VK_OEM_PLUS);
}
TEST_CASE("Config ignores the retired Pin hotkey in an existing INI") {
    for (const auto* value : {"Ctrl+3", "Ctrl+"}) {
        const auto result = ParseSettings(std::string("[Hotkeys]\nPin=") + value + "\nGif=Alt+F2\n[Pin]\nInstant=true");
        REQUIRE(result.accepted);
        CHECK(result.diagnostics.empty());
        CHECK(result.config.hotkeys.screenshot == vispick::Hotkey{MOD_CONTROL, '1'});
        CHECK(result.config.hotkeys.gif == vispick::Hotkey{MOD_ALT, VK_F2});
        CHECK(result.config.pin.instant);
    }
}
TEST_CASE("Config GIF accepts minimum maximum and auto scale") {
    CHECK(ParseSettings("[Gif]\nFps=1\nScale=1").diagnostics.empty());
    const auto result = ParseSettings("[Gif]\nFps=50\nScale=Auto");
    CHECK(result.diagnostics.empty()); CHECK(result.config.gif.fps == 50); CHECK(result.config.gif.scale == 0);
}
TEST_CASE("Config GIF rejects out-of-range FPS and numeric scale") {
    const auto result = ParseSettings("[Gif]\nFps=51\nScale=0");
    CHECK(result.diagnostics.size() == 2); CHECK(result.config.gif.fps == 30); CHECK(result.config.gif.scale == 100);
}
TEST_CASE("Config ignores the legacy GIF duration limit when upgrading an INI") {
    for (const auto* value : {"60", "600"}) {
        const auto result = ParseSettings(std::string("[Gif]\nMaxSeconds=") + value + "\nFps=24");
        CHECK(result.diagnostics.empty());
        CHECK(result.config.gif.maxSeconds == 300);
        CHECK(result.config.gif.fps == 24);
    }
}
TEST_CASE("Config numeric resource limits enforce their upper bounds") {
    const auto result = ParseSettings("[Editor]\nMosaicSize=257\n[Pin]\nZoomStep=101\nSnapDistance=257\nDustBox=101\nCompactSize=2049\n[Save]\nJpgQuality=101");
    CHECK(result.diagnostics.size() == 6);
}
TEST_CASE("Config numeric resource limits accept documented endpoints") {
    const auto result = ParseSettings("[Editor]\nMosaicSize=256\n[Pin]\nZoomStep=100\nSnapDistance=0\nDustBox=0\nCompactSize=2048\n[Save]\nJpgQuality=1");
    CHECK(result.diagnostics.empty()); CHECK(result.config.pin.dustBox == 0); CHECK(result.config.editor.mosaicSize == 256);
}
TEST_CASE("Config duplicate keys diagnose the last source line and use the last value") {
    const auto result = ParseSettings("[Pin]\nOpacity=20\nOPACITY=30");
    CHECK(result.config.pin.opacity == 30); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 3);
}
TEST_CASE("Config invalid save placeholders preserve previous values") {
    vispick::Config previous; previous.save.folder = L"{desktop}"; previous.save.fileName = L"Shot_{ms}";
    const auto result = ParseSettings("[Save]\nFolder={unknown}\nFileName=bad:{unknown}", &previous);
    CHECK(result.diagnostics.size() == 2); CHECK(result.config.save.folder == previous.save.folder); CHECK(result.config.save.fileName == previous.save.fileName);
}
TEST_CASE("Config valid folder and filename templates stay unexpanded until save") {
    const auto result = ParseSettings("[Save]\nFolder={desktop}\\shots\nFileName={date}_{time}_{ms}_{w}x{h}");
    CHECK(result.diagnostics.empty()); CHECK(result.config.save.folder == L"{desktop}\\shots"); CHECK(result.config.save.fileName == L"{date}_{time}_{ms}_{w}x{h}");
}
TEST_CASE("Config failed file load returns previous snapshot and a file diagnostic") {
    vispick::Config previous; previous.pin.opacity = 27;
    const auto result = vispick::LoadConfig(L"Z:\\VisPick-missing-unit-test-directory\\config.ini", &previous);
    CHECK_FALSE(result.accepted); CHECK(result.config.pin.opacity == 27); REQUIRE(result.diagnostics.size() == 1); CHECK(result.diagnostics[0].line == 0);
}
