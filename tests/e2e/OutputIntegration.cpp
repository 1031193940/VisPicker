// Exercises the real Windows clipboard in a private noninteractive window
// station. The user's clipboard and visible desktop are never changed.
#include "imaging/Clipboard.h"
#include "imaging/ImageCodec.h"
#include "gif/GifWriter.h"
#include "gif/GifTrim.h"
#include "platform/Gdi.h"
#include "platform/Compat.h"
#include "platform/Window.h"
#include "recording/GifPreview.h"
#include "recording/RecordingFiles.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Require(bool condition, const char* check) {
    if (!condition) { throw std::runtime_error(check); }
    std::cout << "PASS " << check << '\n';
}
class PrivateClipboard final {
public:
    PrivateClipboard() {
        originalStation_ = GetProcessWindowStation();
        originalDesktop_ = GetThreadDesktop(GetCurrentThreadId());
        // Only administrators may choose a station name. Windows assigns a
        // noninteractive logon-session name for ordinary users with nullptr.
        station_ = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
        if (!station_) { throw std::runtime_error("Create private test window station failed"); }
        wchar_t originalName[256]{}, isolatedName[256]{};
        DWORD required = 0;
        if (!GetUserObjectInformationW(originalStation_, UOI_NAME, originalName, sizeof(originalName), &required) ||
            !GetUserObjectInformationW(station_, UOI_NAME, isolatedName, sizeof(isolatedName), &required) ||
            std::wstring_view(originalName) == isolatedName) {
            CloseWindowStation(station_);
            throw std::runtime_error("Cannot prove clipboard isolation from the interactive window station");
        }
        if (!SetProcessWindowStation(station_)) { CloseWindowStation(station_); throw std::runtime_error("Select private window station failed"); }
        desktop_ = CreateDesktopW(L"OutputTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop_ || !SetThreadDesktop(desktop_)) {
            if (desktop_) { CloseDesktop(desktop_); }
            SetProcessWindowStation(originalStation_); CloseWindowStation(station_);
            throw std::runtime_error("Select private test desktop failed");
        }
    }
    ~PrivateClipboard() {
        SetThreadDesktop(originalDesktop_);
        SetProcessWindowStation(originalStation_);
        CloseDesktop(desktop_); CloseWindowStation(station_);
    }
private:
    HWINSTA originalStation_ = nullptr, station_ = nullptr;
    HDESK originalDesktop_ = nullptr, desktop_ = nullptr;
};
class Owner final : public vispick::Window {
public:
    Owner() { Create({L"VisPick.OutputTest"}, L"", WS_POPUP, WS_EX_TOOLWINDOW, {0,0,1,1}, vispick::WindowActivation::Passive); }
    ~Owner() override { Destroy(); }
};
void PublishFixture(UINT format, std::span<const std::uint8_t> bytes) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) { throw std::bad_alloc(); }
    auto* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); throw std::runtime_error("Lock clipboard fixture failed"); }
    std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);
    if (!SetClipboardData(format, memory)) { GlobalFree(memory); throw std::runtime_error("Publish clipboard fixture failed"); }
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Open GIF clipboard test file failed"); }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

constexpr UINT ImageClipboardReady = WM_APP + 83;

void WaitForImageClipboard(vispick::ImageClipboard& clipboard, HWND owner, bool expectPng) {
    const auto deadline = GetTickCount64() + 10000;
    bool received = false;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, owner, ImageClipboardReady, ImageClipboardReady, PM_REMOVE)) {
            clipboard.PublishReady();
            received = true;
        }
        if (received && (!expectPng || IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")))) { return; }
        Sleep(1);
    }
    throw std::runtime_error("Timed out waiting for image clipboard worker");
}

