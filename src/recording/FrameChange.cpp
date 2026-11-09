#include "recording/FrameChange.h"
#include "core/Config.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick::recording {

std::uint16_t RecordedFrameDelay(std::uint64_t centiseconds) noexcept {
    static_assert(Config::Gif::MaximumSeconds * 100 <= std::numeric_limits<std::uint16_t>::max());
    // All active timestamps are bounded by the recording limit. The
    // upper clamp also prevents a narrowing wrap if the caller violates it.
    return static_cast<std::uint16_t>(std::clamp<std::uint64_t>(centiseconds,
        MinimumFrameDelayCentiseconds, std::numeric_limits<std::uint16_t>::max()));
}

void ActiveTimeline::Pause(std::int64_t elapsedNanoseconds, bool paused) {
    static_cast<void>(Elapsed(elapsedNanoseconds));
    if (paused == paused_) { return; }
    if (paused) { pauseBegan_ = elapsedNanoseconds; }
    else { pausedNanoseconds_ += elapsedNanoseconds - pauseBegan_; }
    lastTransition_ = elapsedNanoseconds;
    paused_ = paused;
}

std::int64_t ActiveTimeline::Elapsed(std::int64_t elapsedNanoseconds) const {
    if (elapsedNanoseconds < lastTransition_) { throw std::invalid_argument("Recording clock moved backwards"); }
    return elapsedNanoseconds - pausedNanoseconds_ - (paused_ ? elapsedNanoseconds - pauseBegan_ : 0);
}

MemoryPlan PlanMemory(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                      unsigned processorCount, unsigned pointerBits) {
    if (sourceWidth < 1 || sourceHeight < 1 || outputWidth < 1 || outputHeight < 1 ||
        outputWidth > 65535 || outputHeight > 65535 || (pointerBits != 32 && pointerBits != 64)) {
        throw std::invalid_argument("Invalid recording dimensions or architecture");
    }
    const auto sourcePixels = static_cast<std::uint64_t>(sourceWidth) * sourceHeight;
    const auto pixels = static_cast<std::uint64_t>(outputWidth) * outputHeight;
    MemoryPlan plan;
    plan.memoryLimit = (pointerBits == 64 ? 256ULL : 128ULL) * 1024 * 1024;
    if (sourcePixels > plan.memoryLimit / 4 || pixels > plan.memoryLimit / 10) {
        throw std::length_error("Recording dimensions exceed the memory budget");
    }
    // Capture + scaled DIB + previous submitted frame + pending and currently
    // transferred writer output. A freed slot can refill during disk I/O.
    const auto persistent = sourcePixels * 4 + pixels * 12 + 65536;
    // At opaque RGB scratch growth, both old/new 4-byte arrays can coexist
    // with quantized and expanded indices. Bound that allocation peak too.
    const auto perWorker = pixels * 10 + 3ULL * 1024 * 1024 + static_cast<std::uint64_t>(outputWidth) * 48;
    // Current/previous rectangle + a reserved <=2 bytes/pixel LZW buffer.
    const auto perSlot = pixels * 10 + 4096;
    const unsigned preferredWorkers = std::clamp(processorCount > 2 ? processorCount - 2 : 1U, 1U, 6U);
    for (unsigned workers = preferredWorkers; workers != 0; --workers) {
        for (unsigned slots = std::min(workers + 2, 8U); slots >= std::max(2U,workers); --slots) {
            const auto bound = persistent + perWorker * workers + perSlot * slots;
            if (bound <= plan.memoryLimit) {
                plan.workerCount = workers;
                plan.slotCount = slots;
                plan.maximumBytes = bound;
                return plan;
            }
        }
    }
    throw std::length_error("Recording region exceeds the bounded pipeline memory budget; reduce region or scale");
}

std::optional<RECT> ChangedRectangle(BitmapView current, BitmapView previous) {
    if (current.Empty() || current.Width() != previous.Width() || current.Height() != previous.Height()) {
        throw std::invalid_argument("Changed-frame dimensions do not match");
    }
    RECT rectangle{current.Width(), current.Height(), 0, 0};
    for (int row = 0; row < current.Height(); ++row) {
        const auto* present = current.Row(row);
        const auto* earlier = previous.Row(row);
        for (int column = 0; column < current.Width(); ++column) {
            if (((present[column] ^ earlier[column]) & 0x00FFFFFFU) != 0) {
                rectangle.left = std::min(rectangle.left, static_cast<LONG>(column));
                rectangle.top = std::min(rectangle.top, static_cast<LONG>(row));
                rectangle.right = std::max(rectangle.right, static_cast<LONG>(column + 1));
                rectangle.bottom = static_cast<LONG>(row + 1);
            }
        }
    }
    if (rectangle.right == 0) { return std::nullopt; }
    return rectangle;
}

void CopyRectangle(BitmapView source, RECT rectangle, std::vector<std::uint32_t>& destination) {
    if (rectangle.left < 0 || rectangle.top < 0 || rectangle.right > source.Width() || rectangle.bottom > source.Height() ||
        rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) { throw std::invalid_argument("Invalid recording crop"); }
    const auto width = static_cast<std::size_t>(rectangle.right - rectangle.left);
    destination.resize(width * static_cast<std::size_t>(rectangle.bottom - rectangle.top));
    for (LONG row = rectangle.top; row < rectangle.bottom; ++row) {
        const auto offset = static_cast<std::size_t>(row - rectangle.top) * width;
        const auto* sourceRow = source.Row(row) + rectangle.left;
        for (std::size_t column = 0; column < width; ++column) { destination[offset + column] = sourceRow[column] | 0xFF000000U; }
    }
}

EncodedRegion RegionEncoder::Encode(std::span<const std::uint32_t> current,
                                    std::span<const std::uint32_t> previous, int width, bool dither) {
    if (width < 1 || current.empty() || current.size() % static_cast<std::size_t>(width) != 0 ||
        (!previous.empty() && previous.size() != current.size())) { throw std::invalid_argument("Invalid changed-frame input"); }
    opaque_.clear();
    opaque_.reserve(current.size());
    for (std::size_t pixel = 0; pixel < current.size(); ++pixel) {
        if (previous.empty() || ((current[pixel] ^ previous[pixel]) & 0x00FFFFFFU) != 0) { opaque_.push_back(current[pixel]); }
    }
    if (opaque_.empty()) { throw std::invalid_argument("Unchanged frames must extend duration without encoding"); }
    quantizer_.QuantizeTo(opaque_, quantized_);
    expanded_.palette = quantized_.palette;
    expanded_.pixels.reserve(current.size());
    expanded_.pixels.resize(current.size());
    expanded_.exact = quantized_.exact;
    expanded_.paletteReused = false;
    expanded_.maximumChannelError = quantized_.maximumChannelError;
    const bool transparent = opaque_.size() != current.size();
    const int transparentIndex = transparent ? static_cast<int>(expanded_.palette.size()) : -1;
    if (transparent) { expanded_.palette.push_back(0); }
    std::size_t opaqueIndex = 0;
    for (std::size_t pixel = 0; pixel < current.size(); ++pixel) {
        if (!previous.empty() && ((current[pixel] ^ previous[pixel]) & 0x00FFFFFFU) == 0) {
            expanded_.pixels[pixel] = static_cast<std::uint8_t>(transparentIndex);
        } else { expanded_.pixels[pixel] = quantized_.pixels[opaqueIndex++]; }
    }
    if (dither && !expanded_.exact) { gif::DitherFloydSteinberg(current, width, expanded_, transparentIndex); }
    return {encoder_.EncodeIndexed(expanded_), transparentIndex};
}

} // namespace vispick::recording
