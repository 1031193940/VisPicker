#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace vispick {

class Autostart {
public:
    // HKCU only. Alternate keys/value names allow tests to remain isolated.
    explicit Autostart(std::wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       std::wstring valueName = L"VisPick");
    [[nodiscard]] bool Enabled() const;
    [[nodiscard]] std::optional<std::wstring> Command() const;
    void SetEnabled(bool enabled, const std::filesystem::path& executable,
                    const std::filesystem::path& config = {}) const;
    // Repair an existing VisPick command only when its original executable is missing.
    [[nodiscard]] bool RepairIfEnabled(const std::filesystem::path& executable) const;
    [[nodiscard]] static std::wstring BuildCommand(const std::filesystem::path& executable,
                                                   const std::filesystem::path& config = {});

private:
    std::wstring keyPath_;
    std::wstring valueName_;
};

} // namespace vispick
