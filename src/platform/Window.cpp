#include "platform/Window.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace vispick {
namespace {
std::atomic<WindowExceptionCallback> exceptionCallback{nullptr};
std::atomic<std::uint64_t> exceptionCount{0};
} // namespace

Window::~Window() {
    // Derived destructors must destroy HWND before any message-visible state.
    assert(window_ == nullptr && "Call Destroy() in the derived window destructor");
    if (window_) {
        // Release-build last resort: never dispatch into partially destroyed state.
        const HWND detachedWindow = std::exchange(window_, nullptr);
        SetWindowLongPtrW(detachedWindow, GWLP_USERDATA, 0);
        DestroyWindow(detachedWindow);
    }
}

void Window::RegisterClass(const WindowClassOptions& options) {
    if (!options.name || !*options.name) { throw std::invalid_argument("Window class requires a name"); }
    const HINSTANCE module = GetModuleHandleW(nullptr);
    const HCURSOR cursor = options.cursor ? LoadCursorW(nullptr, options.cursor) : nullptr;
    if (options.cursor && !cursor) { throw std::runtime_error("LoadCursorW failed for window class"); }
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(module, options.name, &existing)) {
        if (existing.lpfnWndProc != Dispatch || existing.style != options.style || existing.hCursor != cursor) {
            throw std::invalid_argument("Window class name already has different registration options");
        }
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = options.style;
    windowClass.lpfnWndProc = Dispatch;
    windowClass.hInstance = module;
    windowClass.hCursor = cursor;
    windowClass.lpszClassName = options.name;
    if (!RegisterClassExW(&windowClass)) { throw std::runtime_error("RegisterClassExW failed"); }
}

void Window::Create(const WindowClassOptions& windowClass, const wchar_t* title, const DWORD style, DWORD extendedStyle,
                    const RECT& bounds, const WindowActivation activation, const HWND owner) {
    if (window_) { throw std::logic_error("Window already created"); }
    RegisterClass(windowClass);
    if (activation == WindowActivation::Passive) {
        extendedStyle |= WS_EX_NOACTIVATE;
    } else {
        extendedStyle &= ~static_cast<DWORD>(WS_EX_NOACTIVATE);
    }
    creatingThread_ = GetCurrentThreadId();
    if (!CreateWindowExW(extendedStyle, windowClass.name, title, style, bounds.left, bounds.top,
                         bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr,
                         GetModuleHandleW(nullptr), this)) {
        throw std::runtime_error("CreateWindowExW failed");
    }
}

void Window::Destroy() noexcept {
    if (window_) {
        assert(creatingThread_ == GetCurrentThreadId());
        DestroyWindow(window_);
    }
}

void Window::SetExceptionCallback(const WindowExceptionCallback callback) noexcept {
    exceptionCallback.store(callback, std::memory_order_release);
}

std::uint64_t Window::ExceptionCount() noexcept {
    return exceptionCount.load(std::memory_order_relaxed);
}

void Window::ReportException(const HWND window, const UINT message, const char* description) noexcept {
    exceptionCount.fetch_add(1, std::memory_order_relaxed);
    char diagnostic[160]{};
    _snprintf_s(diagnostic, sizeof(diagnostic), _TRUNCATE,
                "VisPick window callback failed (message 0x%04X): ", message);
    OutputDebugStringA(diagnostic);
    OutputDebugStringA(description);
    OutputDebugStringA("\n");
    if (const auto callback = exceptionCallback.load(std::memory_order_acquire)) {
        callback({window, message, description});
    }
}

LRESULT Window::OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) {
    return DefWindowProcW(window_, message, parameter, data);
}

LRESULT CALLBACK Window::Dispatch(const HWND window, const UINT message, const WPARAM parameter, const LPARAM data) {
    auto* instance = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(data);
        instance = static_cast<Window*>(creation->lpCreateParams);
        instance->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    }
    if (!instance) { return DefWindowProcW(window, message, parameter, data); }
    // Window procedures cannot propagate C++ exceptions through Win32 callbacks.
    LRESULT result = 0;
    try {
        result = instance->OnMessage(message, parameter, data);
    } catch (const std::exception& error) {
        ReportException(window, message, error.what());
        result = message == WM_NCCREATE ? FALSE
                 : message == WM_CREATE ? -1
                                        : DefWindowProcW(window, message, parameter, data);
    } catch (...) {
        ReportException(window, message, "unknown C++ exception");
        result = message == WM_NCCREATE ? FALSE
                 : message == WM_CREATE ? -1
                                        : DefWindowProcW(window, message, parameter, data);
    }
    if (message == WM_NCDESTROY || (message == WM_NCCREATE && result == FALSE)) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        instance->window_ = nullptr;
    }
    return result;
}

void PumpWindowMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace vispick
