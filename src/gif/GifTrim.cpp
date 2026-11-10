#include "gif/GifTrim.h"
#include "gif/GifReader.h"
#include "gif/GifWriter.h"
#include "recording/FrameChange.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick::gif {

void TrimRecordedGif(const std::filesystem::path& source, const std::filesystem::path& destination, GifTrimRange range,
                     const std::function<bool()>& cancelled) {
    if (range.beginCentiseconds >= range.endCentiseconds) {
        throw std::invalid_argument("The GIF trim range is empty");
    }
    std::error_code error;
    if (std::filesystem::equivalent(source, destination, error)) {
        throw std::invalid_argument("GIF trimming requires a separate output file");
    }

    GifReader reader(source);
    const auto& document = reader.Metadata();
    std::uint64_t elapsed = 0;
    unsigned firstFrame = document.frameCount;
    unsigned lastFrame = document.frameCount;
    // Validate the complete range before opening the output. Frame boundaries
    // keep variable delays exact, including the minimum 20 ms GIF delay.
    for (unsigned frameIndex = 0; frameIndex < document.frameCount; ++frameIndex) {
        if (cancelled && cancelled()) { throw std::runtime_error("GIF trimming was cancelled"); }
        const auto frame = reader.ReadFrameMetadata(frameIndex);
        if (frame.delayCentiseconds < recording::MinimumFrameDelayCentiseconds ||
            frame.delayCentiseconds > std::numeric_limits<std::uint64_t>::max() - elapsed) {
            throw std::runtime_error("Recorded GIF contains an invalid frame delay");
        }
        if (elapsed == range.beginCentiseconds) { firstFrame = frameIndex; }
        elapsed += frame.delayCentiseconds;
        if (elapsed == range.endCentiseconds) { lastFrame = frameIndex; }
    }
    if (firstFrame == document.frameCount || lastFrame == document.frameCount || firstFrame > lastFrame) {
        throw std::invalid_argument("GIF trim points must be valid frame boundaries");
    }

    GifCanvas canvas(document);
    GifWriter writer(destination, static_cast<int>(document.width), static_cast<int>(document.height));
    FrameEncoder encoder;
    for (unsigned frameIndex = 0; frameIndex <= lastFrame; ++frameIndex) {
        if (cancelled && cancelled()) { throw std::runtime_error("GIF trimming was cancelled"); }
        const auto frame = reader.ReadFrame(frameIndex);
        canvas.Compose(frame);
        if (frameIndex < firstFrame) { continue; }
        // Recording canvases are opaque. Full visual frames make every output
        // frame independent of a discarded prefix, transparency or disposal.
        if (std::any_of(canvas.Pixels().begin(), canvas.Pixels().end(),
                        [](std::uint32_t pixel) { return (pixel >> 24) != 255; })) {
            throw std::runtime_error("Cannot trim a recording with a transparent logical canvas");
        }
        writer.WriteFrame(encoder.Encode(canvas.Pixels()), frame.metadata.delayCentiseconds);
    }
    writer.Finish();
}

} // namespace vispick::gif
