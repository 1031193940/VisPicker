#include "platform/Tray.h"

#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <stdexcept>

namespace vispick {
namespace {

constexpr UINT IconId = 1;
constexpr UINT FirstMenuId = 1000;

template <std::size_t Size>
void CopyText(wchar_t (&destination)[Size], const std::wstring_view text) noexcept {
    static_assert(Size >= 2);
    std::size_t count = std::min(text.size(), Size - 1);
    const bool truncated = count < text.size();
    if (truncated) { --count; }
    if (count != 0 && text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF) { --count; }
    std::copy_n(text.data(), count, destination);
    if (truncated) { destination[count++] = L'…'; }
    destination[count] = L'\0';
}

NOTIFYICONDATAW NotificationData(const HWND owner) noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = IconId;
    return data;
}

class MenuOwner {
public:
    MenuOwner() : menu_(CreatePopupMenu()) {
        if (!menu_) { throw std::runtime_error("CreatePopupMenu failed"); }
    }
    ~MenuOwner() { if (menu_) { DestroyMenu(menu_); } }
    MenuOwner(const MenuOwner&) = delete;
    MenuOwner& operator=(const MenuOwner&) = delete;
    [[nodiscard]] HMENU Get() const noexcept { return menu_; }
    void Relinquish() noexcept { menu_ = nullptr; }
private:
    HMENU menu_;
};

void AddCommand(const HMENU menu, const AppCommand command, const wchar_t* chinese,
                const wchar_t* english, const bool useEnglish, const bool checked = false, const Hotkey hotkey = {}) {
    const auto label = Tray::ShortcutLabel(useEnglish ? english : chinese, hotkey);
    if (!AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
        FirstMenuId + static_cast<UINT>(command), label.c_str())) {
        throw std::runtime_error("AppendMenuW failed");
    }
}

void Separator(const HMENU menu) {
    if (!AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) { throw std::runtime_error("AppendMenuW failed"); }
}

AppCommand ShowMenu(const HWND owner, POINT position, const bool autostart, const bool english,
                    const bool pinsAcrossDesktops, const Config::Hotkeys hotkeys) {
    MenuOwner menu;
    AddCommand(menu.Get(), AppCommand::Screenshot, L"截图", L"Screenshot", english, false, hotkeys.screenshot);
    AddCommand(menu.Get(), AppCommand::Gif, L"录制 GIF", L"Record GIF", english, false, hotkeys.gif);
    AddCommand(menu.Get(), AppCommand::Pin, L"截取参考图", L"Capture reference", english);
    AddCommand(menu.Get(), AppCommand::PinClipboard, L"从剪贴板贴图", L"Pin clipboard image", english, false, hotkeys.pinClipboard);
    MenuOwner pins;
    AddCommand(pins.Get(), AppCommand::TogglePins, L"显示 / 隐藏全部", L"Show / hide all", english, false, hotkeys.togglePins);
    AddCommand(pins.Get(), AppCommand::ShowPins, L"显示全部", L"Show all", english);
    AddCommand(pins.Get(), AppCommand::HidePins, L"隐藏全部", L"Hide all", english);
    AddCommand(pins.Get(), AppCommand::ClosePins, L"关闭全部", L"Close all", english);
    AddCommand(pins.Get(), AppCommand::ResetPinPassthrough, L"取消全部鼠标穿透", L"Disable click-through for all", english);
    AddCommand(pins.Get(), AppCommand::RestorePin, L"恢复最近关闭", L"Restore last closed", english);
    Separator(pins.Get());
    AddCommand(pins.Get(), AppCommand::TogglePinsAcrossDesktops, L"参考图跨桌面",
        L"Show reference images on all desktops", english, pinsAcrossDesktops);
    if (!AppendMenuW(menu.Get(), MF_POPUP, reinterpret_cast<UINT_PTR>(pins.Get()), english ? L"Reference images" : L"参考图")) {
        throw std::runtime_error("Append reference submenu failed");
    }
    pins.Relinquish(); // Parent menu now owns the submenu and all its children.
    Separator(menu.Get());
    AddCommand(menu.Get(), AppCommand::OpenConfig, L"打开配置文件", L"Open configuration", english);
    AddCommand(menu.Get(), AppCommand::Reload, L"重新加载配置", L"Reload configuration", english);
    AddCommand(menu.Get(), AppCommand::ToggleAutostart, L"开机自启", L"Start at sign-in", english, autostart);
    Separator(menu.Get());
    AddCommand(menu.Get(), AppCommand::About, L"关于 VisPick", L"About VisPick", english);
    AddCommand(menu.Get(), AppCommand::Exit, L"退出", L"Exit", english);
    if (position.x == -1 && position.y == -1) { GetCursorPos(&position); }
    SetForegroundWindow(owner);
    // All mutable state was copied before this modal call. No Tray/App reference
    // is retained while nested window messages run.
    const UINT selected = TrackPopupMenu(menu.Get(), TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        position.x, position.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    auto notification = NotificationData(owner);
    Shell_NotifyIconW(NIM_SETFOCUS, &notification);
    return Tray::CommandFromMenuId(selected);
}

} // namespace

