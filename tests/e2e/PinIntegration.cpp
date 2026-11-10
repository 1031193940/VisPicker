// Expose the optional Windows 10 COM interface in this test only. Product
// binaries retain their Windows 7 target and do not depend on this service.
#define NTDDI_VERSION 0x0A000000
#include "pin/PinManager.h"
#include "capture/ScreenGrab.h"
#include "platform/Compat.h"
#include "platform/Window.h"

#include <dwmapi.h>
#include <psapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <vector>

namespace {

void Require(const bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }

std::uint32_t Pixel(const int column, const int row) {
    return 0xFF000000U | ((static_cast<unsigned>(column * 17 + row * 3) & 255U) << 16U) |
        ((static_cast<unsigned>(column * 7 + row * 11) & 255U) << 8U) | (static_cast<unsigned>(column + row * 13) & 255U);
}

vispick::Bitmap MakeImage(const int width, const int height) {
    vispick::Bitmap image(width, height);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) { image.Row(row)[column] = Pixel(column, row); }
    }
    return image;
}

class TestOwner final : public vispick::Window {
public:
    TestOwner() { Create({L"VisPick.Tests.PinOwner", 0, IDC_ARROW}, L"Pin test owner", WS_POPUP,
        WS_EX_TOOLWINDOW, {0, 0, 1, 1}, vispick::WindowActivation::Passive); }
    ~TestOwner() override { Destroy(); }
    vispick::PinManager* pins = nullptr;
protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == vispick::PinManager::PendingMessage && pins) { pins->ProcessPending(); return 0; }
        return Window::OnMessage(message, parameter, data);
    }
};

struct PinSearch { HWND owner; std::vector<HWND> handles; bool failed = false; };
BOOL CALLBACK FindPins(const HWND window, const LPARAM data) noexcept {
    auto& search = *reinterpret_cast<PinSearch*>(data);
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || thread != GetWindowThreadProcessId(search.owner, nullptr)) { return TRUE; }
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (std::wcscmp(name, L"VisPick.Pin") == 0) {
        try { search.handles.push_back(window); }
        catch (...) { search.failed = true; return FALSE; }
    }
    return TRUE;
}

std::vector<HWND> Handles(const HWND owner) {
    PinSearch search{owner, {}, false};
    Require(EnumWindows(FindPins, reinterpret_cast<LPARAM>(&search)) && !search.failed, "Cannot enumerate test-owned pins");
    return std::move(search.handles);
}

RECT Bounds(const HWND window) { RECT bounds{}; Require(GetWindowRect(window, &bounds) != FALSE, "Get pin bounds failed"); return bounds; }

struct OtherDesktopSearch {
    IVirtualDesktopManager* manager;
    GUID current{};
    GUID other{};
};

BOOL CALLBACK FindOtherDesktop(const HWND window, const LPARAM data) noexcept {
    auto& search = *reinterpret_cast<OtherDesktopSearch*>(data);
    GUID desktop{};
    if (SUCCEEDED(search.manager->GetWindowDesktopId(window, &desktop)) &&
        desktop != GUID_NULL && desktop != search.current) {
        search.other = desktop;
        return FALSE;
    }
    return TRUE;
}

void CheckPattern(HWND pin, const char* stage, const RECT rectangle) {
    const BOOL gdiFlush = GdiFlush();
    const HRESULT compositor = DwmFlush();
    const auto captured = vispick::CaptureScreen(rectangle);
    for (int row = 0; row < captured->Image().Height(); ++row) {
        for (int column = 0; column < captured->Image().Width(); ++column) {
            const auto actual = captured->Image().Row(row)[column] & 0xFFFFFFU;
            const auto expected = Pixel(column, row) & 0xFFFFFFU;
            if (actual == expected) { continue; }
            const POINT location{rectangle.left + column, rectangle.top + row};
            const auto hit = WindowFromPoint(location);
            DWORD hitProcess = 0;
            char hitClass[96]{};
            if (hit) { GetWindowThreadProcessId(hit, &hitProcess); GetClassNameA(hit, hitClass, sizeof(hitClass)); }
            const RECT pinBounds = Bounds(pin);
            std::fprintf(stderr, "Pin pixel mismatch stage=%s image=(%d,%d) screen=(%ld,%ld) expected=%06X actual=%06X "
                "pin_bounds=(%ld,%ld,%ld,%ld) pin_visible=%d hit_pin=%d hit_pid=%lu hit_class=%s gdi_flush=%d dwm_flush=0x%08lX\n",
                stage, column, row, location.x, location.y, expected, actual,
                pinBounds.left, pinBounds.top, pinBounds.right, pinBounds.bottom, IsWindowVisible(pin), hit == pin,
                hitProcess, hitClass, gdiFlush, static_cast<unsigned long>(compositor));
            throw std::runtime_error("Pin physical pixels do not match the original image");
        }
    }
    std::printf("Pin exact pixels: %s\n", stage);
}

