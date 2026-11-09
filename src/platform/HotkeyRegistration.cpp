#include "platform/HotkeyRegistration.h"
#include <algorithm>

namespace vispick {

HotkeyRegistration::~HotkeyRegistration() { Clear(); }

void HotkeyRegistration::Clear() noexcept {
    for (const auto& binding : registered_) { UnregisterHotKey(window_, binding.identifier); }
    registered_.clear();
}

std::vector<std::wstring> HotkeyRegistration::Rebind(std::span<const HotkeyBinding> bindings) {
    registered_.reserve(bindings.size());
    // Release changed entries together to support swaps. Keep unrelated keys
    // registered throughout reload so another process cannot take them.
    std::erase_if(registered_, [&](const Registered& current) {
        const auto index = static_cast<std::size_t>(current.identifier - 1);
        if (index < bindings.size() && bindings[index].command == current.command &&
            bindings[index].hotkey == current.hotkey) { return false; }
        UnregisterHotKey(window_, current.identifier);
        return true;
    });
    std::vector<std::wstring> conflicts;
    int identifier = 1;
    for (const auto& binding : bindings) {
        if (binding.hotkey.Enabled() && std::none_of(registered_.begin(), registered_.end(),
            [&](const Registered& current) { return current.identifier == identifier; })) {
            if (RegisterHotKey(window_, identifier, binding.hotkey.modifiers | MOD_NOREPEAT, binding.hotkey.virtualKey)) {
                registered_.push_back({identifier, binding.command, binding.hotkey});
            } else {
                conflicts.push_back(binding.label + L"（注册失败，错误 " + std::to_wstring(GetLastError()) + L"）");
            }
        }
        ++identifier;
    }
    return conflicts;
}

AppCommand HotkeyRegistration::CommandFor(int identifier) const noexcept {
    for (const auto& binding : registered_) {
        if (binding.identifier == identifier) { return binding.command; }
    }
    return AppCommand::None;
}

} // namespace vispick