Tray::Tray(const HWND owner, const UINT callbackMessage, const HICON sharedIcon)
    : owner_(owner), callbackMessage_(callbackMessage), icon_(sharedIcon) {
    if (!owner || !sharedIcon || callbackMessage < WM_APP) {
        throw std::invalid_argument("Tray requires an owner, shared icon and WM_APP callback");
    }
}

Tray::~Tray() { Remove(); }

bool Tray::Restore() noexcept {
    auto data = NotificationData(owner_);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = callbackMessage_;
    data.hIcon = icon_;
    CopyText(data.szTip, english_ ? L"VisPick — Screenshot · GIF · Reference images" : L"VisPick — 截图 · GIF · 参考图");
    added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (!added_) { added_ = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE; }
    if (added_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        version4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
    }
    return added_;
}

void Tray::Remove() noexcept {
    if (added_) {
        auto data = NotificationData(owner_);
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }
}

void Tray::SetLeftClickCommand(const AppCommand command) noexcept { leftClickCommand_ = command; }

void Tray::SetHotkeys(const Config::Hotkeys& hotkeys) noexcept { hotkeys_ = hotkeys; }

std::wstring Tray::ShortcutLabel(const std::wstring_view label, const Hotkey hotkey) {
    return hotkey.Enabled() ? std::wstring(label) + L"\t" + FormatHotkey(hotkey) : std::wstring(label);
}

void Tray::SetLanguage(const bool english) noexcept {
    english_ = english;
    if (added_) {
        auto data = NotificationData(owner_);
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        CopyText(data.szTip, english_ ? L"VisPick — Screenshot · GIF · Reference images" : L"VisPick — 截图 · GIF · 参考图");
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

AppCommand Tray::HandleCallback(const WPARAM parameter, const LPARAM data, const bool autostartEnabled,
                               const bool pinsAcrossDesktops) {
    const UINT identifier = version4_ ? HIWORD(data) : static_cast<UINT>(parameter);
    if (identifier != IconId) { return AppCommand::None; }
    const UINT event = LOWORD(data);
    if (event == NIN_SELECT || event == NIN_KEYSELECT || (!version4_ && event == WM_LBUTTONUP)) {
        return leftClickCommand_;
    }
    if (event == WM_CONTEXTMENU || (!version4_ && event == WM_RBUTTONUP)) {
        POINT position{-1, -1};
        if (version4_) { position = {GET_X_LPARAM(parameter), GET_Y_LPARAM(parameter)}; }
        return ShowMenu(owner_, position, autostartEnabled, english_, pinsAcrossDesktops, hotkeys_);
    }
    return AppCommand::None;
}

bool Tray::Notify(const std::wstring_view title, const std::wstring_view text, const bool warning) noexcept {
    if (!added_) { return false; }
    auto data = NotificationData(owner_);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    CopyText(data.szInfoTitle, title);
    CopyText(data.szInfo, text);
    return Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
}

UINT Tray::TaskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    if (!message) { throw std::runtime_error("Register TaskbarCreated message failed"); }
    return message;
}

AppCommand Tray::CommandFromMenuId(const UINT identifier) noexcept {
    if (identifier == FirstMenuId + static_cast<UINT>(AppCommand::TogglePinsAcrossDesktops)) {
        return AppCommand::TogglePinsAcrossDesktops;
    }
    if (identifier <= FirstMenuId || identifier > FirstMenuId + static_cast<UINT>(AppCommand::Exit)) { return AppCommand::None; }
    const auto command = static_cast<AppCommand>(identifier - FirstMenuId);
    return command == AppCommand::PinFile ? AppCommand::None : command;
}

} // namespace vispick
