#pragma once

#include "core/Config.h"
#include "imaging/Bitmap.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vispick {

struct PinMemory {
    std::size_t windows = 0;
    std::uint64_t logicalSourceBytes = 0; // Native width * height * 4, before lossless storage packing.
    std::uint64_t sourceBytes = 0;
    std::uint64_t renderContentBytes = 0;
    std::uint64_t decorationBytes = 0;
    std::uint64_t recycledBytes = 0;
};

class PinManager {
public:
    static constexpr UINT PendingMessage = WM_APP + 61;
    PinManager(HWND owner, std::function<void(Bitmap)> save, std::function<void(std::wstring, bool)> notify);
    ~PinManager();
    PinManager(const PinManager&) = delete;
    PinManager& operator=(const PinManager&) = delete;
    void Add(Bitmap image, POINT screenOrigin, const Config::Pin& config);
    // Clipboard/file pins use the actual bounded render size when centering.
    void AddCentered(Bitmap image, POINT screenCenter, const Config::Pin& config);
    void Configure(const Config::Pin& config, bool english = false);
    void ShowAll();
    void HideAll();
    void ToggleAll();
    void CloseAll();
    void ResetPassthrough();
    void RestoreLast();
    void DisplayChanged();
    // Called by App after PendingMessage. Window callbacks enqueue requests;
    // deletion and modal save dialogs happen only after those callbacks return.
    void ProcessPending();
    [[nodiscard]] PinMemory Memory() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vispick
