#include "core/Config.h"
#include "core/Paths.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace vispick {
namespace {

bool ParseInteger(std::wstring_view text, int minimum, int maximum, int& value) noexcept {
    if (text.empty()) { return false; }
    unsigned parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') { return false; }
        const unsigned digit = static_cast<unsigned>(character - L'0');
        if (digit > static_cast<unsigned>(maximum) || parsed > (static_cast<unsigned>(maximum) - digit) / 10) { return false; }
        parsed = parsed * 10 + digit;
        if (parsed > static_cast<unsigned>(maximum)) { return false; }
    }
    if (parsed < static_cast<unsigned>(minimum)) { return false; }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseColor(std::wstring_view text, std::uint32_t& value) noexcept {
    if (text.size() != 7 || text.front() != L'#') { return false; }
    std::uint32_t parsed = 0;
    for (const wchar_t character : text.substr(1)) {
        unsigned digit = 0;
        if (character >= L'0' && character <= L'9') { digit = static_cast<unsigned>(character - L'0'); }
        else if (character >= L'a' && character <= L'f') { digit = static_cast<unsigned>(character - L'a') + 10; }
        else if (character >= L'A' && character <= L'F') { digit = static_cast<unsigned>(character - L'A') + 10; }
        else { return false; }
        parsed = parsed * 16 + digit;
    }
    value = parsed;
    return true;
}

bool KnownSection(std::wstring_view section) {
    return section == L"hotkeys" || section == L"capture" || section == L"save" ||
        section == L"editor" || section == L"pin" || section == L"gif" || section == L"advanced" || section == L"general";
}

} // namespace

