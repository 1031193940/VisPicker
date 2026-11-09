// Exercises the real Windows clipboard in a private noninteractive window
// station. The user's clipboard and visible desktop are never changed.
#include "imaging/Clipboard.h"
#include "imaging/ImageCodec.h"
#include "platform/Window.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
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
}
}
int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