void CheckAsyncImageCopy(HWND owner, const vispick::Bitmap& pattern) {
    const auto pngFormat = RegisterClipboardFormatW(L"PNG");
    vispick::ImageClipboard clipboard(owner, ImageClipboardReady);
    clipboard.Copy(pattern);
    Require(IsClipboardFormatAvailable(CF_DIBV5) && IsClipboardFormatAvailable(CF_DIB) &&
        !IsClipboardFormatAvailable(pngFormat), "Async copy immediately publishes DIBV5 and DIB before PNG completion");
    const auto immediate = vispick::ReadClipboardImage(owner);
    Require(immediate && std::equal(pattern.Pixels().begin(), pattern.Pixels().end(), immediate->Pixels().begin()),
        "Immediate DIBV5 preserves screenshot pixels and straight alpha");
    WaitForImageClipboard(clipboard, owner, true);
    const auto completed = vispick::ReadClipboardImage(owner);
    Require(completed && std::equal(pattern.Pixels().begin(), pattern.Pixels().end(), completed->Pixels().begin()),
        "Background PNG preserves screenshot pixels and alpha");

    clipboard.Copy(pattern);
    vispick::CopyText(owner, L"后来复制的文字 / latest text");
    WaitForImageClipboard(clipboard, owner, false);
    Require(!IsClipboardFormatAvailable(pngFormat) && !IsClipboardFormatAvailable(CF_DIBV5),
        "Old PNG cannot overwrite newer clipboard data from the same owner");
    Require(OpenClipboard(owner) != FALSE, "open isolated clipboard to check Unicode text");
    const auto textMemory = GetClipboardData(CF_UNICODETEXT);
    const auto* text = textMemory ? static_cast<const wchar_t*>(GlobalLock(textMemory)) : nullptr;
    const bool textMatches = text && std::wstring_view(text) == L"后来复制的文字 / latest text";
    if (text) { GlobalUnlock(textMemory); }
    CloseClipboard();
    Require(textMatches, "CopyText retains Unicode text after stale PNG completion");

    vispick::Bitmap latest;
    for (int iteration = 0; iteration < 32; ++iteration) {
        latest = pattern;
        latest.Pixels()[1] = 0xFF112200U | static_cast<std::uint32_t>(iteration);
        clipboard.Copy(latest);
    }
    WaitForImageClipboard(clipboard, owner, true);
    const auto finalImage = vispick::ReadClipboardImage(owner);
    Require(finalImage && std::equal(latest.Pixels().begin(), latest.Pixels().end(), finalImage->Pixels().begin()),
        "Rapid copies coalesce to the newest image without stale PNG publication");

    {
        vispick::ImageClipboard closing(owner, ImageClipboardReady);
        closing.Copy(pattern);
    }
    const auto afterClose = vispick::ReadClipboardImage(owner);
    Require(afterClose && std::equal(pattern.Pixels().begin(), pattern.Pixels().end(), afterClose->Pixels().begin()),
        "Worker shutdown joins safely and retains the immediate clipboard image");
    MSG message{};
    while (PeekMessageW(&message, owner, ImageClipboardReady, ImageClipboardReady, PM_REMOVE)) {}
}