class ThreadKeys {
public:
    explicit ThreadKeys(const int modifier = 0) {
        Require(GetKeyboardState(previous_.data()) != FALSE, "Get thread keyboard state failed");
        auto keys = previous_;
        for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU}) {
            keys[key] = 0;
        }
        if (modifier) { keys[modifier] = 0x80; }
        Require(SetKeyboardState(keys.data()) != FALSE, "Set thread keyboard state failed");
    }
    ~ThreadKeys() { SetKeyboardState(previous_.data()); }
private:
    std::array<BYTE, 256> previous_{};
};

std::uint64_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE,
        "Read process memory failed");
    return memory.PrivateUsage;
}

std::uint64_t CpuTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "Read process CPU time failed");
    const auto value = [](const FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime; };
    return value(kernel) + value(user);
}

void Run() {
    vispick::InitializeDpiAwareness();
    TestOwner owner;
    bool notifiedFailure = false;
    vispick::PinManager manager(owner.Handle(), {}, [&](std::wstring, const bool warning) { notifiedFailure = notifiedFailure || warning; });
    owner.pins = &manager;
    vispick::Config::Pin config;
    config.opacity = 100; config.inactiveOpacity = 100;
    config.shadow = true; config.border = true; config.snapDistance = 0;
    manager.Configure(config);
    const auto monitors = vispick::EnumerateMonitors();
    Require(!monitors.empty(), "No monitor available for pin integration");
    const RECT work = monitors.front().workArea;
    POINT origin{work.left + 120, work.top + 120};
    manager.Add(MakeImage(128, 96), origin, config);
    auto handles = Handles(owner.Handle());
    Require(handles.size() == 1, "Expected one reference window");
    const HWND pin = handles.front();
    RECT window = Bounds(pin);
    const LONG margin = origin.x - window.left;
    Require(origin.y - window.top == margin && margin >= 0, "Pin decoration changed physical image alignment");
    CheckPattern(pin, "initial native pin", {origin.x, origin.y, origin.x + 128, origin.y + 96});

    // Query the real Shell, not just style bits: a TOOLWINDOW owner can still
    // exclude an otherwise ordinary pin from virtual desktop management.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Require(SUCCEEDED(apartment), "Initialize virtual desktop test COM failed");
    struct ApartmentScope { ~ApartmentScope() { CoUninitialize(); } } apartmentScope;
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> desktops;
    const HRESULT desktopService = CoCreateInstance(__uuidof(VirtualDesktopManager), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(desktops.GetAddressOf()));
    GUID initialDesktop{};
    const auto waitForDesktop = [&](const bool assigned, const GUID* expectedDesktop = nullptr) {
        const auto started = GetTickCount64();
        do {
            vispick::PumpWindowMessages();
            GUID desktop{};
            BOOL current = FALSE;
            DWORD cloaked = 1;
            if (SUCCEEDED(desktops->GetWindowDesktopId(pin, &desktop)) &&
                (desktop != GUID_NULL) == assigned &&
                (!expectedDesktop || desktop == *expectedDesktop) &&
                SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(pin, &current)) && (assigned || current) &&
                SUCCEEDED(DwmGetWindowAttribute(pin, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
                (cloaked == 0) == (current != FALSE)) { return true; }
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() - started < 4000);
        return false;
    };
    if (SUCCEEDED(desktopService)) {
        Require(waitForDesktop(true), "Default pin is not assigned to a virtual desktop");
        OtherDesktopSearch search{desktops.Get()};
        Require(SUCCEEDED(desktops->GetWindowDesktopId(pin, &search.current)), "Read pin desktop failed");
        initialDesktop = search.current;
        EnumWindows(FindOtherDesktop, reinterpret_cast<LPARAM>(&search));
        if (search.other != GUID_NULL) {
            // Move only this test's window. The user's active desktop and
            // application windows stay untouched; no global keys are sent.
            Require(SUCCEEDED(desktops->MoveWindowToDesktop(pin, search.other)), "Move test pin to other desktop failed");
            const auto waitForOtherDesktop = [&] {
                const auto started = GetTickCount64();
                do {
                    vispick::PumpWindowMessages();
                    BOOL current = TRUE;
                    DWORD cloaked = 0;
                    if (SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(pin, &current)) && !current &&
                        SUCCEEDED(DwmGetWindowAttribute(pin, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) { return true; }
                    MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                } while (GetTickCount64() - started < 4000);
                return false;
            };
            Require(waitForOtherDesktop(), "Pin on another desktop remains onscreen");
            manager.HideAll(); manager.ShowAll();
            Require(waitForOtherDesktop(), "Hide/show moved the pin onto the current desktop");
            std::puts("Pin on another desktop: Shell cloak and hide/show membership passed");
        } else { std::puts("No other desktop with a window found; cross-desktop cloak check skipped"); }
    } else { std::puts("Virtual desktop service unavailable; desktop membership check skipped"); }

    const HWND foreground = GetForegroundWindow();
    const RECT beforeDesktopToggle = Bounds(pin);
    const auto beforeDesktopMemory = manager.Memory();
    config.acrossDesktops = true; manager.Configure(config);
    Require(IsWindowVisible(pin) != FALSE, "Enabling across desktops hid the pin");
    if (desktops) { Require(waitForDesktop(false), "Across-desktop pin remains bound to one desktop"); }
    config.acrossDesktops = false; manager.Configure(config);
    if (desktops) { Require(waitForDesktop(true), "Disabling across desktops did not reassign the pin"); }
    const RECT afterDesktopToggle = Bounds(pin);
    Require(EqualRect(&beforeDesktopToggle, &afterDesktopToggle) != FALSE &&
        manager.Memory().sourceBytes == beforeDesktopMemory.sourceBytes &&
        manager.Memory().renderContentBytes == beforeDesktopMemory.renderContentBytes,
        "Changing desktop mode lost pin position or image storage");
    Require(GetForegroundWindow() == foreground, "Changing desktop mode stole foreground focus");
    if (desktops) {
        // The Shell may retain the previous desktop across a quick toggle.
        // Bring only the test pin back before the remaining pixel checks.
        Require(SUCCEEDED(desktops->MoveWindowToDesktop(pin, initialDesktop)), "Return test pin to its initial desktop failed");
        Require(waitForDesktop(true, &initialDesktop), "Returned pin did not settle on its initial desktop");
    }
    manager.HideAll();
    config.acrossDesktops = true; manager.Configure(config);
    Require(!IsWindowVisible(pin), "Changing desktop mode revealed a hidden pin");
    config.acrossDesktops = false; manager.Configure(config);
    Require(!IsWindowVisible(pin), "Disabling across desktops revealed a hidden pin");
    manager.ShowAll();
    if (desktops) { Require(waitForDesktop(true), "Showing a hidden pin lost desktop membership"); }
    CheckPattern(pin, "after virtual desktop options", {origin.x, origin.y, origin.x + 128, origin.y + 96});
    std::puts("Pin desktop mode: default, enable, disable and hidden-window updates passed");

    // Direct window messages only. Keyboard state changes are private to this
    // test's UI thread and are restored by RAII; no global input is injected.
    const ThreadKeys neutralKeys;
    SendMessageW(pin, WM_KEYDOWN, VK_RIGHT, 0); ++origin.x;
    {
        const ThreadKeys shift(VK_SHIFT);
        SendMessageW(pin, WM_KEYDOWN, VK_DOWN, 0); origin.y += 50;
    }
    CheckPattern(pin, "after keyboard nudge", {origin.x, origin.y, origin.x + 128, origin.y + 96});
    const POINT clicked{47, 33};
    SendMessageW(pin, WM_LBUTTONDBLCLK, 0, MAKELPARAM(clicked.x + margin, clicked.y + margin));
    const RECT compact = Bounds(pin);
    const int side = MulDiv(config.compactSize, static_cast<int>(vispick::DpiForWindow(pin)), 96);
    Require(compact.right - compact.left == side && compact.bottom - compact.top == side, "Compact pin logical size mismatch");
    Require(compact.left + side / 2 == origin.x + clicked.x && compact.top + side / 2 == origin.y + clicked.y,
        "Compact pin is not centered on the double-clicked image point");
    SendMessageW(pin, WM_KEYDOWN, VK_RIGHT, 0); ++origin.x;
    SendMessageW(pin, WM_KEYDOWN, VK_UP, 0); --origin.y;
    SendMessageW(pin, WM_KEYDOWN, VK_RETURN, 0);
    window = Bounds(pin);
    Require(window.left + margin == origin.x && window.top + margin == origin.y, "Compact move and restore changed image anchor");
    CheckPattern(pin, "after compact restore", {origin.x, origin.y, origin.x + 128, origin.y + 96});

    const POINT anchor{origin.x + 60, origin.y + 40};
    SendMessageW(pin, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(anchor.x, anchor.y));
    const RECT zoomed = Bounds(pin);
    Require(std::abs(zoomed.left + margin + 60 * 1.1 - anchor.x) <= 1 &&
        std::abs(zoomed.top + margin + 40 * 1.1 - anchor.y) <= 1, "Wheel zoom moved the anchored pixel more than one physical pixel");
    SendMessageW(pin, WM_TIMER, 1, 0); // Deterministic high-quality completion, cancels the pending timer.
    SendMessageW(pin, WM_KEYDOWN, '0', 0);

    manager.HideAll(); Require(!IsWindowVisible(pin), "HideAll did not hide pin");
    manager.ShowAll(); Require(IsWindowVisible(pin) != FALSE, "ShowAll did not show pin");
    manager.ToggleAll(); Require(!IsWindowVisible(pin), "ToggleAll did not hide pins");
    manager.ToggleAll(); Require(IsWindowVisible(pin) != FALSE, "ToggleAll did not show pins");
    SetWindowLongPtrW(pin, GWL_EXSTYLE, GetWindowLongPtrW(pin, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    manager.ResetPassthrough();
    Require((GetWindowLongPtrW(pin, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0, "Tray passthrough reset did not restore mouse interaction");

    manager.CloseAll(); manager.ProcessPending();
    Require(manager.Memory().windows == 0, "CloseAll retained an open window");
    manager.RestoreLast(); manager.ProcessPending();
    Require(manager.Memory().windows == 1, "RestoreLast did not recover the closed pin");
    manager.CloseAll(); manager.ProcessPending();

    // Real 20 x 1080p images. Distinguish the retained cache contract from OS,
    // heap and window bookkeeping instead of claiming all process bytes are pixels.
    config.dustBox = 0; manager.Configure(config);
    const auto privateBefore = PrivateBytes();
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int index = 0; index < 20; ++index) { manager.Add(MakeImage(1920, 1080), {work.left + 20 + index * 4, work.top + 20 + index * 4}, config); }
    const auto memory = manager.Memory();
    constexpr std::uint64_t expectedSources = 20ULL * 1920 * 1080 * 4;
    Require(memory.windows == 20 && memory.logicalSourceBytes == expectedSources, "Twenty-pin logical source memory count mismatch");
    Require(memory.sourceBytes == expectedSources * 3 / 4, "Opaque pin sources were not retained as lossless BGR24");
    Require(memory.renderContentBytes <= expectedSources, "Pins retained more than source plus one render content cache");
    Require(memory.recycledBytes == 0, "Disabled recycle bin retained source pixels");
    const auto privateAfter = PrivateBytes();
    vispick::PumpWindowMessages();
    const auto cpuBefore = CpuTime();
    const auto idleBegin = GetTickCount64();
    // Bound the observation interval; no busy message polling or UI interaction.
    for (;;) {
        const auto observed = GetTickCount64() - idleBegin;
        if (observed >= 1000) { break; }
        const auto remaining = 1000 - observed;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(remaining), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        vispick::PumpWindowMessages();
    }
    const auto elapsed = GetTickCount64() - idleBegin;
    const auto cpuAfter = CpuTime();
    std::printf("20x1920x1080 retained: logical-source=%llu actual-source=%llu render-content=%llu decoration=%llu private-delta=%lld bytes\n",
        memory.logicalSourceBytes, memory.sourceBytes, memory.renderContentBytes, memory.decorationBytes,
        static_cast<long long>(privateAfter) - static_cast<long long>(privateBefore));
    Require(static_cast<long long>(privateAfter) - static_cast<long long>(privateBefore) <= static_cast<long long>(expectedSources * 2),
        "Twenty 1080p pins exceed twice their logical image bytes in actual process private memory");
    std::printf("idle observation=%llu ms CPU-time=%llu us (environment-dependent; no pass claim for physical idle CPU)\n",
        elapsed, (cpuAfter - cpuBefore) / 10);
    manager.CloseAll(); manager.ProcessPending();
    vispick::PumpWindowMessages();
    const auto gdiDelta = static_cast<long long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - gdiBefore;
    const auto userDelta = static_cast<long long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - userBefore;
    Require(std::abs(gdiDelta) <= 5 && std::abs(userDelta) <= 5, "Closing twenty pins leaked GUI resources");
    std::printf("20 pin close lifecycle: GDI delta=%lld USER delta=%lld\n", gdiDelta, userDelta);

    // This exceeds the old desktop-derived budget in a 1080p/1440p session.
    // Layered decoration must leave a 4K source at its exact physical size.
    manager.Add(MakeImage(3840, 2160), origin, config);
    const auto nativeMemory = manager.Memory();
    Require(nativeMemory.renderContentBytes == 3840ULL * 2160 * 4, "A native 4K pin was silently scaled");
    const auto largePin = Handles(owner.Handle()).front();
    const RECT largeBounds = Bounds(largePin);
    SendMessageW(largePin, WM_ACTIVATE, WA_ACTIVE, 0);
    SendMessageW(largePin, WM_ACTIVATE, WA_INACTIVE, 0);
    const RECT afterActivation = Bounds(largePin);
    Require(EqualRect(&largeBounds, &afterActivation) != FALSE && manager.Memory().renderContentBytes == nativeMemory.renderContentBytes,
        "Activation changed native pin dimensions");
    manager.CloseAll(); manager.ProcessPending();

    config.dustBox = 5; manager.Configure(config);
    for (int index = 0; index < 7; ++index) { manager.Add(MakeImage(32, 24), {work.left + index * 10, work.top + index * 10}, config); }
    manager.CloseAll(); manager.ProcessPending();
    Require(manager.Memory().recycledBytes == 5ULL * 32 * 24 * 3, "Recycle bin did not retain exactly the last five packed sources");
    for (int index = 0; index < 6; ++index) { manager.RestoreLast(); manager.ProcessPending(); }
    Require(manager.Memory().windows == 5 && manager.Memory().recycledBytes == 0, "Recycle restoration count mismatch");
    Require(!notifiedFailure && vispick::Window::ExceptionCount() == 0, "Pin command or window callback reported an exception");
    config.dustBox = 0; manager.Configure(config); manager.CloseAll(); manager.ProcessPending();
    owner.pins = nullptr;
}

} // namespace

int wmain() {
    try { Run(); std::puts("Pin integration passed"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "Pin integration failed: %s\n", error.what()); return 1; }
}
