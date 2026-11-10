#pragma once

#include "gif/GifWriter.h"
#include "imaging/Bitmap.h"
#include <Windows.h>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vispick::recording {

inline constexpr std::uint16_t MinimumFrameDelayCentiseconds = 2;
inline constexpr std::int64_t MinimumFrameDelayNanoseconds = MinimumFrameDelayCentiseconds * 10000000LL;

// Capture coalesces short intermediate intervals. A manual stop may leave a
// shorter tail; retain that last image with one browser-compatible delay.
[[nodiscard]] std::uint16_t RecordedFrameDelay(std::uint64_t centiseconds) noexcept;

struct MemoryPlan {
    unsigned workerCount = 1;
    unsigned slotCount = 2;
    std::uint64_t memoryLimit = 0;
    std::uint64_t maximumBytes = 0;
};

struct RecordingPlan {
    int width;
    int height;
    MemoryPlan memory;
};

// Elapsed nanoseconds are relative to recording start, not wall-clock dates.
// The caller serializes access with control-state transitions.
class ActiveTimeline final {
public:
    void Pause(std::int64_t elapsedNanoseconds, bool paused);
    [[nodiscard]] std::int64_t Elapsed(std::int64_t elapsedNanoseconds) const;
private:
    std::int64_t pausedNanoseconds_ = 0;
    std::int64_t pauseBegan_ = 0;
    std::int64_t lastTransition_ = 0;
    bool paused_ = false;
};

[[nodiscard]] MemoryPlan PlanMemory(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
    unsigned processorCount, unsigned pointerBits = sizeof(void*) * 8);
// Keep the requested scale when it fits. Otherwise try smaller percentages without cropping the source.
[[nodiscard]] RecordingPlan PlanRecording(int sourceWidth, int sourceHeight, int requestedScale,
    unsigned processorCount, unsigned pointerBits = sizeof(void*) * 8);
[[nodiscard]] std::optional<RECT> ChangedRectangle(BitmapView current, BitmapView previous);
void CopyRectangle(BitmapView source, RECT rectangle, std::vector<std::uint32_t>& destination);

struct EncodedRegion {
    gif::EncodedFrame frame;
    int transparentColorIndex = -1;
};

// Reusable per-worker workspace. Inputs are a tightly packed changed rectangle;
// an empty previous span means the first, completely opaque logical frame.
class RegionEncoder final {
public:
    [[nodiscard]] EncodedRegion Encode(std::span<const std::uint32_t> current,
        std::span<const std::uint32_t> previous, int width, bool dither);
private:
    gif::Quantizer quantizer_{{false, 0}}; // Scheduling must not change palettes.
    gif::FrameEncoder encoder_;
    std::vector<std::uint32_t> opaque_;
    gif::IndexedImage quantized_;
    gif::IndexedImage expanded_;
};

} // namespace vispick::recording
