#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vispick::gif {

struct IndexedImage {
    // BGRA packed colors; opaque output uses indices 0..254 (255 reserved).
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> pixels;
    bool exact = true;
    bool paletteReused = false;
    int maximumChannelError = 0;
};

struct QuantizerOptions {
    bool reusePalette = true;
    // Reuse is accepted only after checking every source RGB channel. Exact
    // <=255-color inputs are always kept exact regardless of this setting.
    int maximumReuseChannelError = 8;
};

class Quantizer {
public:
    explicit Quantizer(QuantizerOptions options = {});
    IndexedImage Quantize(std::span<const std::uint32_t> pixels);
    void QuantizeTo(std::span<const std::uint32_t> pixels, IndexedImage& destination);
    void ResetPalette() noexcept;

private:
    void ClearLookup();
    int FindColor(std::uint32_t color) const;
    void AddColor(std::uint32_t color, std::uint8_t paletteIndex);
    void BuildPreviousLookup();
    bool TryReusePalette(std::span<const std::uint32_t> pixels, IndexedImage& destination);
    void RememberPalette(const std::vector<std::uint32_t>& palette);

    QuantizerOptions options_;
    // Fixed-capacity open-address tables. No node allocation or rehashing.
    std::vector<std::uint32_t> lookupColors_;
    std::vector<std::uint8_t> lookupIndices_;
    std::vector<std::int16_t> nearestLookup_;
    std::vector<std::uint32_t> previousPalette_;
};

// Preserves all RGB values when <=255 colors occur. Otherwise applies Wu's
// variance-minimizing box subdivision on a 5-bit/channel histogram; no dithering.
IndexedImage Quantize(std::span<const std::uint32_t> pixels);

// Apply Floyd-Steinberg error diffusion to an existing lossy palette mapping.
// Exact mappings remain unchanged. Transparent indices neither receive nor
// propagate error; the two error rows and 32K lookup are bounded by image width.
void DitherFloydSteinberg(std::span<const std::uint32_t> source, int width,
    IndexedImage& image, int transparentColorIndex = -1);

} // namespace vispick::gif
