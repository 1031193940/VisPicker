#pragma once

#include <windows.h>
#include <stdexcept>

namespace vispick::test {

inline void ActivateOwnedWindow(HWND window) {
    DWORD process = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(window, &process);
    const DWORD currentThread = GetCurrentThreadId();
    if (ownerThread != currentThread || process != GetCurrentProcessId() || !IsWindowVisible(window)) {
        throw std::invalid_argument("Foreground test target must be a visible window owned by this thread");
    }
    if (GetForegroundWindow() == window) { return; }
    SetForegroundWindow(window);
    const HWND previous = GetForegroundWindow();
    if (previous == window) { return; }

    // Console-launched fixtures may have no foreground permission. Share the
    // responsive foreground queue only for this synchronous activation attempt.
    // This is confined to the test executable. No keyboard input is injected.
    const DWORD previousThread = previous ? GetWindowThreadProcessId(previous, nullptr) : 0;
    DWORD_PTR response = 0;
    if (previousThread != 0 && previousThread != currentThread &&
        SendMessageTimeoutW(previous, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &response) &&
        AttachThreadInput(currentThread, previousThread, TRUE)) {
        SetForegroundWindow(window);
        if (!AttachThreadInput(currentThread, previousThread, FALSE)) {
            throw std::runtime_error("Could not detach the test foreground input queue");
        }
    }
    if (GetForegroundWindow() != window) {
        throw std::runtime_error("Could not activate the owned test fixture on the interactive desktop");
    }
}

class ForegroundFixture {
public:
    ForegroundFixture() : previous_(GetForegroundWindow()) {}
    ~ForegroundFixture() {
        if (!window_) { return; }
        if (GetForegroundWindow() == window_ && IsWindow(previous_)) { SetForegroundWindow(previous_); }
        DestroyWindow(window_);
    }
    ForegroundFixture(const ForegroundFixture&) = delete;
    ForegroundFixture& operator=(const ForegroundFixture&) = delete;

    void Activate() {
        if (!window_) {
            RECT work{};
            if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) { throw std::runtime_error("Cannot read desktop test work area"); }
            window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"VisPick desktop test", WS_POPUP | WS_CAPTION,
                work.right - 218, work.bottom - 60, 210, 52, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window_) { throw std::runtime_error("Cannot create foreground test fixture"); }
            if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                throw std::runtime_error("Cannot show foreground test fixture");
            }
        }
        ActivateOwnedWindow(window_);
    }

    void GrantTo(DWORD childProcess) {
        if (childProcess == 0 || childProcess == ASFW_ANY) { throw std::invalid_argument("Foreground grant requires the owned child PID"); }
        Activate();
        // Starting from the foreground can already make a child eligible.
        // The following visibility/focus assertions are the actual gate.
        AllowSetForegroundWindow(childProcess);
    }

private:
    HWND previous_ = nullptr;
    HWND window_ = nullptr;
};

} // namespace vispick::test
