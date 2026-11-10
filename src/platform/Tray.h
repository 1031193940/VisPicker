#pragma once

#include "core/AppCommand.h"
#include "core/Config.h"
#include <windows.h>
#include <string_view>

namespace vispick {

class Tray {
public:
    // The HWND and shared icon must outlive this object; App owns DestroyIcon.
    Tray(HWND owner, UINT callbackMessage, HICON sharedIcon);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    [[nodiscard]] bool Restore() noexcept;
    void Remove() noexcept;
    void SetLeftClickCommand(AppCommand command) noexcept;
    void SetLanguage(bool english) noexcept;
    void SetHotkeys(const Config::Hotkeys& hotkeys) noexcept;
    [[nodiscard]] static std::wstring ShortcutLabel(std::wstring_view label, Hotkey hotkey);
    [[nodiscard]] AppCommand HandleCallback(WPARAM parameter, LPARAM data, bool autostartEnabled,
                                          bool pinsAcrossDesktops = false);
    [[nodiscard]] bool Notify(std::wstring_view title, std::wstring_view text, bool warning = false) noexcept;
    [[nodiscard]] static UINT TaskbarCreatedMessage();
    [[nodiscard]] static AppCommand CommandFromMenuId(UINT identifier) noexcept;
private:
    HWND owner_;
    UINT callbackMessage_;
    HICON icon_;
    AppCommand leftClickCommand_ = AppCommand::Screenshot;
    bool added_ = false;
    bool version4_ = false;
    bool english_ = false;
    Config::Hotkeys hotkeys_;
};

} // namespace vispick