void CheckGifPreviewCopy(HWND owner) {
    // The executable uses this fixture last. Only its own process environment
    // changes, so clipboard cleanup cannot see production temporary GIFs.
    const auto temporary = std::filesystem::temp_directory_path() / (L"VisPick-output-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(temporary) || !SetEnvironmentVariableW(L"TMP", temporary.c_str()) ||
        !SetEnvironmentVariableW(L"TEMP", temporary.c_str())) {
        throw std::runtime_error("Isolate GIF clipboard fixture files failed");
    }
    for (const bool trim : {false,true}) {
    vispick::recording::TemporaryGif recording(vispick::recording::TemporaryGifKind::Recording);
    {
        vispick::gif::GifWriter writer(recording.path, 2, 1);
        const std::array first{0xFF33AA33U, 0xFF7777AAU};
        const std::array second{0xFFFF1111U, 0xFF773311U};
        writer.WriteFrame(vispick::gif::EncodeFrame(first), 40);
        writer.WriteFrame(vispick::gif::EncodeFrame(second), 60);
        writer.Finish();
    }
    const auto original = ReadFileBytes(recording.path);
    auto expected = original;
    if (trim) {
        vispick::recording::TemporaryGif selected(vispick::recording::TemporaryGifKind::Recording);
        vispick::gif::TrimRecordedGif(recording.path,selected.path,{40,100});
        expected = ReadFileBytes(selected.path);
    }
    {
        vispick::GifPreview preview({recording.path, 2, 1, 2, 100}, {}, owner, WM_APP + 1,
            [](HWND, int, int) -> std::optional<std::filesystem::path> { return std::nullopt; }, true);
        preview.Show();
        RECT client{};
        if (!GetClientRect(preview.Handle(), &client)) { throw std::runtime_error("Read GIF preview bounds failed"); }
        const auto dpi = static_cast<int>(vispick::DpiForWindow(preview.Handle()));
        const auto x = MulDiv(32, dpi, 96);
        const auto y = client.bottom - MulDiv(20, dpi, 96);
        if (trim) {
            SendMessageW(preview.Handle(),WM_KEYDOWN,VK_SPACE,0);
            vispick::DibSection image(client.right,client.bottom);
            const auto waitForFrame = [&](std::uint32_t color) {
                const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline) {
                    vispick::PumpWindowMessages();
                    SendMessageW(preview.Handle(),WM_PRINTCLIENT,reinterpret_cast<WPARAM>(image.DeviceContext()),PRF_CLIENT);
                    const auto row = (client.bottom-MulDiv(100,dpi,96))/2;
                    if (image.Pixels()[static_cast<std::size_t>(row)*image.Width()+client.right/2-1] == color) { return; }
                    Sleep(1);
                }
                throw std::runtime_error("Trim clipboard preview did not display the requested frame");
            };
            waitForFrame(0xFF33AA33U);
            SendMessageW(preview.Handle(),WM_KEYDOWN,VK_OEM_6,0); // Keep the first frame.
            SendMessageW(preview.Handle(),WM_LBUTTONUP,0,MAKELPARAM(MulDiv(158,dpi,96),y)); // Reset.
            const int trackLeft = MulDiv(16,dpi,96);
            const int trackRight = client.right-MulDiv(124,dpi,96)-1;
            const auto seek = MAKELPARAM(trackLeft+(trackRight-trackLeft)*70/100,
                client.bottom-MulDiv(83,dpi,96));
            SendMessageW(preview.Handle(),WM_LBUTTONDOWN,MK_LBUTTON,seek);
            SendMessageW(preview.Handle(),WM_LBUTTONUP,0,seek);
            waitForFrame(0xFFFF1111U);
            SendMessageW(preview.Handle(),WM_KEYDOWN,VK_OEM_4,0); // Keep the second frame.
        }
        SendMessageW(preview.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(x,y));
        if (!preview.Busy()) { throw std::runtime_error("GIF Copy button did not start publishing"); }
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (preview.Busy() && std::chrono::steady_clock::now() < timeout) {
            vispick::PumpWindowMessages();
            Sleep(1);
        }
        Require(!preview.Busy() && !preview.Closed() && std::filesystem::exists(recording.path),
            "GIF Copy completes while preserving the preview and recording");
        Require(ReadFileBytes(recording.path) == original,"GIF Copy leaves original recording bytes unchanged");
    }
    Require(!std::filesystem::exists(recording.path), "Original recording is removed when the preview closes");

    if (!OpenClipboard(owner)) { throw std::runtime_error("Open isolated GIF clipboard failed"); }
    std::filesystem::path copied;
    try {
        const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        const auto count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
        const auto length = count == 1 ? DragQueryFileW(drop, 0, nullptr, 0) : 0;
        std::wstring path(length + 1, L'\0');
        if (!length || DragQueryFileW(drop, 0, path.data(), length + 1) != length) {
            throw std::runtime_error("GIF file drop missing after preview closes");
        }
        path.resize(length);
        copied = std::filesystem::path(path);
        const auto ownPrefix = L"clip_vispick_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        Require(copied.parent_path() == recording.path.parent_path() && copied.filename().native().starts_with(ownPrefix),
            "GIF clipboard file is a separately owned copy");
        const auto effect = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        if (!effect || GlobalSize(effect) < sizeof(DWORD)) { throw std::runtime_error("GIF clipboard drop effect missing"); }
        const auto* dropEffect = static_cast<const DWORD*>(GlobalLock(effect));
        const bool copy = dropEffect && *dropEffect == DROPEFFECT_COPY;
        if (dropEffect) { GlobalUnlock(effect); }
        Require(copy, "GIF file drop requests copy instead of move");
        const auto memory = GetClipboardData(RegisterClipboardFormatW(L"image/gif"));
        if (!memory || GlobalSize(memory) < expected.size()) { throw std::runtime_error("GIF clipboard animation missing"); }
        const auto* data = static_cast<const char*>(GlobalLock(memory));
        const bool matches = data && std::equal(expected.begin(), expected.end(), data);
        if (data) { GlobalUnlock(memory); }
        Require(matches, trim ? "Clipboard image/gif contains only the selected frame" : "Clipboard image/gif bytes retain the complete animation");
        EmptyClipboard();
    } catch (...) { CloseClipboard(); throw; }
    CloseClipboard();
    const auto info = vispick::gif::InspectGif(copied);
    Require(ReadFileBytes(copied) == expected && info.frameCount == (trim ? 1U : 2U) && info.durationCentiseconds == (trim ? 60U : 100U),
        "Clipboard file drop preserves the selected range after the preview closes");
    std::filesystem::remove(copied);
    }
    std::filesystem::remove(temporary / L"VisPick");
    std::filesystem::remove(temporary);
}