ConfigLoadResult ParseConfig(std::span<const std::uint8_t> bytes, const Config* previous) {
    ConfigLoadResult result;
    const Config defaults;
    const Config& retained = previous != nullptr ? *previous : defaults;
    auto document = ParseIni(bytes, CP_ACP);
    result.config = retained;
    result.diagnostics = std::move(document.diagnostics);
    if (!document.valid) {
        result.accepted = false;
        return result;
    }
    if (document.sections.empty()) {
        result.accepted = false;
        result.retryable = true;
        result.diagnostics.push_back({0, L"配置文件为空，保留上次有效设置。"});
        return result;
    }
    for (const auto& section : document.sections) {
        if (!KnownSection(section.name)) { result.diagnostics.push_back({section.line, L"未知配置节：[" + section.name + L"]。"}); }
    }
    std::set<std::wstring> encountered;
    for (const auto& entry : document.entries) {
        if (!KnownSection(entry.section)) { continue; }
        const auto fullKey = entry.section + L"." + entry.key;
        if (!encountered.insert(fullKey).second) { result.diagnostics.push_back({entry.line, L"重复配置键：" + fullKey + L"；以最后一项为准。"}); }
        const auto normalized = LowerAscii(entry.value);
        std::wstring error;
        bool recognized = true;
        auto boolean = [&](bool& target, bool prior) {
            if (normalized == L"true") { target = true; }
            else if (normalized == L"false") { target = false; }
            else { target = prior; error = L"只接受 true 或 false。"; }
        };
        auto integer = [&](int& target, int prior, int minimum, int maximum) {
            if (!ParseInteger(entry.value, minimum, maximum, target)) {
                target = prior;
                error = L"需要 " + std::to_wstring(minimum) + L"–" + std::to_wstring(maximum) + L" 的整数。";
            }
        };
        auto color = [&](std::uint32_t& target, std::uint32_t prior) {
            if (!ParseColor(entry.value, target)) { target = prior; error = L"颜色须为 #RRGGBB。"; }
        };
        auto hotkey = [&](Hotkey& target, Hotkey prior) {
            const auto parsed = ParseHotkey(entry.value);
            if (parsed.value) {
                target = *parsed.value;
                const auto key = target.virtualKey;
                if (target.modifiers == 0 && ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
                    key == VK_SPACE || key == VK_ESCAPE || key == VK_RETURN || key == VK_TAB || key == VK_BACK)) {
                    error = L"未设置修饰键，该按键将被全局占用。";
                }
            }
            else { target = prior; error = parsed.error; }
        };
        auto choice = [&]<typename Value>(Value& target, Value prior,
            std::initializer_list<std::pair<std::wstring_view, Value>> choices) {
            for (const auto& [name, value] : choices) { if (normalized == name) { target = value; return; } }
            target = prior;
            error = L"值必须为：";
            for (const auto& [name, value] : choices) { static_cast<void>(value); error += std::wstring(name) + L" / "; }
            error.resize(error.size() - 3);
            error += L"。";
        };
        auto& config = result.config;
        const auto& key = entry.key;
        if (entry.section == L"hotkeys") {
            if (key == L"screenshot") { hotkey(config.hotkeys.screenshot, retained.hotkeys.screenshot); }
            else if (key == L"gif") { hotkey(config.hotkeys.gif, retained.hotkeys.gif); }
            else if (key == L"pinclipboard") { hotkey(config.hotkeys.pinClipboard, retained.hotkeys.pinClipboard); }
            else if (key == L"togglepins") { hotkey(config.hotkeys.togglePins, retained.hotkeys.togglePins); }
            else { recognized = key == L"pin"; } // Ignore the retired shortcut in older files.
        } else if (entry.section == L"capture") {
            if (key == L"windowsnap") { boolean(config.capture.windowSnap, retained.capture.windowSnap); }
            else if (key == L"magnifier") { boolean(config.capture.magnifier, retained.capture.magnifier); }
            else if (key == L"maskopacity") { integer(config.capture.maskOpacity, retained.capture.maskOpacity, 0, 100); }
            else if (key == L"includecursor") { boolean(config.capture.includeCursor, retained.capture.includeCursor); }
            else { recognized = false; }
        } else if (entry.section == L"save") {
            if (key == L"folder") {
                try {
                    // Validate syntax against stable synthetic locations; resolve actual known folders at use.
                    static_cast<void>(ExpandSaveFolder(entry.value, {L"C:\\Pictures", L"C:\\Desktop", L"C:\\VisPick"}));
                    config.save.folder = entry.value;
                } catch (const std::invalid_argument&) { config.save.folder = retained.save.folder; error = L"目录模板无效：仅支持 {pictures}、{desktop}、{exe_dir}、已定义的 %环境变量% 和绝对路径。"; }
            } else if (key == L"filename") {
                try {
                    const SYSTEMTIME example{2026, 1, 0, 2, 3, 4, 5, 6};
                    static_cast<void>(ExpandFileName(entry.value, example, 1920, 1080));
                    config.save.fileName = entry.value;
                } catch (const std::invalid_argument&) { config.save.fileName = retained.save.fileName; error = L"文件名模板无效：仅支持 {date}、{time}、{ms}、{w}、{h}，且不得含路径或非法文件名字符。"; }
            } else if (key == L"format") { choice(config.save.format, retained.save.format, {{L"png", SaveFormat::Png}, {L"jpg", SaveFormat::Jpg}, {L"bmp", SaveFormat::Bmp}}); }
            else if (key == L"jpgquality") { integer(config.save.jpgQuality, retained.save.jpgQuality, 1, 100); }
            else { recognized = false; }
        } else if (entry.section == L"editor") {
            if (key == L"color") { color(config.editor.color, retained.editor.color); }
            else if (key == L"strokewidth") {
                integer(config.editor.strokeWidth, retained.editor.strokeWidth, 2, 6);
                if (error.empty() && config.editor.strokeWidth % 2 != 0) { config.editor.strokeWidth = retained.editor.strokeWidth; error = L"线宽只接受 2 / 4 / 6。"; }
            } else if (key == L"fontsize") {
                integer(config.editor.fontSize, retained.editor.fontSize, 14, 28);
                if (error.empty() && config.editor.fontSize != 14 && config.editor.fontSize != 20 && config.editor.fontSize != 28) { config.editor.fontSize = retained.editor.fontSize; error = L"字号只接受 14 / 20 / 28。"; }
            } else if (key == L"fontname") {
                if (entry.value.empty() || entry.value.size() > LF_FACESIZE - 1) { config.editor.fontName = retained.editor.fontName; error = L"字体名称需要 1–31 个 UTF-16 字符。"; }
                else { config.editor.fontName = entry.value; }
            } else if (key == L"mosaicsize") { integer(config.editor.mosaicSize, retained.editor.mosaicSize, 1, 256); }
            else { recognized = false; }
        } else if (entry.section == L"pin") {
            if (key == L"instant") { boolean(config.pin.instant, retained.pin.instant); }
            else if (key == L"topmost") { boolean(config.pin.topmost, retained.pin.topmost); }
            else if (key == L"acrossdesktops") { boolean(config.pin.acrossDesktops, retained.pin.acrossDesktops); }
            else if (key == L"border") { boolean(config.pin.border, retained.pin.border); }
            else if (key == L"bordercolor") { color(config.pin.borderColor, retained.pin.borderColor); }
            else if (key == L"shadow") { boolean(config.pin.shadow, retained.pin.shadow); }
            else if (key == L"opacity") { integer(config.pin.opacity, retained.pin.opacity, 10, 100); }
            else if (key == L"inactiveopacity") { integer(config.pin.inactiveOpacity, retained.pin.inactiveOpacity, 10, 100); }
            else if (key == L"wheel") { choice(config.pin.wheel, retained.pin.wheel, {{L"zoom", PinWheel::Zoom}, {L"opacity", PinWheel::Opacity}}); }
            else if (key == L"zoomstep") { integer(config.pin.zoomStep, retained.pin.zoomStep, 1, 100); }
            else if (key == L"smooth") { boolean(config.pin.smooth, retained.pin.smooth); }
            else if (key == L"snapdistance") { integer(config.pin.snapDistance, retained.pin.snapDistance, 0, 256); }
            else if (key == L"dustbox") { integer(config.pin.dustBox, retained.pin.dustBox, 0, 100); }
            else if (key == L"compactmode") { choice(config.pin.compactMode, retained.pin.compactMode, {{L"crop", CompactMode::Crop}, {L"thumbnail", CompactMode::Thumbnail}}); }
            else if (key == L"compactsize") { integer(config.pin.compactSize, retained.pin.compactSize, 1, 2048); }
            else if (key == L"compactopacity") { integer(config.pin.compactOpacity, retained.pin.compactOpacity, 10, 100); }
            else if (key == L"compactlinecolor") { color(config.pin.compactLineColor, retained.pin.compactLineColor); }
            else { recognized = false; }
        } else if (entry.section == L"gif") {
            if (key == L"fps") { integer(config.gif.fps, retained.gif.fps, 1, 50); }
            else if (key == L"showcursor") { boolean(config.gif.showCursor, retained.gif.showCursor); }
            else if (key == L"highlightclicks") { boolean(config.gif.highlightClicks, retained.gif.highlightClicks); }
            else if (key == L"dither") { boolean(config.gif.dither, retained.gif.dither); }
            else if (key == L"scale") {
                if (normalized == L"auto") { config.gif.scale = 0; }
                else { integer(config.gif.scale, retained.gif.scale, 1, 100); }
            } else { recognized = key == L"maxseconds"; } // Ignore the retired limit in older files.
        } else if (entry.section == L"advanced") {
            if (key == L"capture_layered_gif") { boolean(config.gif.captureLayeredWindows, retained.gif.captureLayeredWindows); }
            else { recognized = false; }
        } else if (entry.section == L"general") {
            if (key == L"trayclick") { choice(config.general.trayClick, retained.general.trayClick, {{L"screenshot", TrayClick::Screenshot}, {L"gif", TrayClick::Gif}, {L"pin", TrayClick::Pin}, {L"none", TrayClick::None}}); }
            else if (key == L"language") { choice(config.general.language, retained.general.language, {{L"auto", Language::Auto}, {L"zh-cn", Language::Chinese}, {L"en", Language::English}}); }
            else if (key == L"log") { boolean(config.general.log, retained.general.log); }
            else { recognized = false; }
        }
        if (!recognized) { error = L"未知配置键。"; }
        if (!error.empty()) { result.diagnostics.push_back({entry.line, fullKey + L"：" + error}); }
    }
    return result;
}

