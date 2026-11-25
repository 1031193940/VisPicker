#include "gif/GifTrim.h"
#include "gif/GifReader.h"
#include "gif/GifWriter.h"
#include "imaging/Bitmap.h"
#include "platform/FileCommit.h"
#include "recording/FrameChange.h"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace vispick::gif {

void TrimRecordedGif(const std::filesystem::path& source, const std::filesystem::path& destination, GifTrimRange range,
                     const std::function<bool()>& cancelled, bool dither) {
    if (range.beginCentiseconds >= range.endCentiseconds) {
        throw std::invalid_argument("The GIF trim range is empty");
    }
    std::error_code error;
    if (std::filesystem::equivalent(ExtendedFilePath(source), ExtendedFilePath(destination), error)) {
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

    const int width = static_cast<int>(document.width);
    const int height = static_cast<int>(document.height);
    GifCanvas canvas(document);
    GifWriter writer(destination, width, height);
    recording::RegionEncoder encoder;
    std::vector<std::uint32_t> previousPixels;
    std::vector<std::uint32_t> currentRegion;
    std::vector<std::uint32_t> previousRegion;
    for (unsigned frameIndex = 0; frameIndex <= lastFrame; ++frameIndex) {
        if (cancelled && cancelled()) { throw std::runtime_error("GIF trimming was cancelled"); }
        const auto frame = reader.ReadFrame(frameIndex);
        canvas.Compose(frame);
        if (frameIndex < firstFrame) { continue; }
        const auto currentPixels = canvas.Pixels();
        // Recording canvases are opaque. Emit the new first frame full-canvas
        // opaque and subsequent frames as transparent changed sub-rectangles.
        if (std::any_of(currentPixels.begin(), currentPixels.end(),
                        [](std::uint32_t pixel) { return (pixel >> 24) != 255; })) {
            throw std::runtime_error("Cannot trim a recording with a transparent logical canvas");
        }
        const BitmapView currentView(width, height, currentPixels);
        if (frameIndex == firstFrame) {
            const auto encoded = encoder.Encode(currentPixels, {}, width, dither);
            writer.WriteFrame(encoded.frame, {0, 0, document.width, document.height, frame.metadata.delayCentiseconds,
                                              DisposalMethod::Keep, encoded.transparentColorIndex});
            previousPixels.assign(currentPixels.begin(), currentPixels.end());
            continue;
        }
        const BitmapView previousView(width, height, previousPixels);
        const auto changed = recording::ChangedRectangle(currentView, previousView);
        if (!changed) {
            // Preserve the exact frame boundary and delay when the source contains a duplicate frame.
            const std::uint32_t unchangedPixel = currentPixels.front();
            const auto encoded = encoder.Encode(std::span<const std::uint32_t>(&unchangedPixel, 1U), {}, 1, false);
            writer.WriteFrame(encoded.frame, {0, 0, 1, 1, frame.metadata.delayCentiseconds, DisposalMethod::Keep,
                                              encoded.transparentColorIndex});
            continue;
        }
        recording::CopyRectangle(currentView, *changed, currentRegion);
        recording::CopyRectangle(previousView, *changed, previousRegion);
        const int regionWidth = static_cast<int>(changed->right - changed->left);
        const auto regionHeight = static_cast<unsigned>(changed->bottom - changed->top);
        const auto encoded = encoder.Encode(currentRegion, previousRegion, regionWidth, dither);
        writer.WriteFrame(encoded.frame,
                          {static_cast<unsigned>(changed->left), static_cast<unsigned>(changed->top),
                           static_cast<unsigned>(regionWidth), regionHeight, frame.metadata.delayCentiseconds,
                           DisposalMethod::Keep, encoded.transparentColorIndex});
        previousPixels.assign(currentPixels.begin(), currentPixels.end());
    }
    writer.Finish();
}

} // namespace vispick::gif
