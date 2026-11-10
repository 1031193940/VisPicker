#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace vispick::gif {

struct GifTrimRange {
    std::uint64_t beginCentiseconds = 0;
    std::uint64_t endCentiseconds = 0;
};

// Export a recorded GIF between frame boundaries. Composing the discarded
// prefix preserves delta frames and disposal at the new first frame. The
// caller supplies a disposable staging path; the source is never modified.
// Cancellation is checked between frames; a cancelled partial stage belongs
// to the caller and must be removed. Memory does not grow with frame count.
void TrimRecordedGif(const std::filesystem::path& source, const std::filesystem::path& destination, GifTrimRange range,
                     const std::function<bool()>& cancelled = {});

} // namespace vispick::gif