ConfigLoadResult LoadConfig(const std::filesystem::path& path, const Config* previous) {
    ConfigLoadResult failure;
    failure.accepted = false;
    if (previous != nullptr) { failure.config = *previous; }
    // One MiB comfortably exceeds the template while bounding accidental binary input.
    constexpr std::streamoff maximumBytes = 1024 * 1024;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file ? static_cast<std::streamoff>(file.tellg()) : -1;
    if (size < 0 || size > maximumBytes) {
        failure.diagnostics.push_back({0, L"无法读取配置文件，或文件超过 1 MiB；保留已有配置。"});
        return failure;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (size != 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        failure.retryable = true;
        failure.diagnostics.push_back({0, L"配置文件读取不完整；保留已有配置。"});
        return failure;
    }
    return ParseConfig(bytes, previous);
}

void SavePinAcrossDesktops(const std::filesystem::path& path, const bool enabled) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = input ? static_cast<std::streamoff>(input.tellg()) : -1;
    if (size < 0 || size > 1024 * 1024) { throw std::runtime_error("Cannot read configuration to save reference desktop setting"); }
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
            const int count = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!count) { throw std::runtime_error("Cannot encode reference desktop setting"); }
            result.resize(static_cast<std::size_t>(count));
            if (WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()),
                reinterpret_cast<char*>(result.data()), count, nullptr, nullptr) != count) {
                throw std::runtime_error("Cannot encode reference desktop setting");
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
    const auto entry = std::find_if(document.entries.rbegin(), document.entries.rend(), [](const IniEntry& item) {
        return item.section == L"pin" && item.key == L"acrossdesktops";
    });
    if (entry != document.entries.rend()) {
        const auto line = lines.at(entry->line - 1);
        replaceStart = line.start;
        while (replaceStart < line.end && characterAt(replaceStart) != '=') { replaceStart += stride; }
        replaceStart += stride;
        const auto oldValue = encode(entry->value);
        // Locate the parsed value in its original bytes so spacing, casing and
        // comments survive without transcoding the rest of the INI document.
        while (replaceStart + oldValue.size() <= line.end &&
            !std::equal(oldValue.begin(), oldValue.end(), bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart))) { replaceStart += stride; }
        if (replaceStart + oldValue.size() > line.end) { throw std::runtime_error("Cannot locate the existing reference desktop setting"); }
        replaceEnd = replaceStart + oldValue.size();
        replacement = enabled ? L"true" : L"false";
    } else {
        const auto section = std::find_if(document.sections.rbegin(), document.sections.rend(), [](const IniSection& item) { return item.name == L"pin"; });
        if (section != document.sections.rend()) {
            const auto line = lines.at(section->line - 1);
            replaceStart = replaceEnd = line.next;
            if (line.end == line.next) { replacement += newline; }
        } else {
            if (bytes.size() > firstByte && characterAt(bytes.size() - stride) != '\r' && characterAt(bytes.size() - stride) != '\n') { replacement += newline; }
            replacement += L"[Pin]" + newline;
        }
        replacement += enabled ? L"AcrossDesktops=true" : L"AcrossDesktops=false";
        replacement += newline;
    }
    const auto inserted = encode(replacement);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart), bytes.begin() + static_cast<std::ptrdiff_t>(replaceEnd));
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(replaceStart), inserted.begin(), inserted.end());

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
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) { throw std::system_error(static_cast<int>(error), std::system_category(), "Stage reference desktop setting"); }
    }
    if (staged.handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot stage reference desktop setting"); }
    DWORD written = 0;
    if (!WriteFile(staged.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() || !FlushFileBuffers(staged.handle)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Write reference desktop setting");
    }
    CloseHandle(staged.handle); staged.handle = INVALID_HANDLE_VALUE;
    if (!ReplaceFileW(path.c_str(), staged.path.c_str(), nullptr, 0, nullptr, nullptr)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Save reference desktop setting");
    }
    staged.path.clear();
}

} // namespace vispick