void Run() {
    const PrivateClipboard isolated;
    Owner owner;
    vispick::Bitmap pattern(257, 131);
    for (std::size_t pixel = 0; pixel < pattern.Pixels().size(); ++pixel) {
        pattern.Pixels()[pixel] = 0xFF000000U | (static_cast<std::uint32_t>(pixel * 73571) & 0x00FFFFFF);
    }
    pattern.Pixels()[0] = 0x80123456;
    const auto resourcesBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 200; ++iteration) {
        vispick::CopyImage(owner.Handle(), pattern.View());
        const auto png = RegisterClipboardFormatW(L"PNG");
        if (!IsClipboardFormatAvailable(png) || !IsClipboardFormatAvailable(CF_DIB)) {
            throw std::runtime_error("Copy must publish both PNG and DIB");
        }
        const auto copied = vispick::ReadClipboardImage(owner.Handle());
        if (!copied || copied->Width() != pattern.Width() || copied->Height() != pattern.Height() ||
            !std::equal(pattern.Pixels().begin(), pattern.Pixels().end(), copied->Pixels().begin())) {
            throw std::runtime_error("PNG clipboard pixel or alpha mismatch");
        }
    }
    Require(true, "200 real clipboard PNG+DIB copies preserve pixels and alpha");
    const auto resourcesAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    Require(resourcesAfter <= resourcesBefore + 5, "GDI objects return within five of baseline");
    Require(OpenClipboard(owner.Handle()) != FALSE, "open isolated clipboard for fallback fixture");
    Require(EmptyClipboard() != FALSE, "empty isolated clipboard");
    auto bytes = vispick::EncodeDib(pattern.View());
    for (std::size_t alpha = sizeof(BITMAPINFOHEADER) + 3; alpha < bytes.size(); alpha += 4) { bytes[alpha] = 0; }
    try { PublishFixture(CF_DIB, bytes); }
    catch (...) { CloseClipboard(); throw; }
    CloseClipboard();
    const auto dib = vispick::ReadClipboardImage(owner.Handle());
    const auto expected = vispick::DecodeDib(bytes);
    Require(dib && std::equal(dib->Pixels().begin(), dib->Pixels().end(), expected.Pixels().begin()), "DIB-only clipboard fallback is correct");
    Require(OpenClipboard(owner.Handle()) != FALSE, "open isolated clipboard for malformed PNG fixture");
    constexpr std::array<std::uint8_t, 5> malformed{1, 2, 3, 4, 5};
    try { PublishFixture(RegisterClipboardFormatW(L"PNG"), malformed); }
    catch (...) { CloseClipboard(); throw; }
    CloseClipboard();
    const auto fallback = vispick::ReadClipboardImage(owner.Handle());
    Require(fallback && std::equal(fallback->Pixels().begin(), fallback->Pixels().end(), expected.Pixels().begin()),
        "Malformed clipboard PNG falls back to valid DIB pixels");
    Require(OpenClipboard(owner.Handle()) != FALSE, "open isolated clipboard for cleanup");
    EmptyClipboard(); CloseClipboard();
    Require(!vispick::ReadClipboardImage(owner.Handle()), "empty clipboard returns no image");
    CheckAsyncImageCopy(owner.Handle(), pattern);
    CheckGifPreviewCopy(owner.Handle());
}
}
int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
