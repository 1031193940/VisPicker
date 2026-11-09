#pragma once

#include <windows.h>
#include <filesystem>
#include <memory>

namespace vispick {

class ConfigWatcher {
public:
    ConfigWatcher(std::filesystem::path path, HWND notificationWindow, UINT notificationMessage);
    ~ConfigWatcher();
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;
    // Start/Stop are owner-thread operations. Start can throw on setup failure.
    void Start();
    void Stop() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Posted message carries WPARAM=0 for a change (UI should debounce and reload), or a
// Win32 error code if monitoring stopped. LPARAM=0; worker never parses configuration.
} // namespace vispick
