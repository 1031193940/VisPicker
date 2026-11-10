#pragma once

#include "core/Hotkey.h"
#include "core/Ini.h"
#include <filesystem>

namespace vispick {

enum class SaveFormat { Png, Jpg, Bmp };
enum class PinWheel { Zoom, Opacity };
enum class CompactMode { Crop, Thumbnail };
enum class TrayClick { Screenshot, Gif, Pin, None };
enum class Language { Auto, Chinese, English };
enum class NotificationMode { Errors, All, None };

struct Config {
    struct Hotkeys {
        Hotkey screenshot{2, '1'};
        Hotkey gif{2, '2'};
        Hotkey pinClipboard;
        Hotkey togglePins;
        bool operator==(const Hotkeys&) const = default;
    } hotkeys;
    struct Capture {
        bool windowSnap = true;
        bool magnifier = true;
        int maskOpacity = 45;
        bool includeCursor = false;
    } capture;
    struct Save {
        std::wstring folder = L"{pictures}";
        std::wstring fileName = L"VisPick_{date}_{time}";
        SaveFormat format = SaveFormat::Png;
        int jpgQuality = 92;
    } save;
    struct Editor {
        std::uint32_t color = 0xFF3B30; // 0xRRGGBB, not COLORREF.
        int strokeWidth = 4;
        int fontSize = 20;
        std::wstring fontName = L"Microsoft YaHei";
        int mosaicSize = 10;
    } editor;
    struct Pin {
        bool instant = false;
        bool topmost = true;
        bool acrossDesktops = false;
        bool border = true;
        std::uint32_t borderColor = 0x3A7AFE;
        bool shadow = true;
        int opacity = 100;
        int inactiveOpacity = 100;
        PinWheel wheel = PinWheel::Zoom;
        int zoomStep = 10;
        bool smooth = true;
        int snapDistance = 10;
        int dustBox = 5;
        CompactMode compactMode = CompactMode::Crop;
        int compactSize = 50;
        int compactOpacity = 80;
        std::uint32_t compactLineColor = 0x0000FF;
    } pin;
    struct Gif {
        static constexpr int MaximumSeconds = 5 * 60;
        int fps = 30;
        int maxSeconds = MaximumSeconds; // Internal duration limit; tests can use a shorter interval.
        bool showCursor = true;
        bool highlightClicks = false;
        bool dither = false;
        int scale = 100; // 0 denotes "auto"; numeric values are 1..100.
        bool captureLayeredWindows = false;
    } gif;
    struct General {
        TrayClick trayClick = TrayClick::Screenshot;
        Language language = Language::Auto;
        NotificationMode notify = NotificationMode::Errors;
        bool log = false;
    } general;
};

struct ConfigLoadResult {
    Config config;
    std::vector<ConfigDiagnostic> diagnostics;
    bool accepted = true; // false: whole-file failure; config is the previous snapshot.
    bool retryable = false; // A writer may still be replacing or filling the file.
};

// Missing and invalid fields retain their previous valid value, or the default at startup.
// Duplicate keys are diagnosed; the last occurrence wins. Unknown keys are ignored.
[[nodiscard]] ConfigLoadResult ParseConfig(std::span<const std::uint8_t> bytes,
    const Config* previous = nullptr);
[[nodiscard]] ConfigLoadResult LoadConfig(const std::filesystem::path& path,
    const Config* previous = nullptr);

// Atomically update just this preference, preserving the user's other INI bytes.
// Throws on an unreadable, malformed, read-only, or unwritable configuration.
void SavePinAcrossDesktops(const std::filesystem::path& path, bool enabled);

} // namespace vispick
