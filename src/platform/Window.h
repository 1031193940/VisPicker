#pragma once

#include <windows.h>
#include <cstdint>

namespace vispick {

enum class WindowActivation { Interactive, Passive };

struct WindowClassOptions {
    const wchar_t* name;
    UINT style = CS_DBLCLKS;
    LPCWSTR cursor = IDC_ARROW;
};

struct WindowException {
    HWND window;
    UINT message;
    const char* description; // Valid only during the callback.
};
using WindowExceptionCallback = void (*)(const WindowException&) noexcept;

// All methods, including destruction, must run on the creating UI thread.
class Window {
public:
    Window() = default;
    virtual ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Create(const WindowClassOptions& windowClass, const wchar_t* title,
                DWORD style, DWORD extendedStyle, const RECT& bounds,
                WindowActivation activation, HWND owner = nullptr);
    void Destroy() noexcept;
    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    static void SetExceptionCallback(WindowExceptionCallback callback) noexcept;
    [[nodiscard]] static std::uint64_t ExceptionCount() noexcept;

protected:
    virtual LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data);

private:
    static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM parameter, LPARAM data);
    static void RegisterClass(const WindowClassOptions& options);
    static void ReportException(HWND window, UINT message, const char* description) noexcept;
    HWND window_ = nullptr;
    DWORD creatingThread_ = 0;
};

// Pump pending messages without changing the caller's WM_QUIT state.
void PumpWindowMessages();

} // namespace vispick
