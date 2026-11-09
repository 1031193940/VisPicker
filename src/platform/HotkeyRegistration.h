#pragma once

#include "core/AppCommand.h"
#include "core/Hotkey.h"
#include <windows.h>
#include <span>
#include <string>
#include <vector>

namespace vispick {

struct HotkeyBinding {
    AppCommand command;
    Hotkey hotkey;
    std::wstring label;
};

struct HotkeyFailure {
    Hotkey hotkey;
    std::wstring label;
    DWORD error;
};

class HotkeyRegistration {
public:
    explicit HotkeyRegistration(HWND window) : window_(window) {}
    ~HotkeyRegistration();
    HotkeyRegistration(const HotkeyRegistration&) = delete;
    HotkeyRegistration& operator=(const HotkeyRegistration&) = delete;
    std::vector<HotkeyFailure> Rebind(std::span<const HotkeyBinding> bindings);
    [[nodiscard]] AppCommand CommandFor(int identifier) const noexcept;
    void Clear() noexcept;
private:
    struct Registered { int identifier; AppCommand command; Hotkey hotkey; };
    HWND window_;
    std::vector<Registered> registered_;
};

} // namespace vispick
