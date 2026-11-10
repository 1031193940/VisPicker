#include "gif/Lzw.h"

#include <algorithm>
#include <stdexcept>

namespace vispick::gif {
namespace {

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& destination) : bytes_(destination) { bytes_.clear(); }
    void Write(int code, int bitCount) {
        pendingBits_ |= static_cast<std::uint32_t>(code) << pendingCount_;
        pendingCount_ += bitCount;
        while (pendingCount_ >= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(pendingBits_));
            pendingBits_ >>= 8;
            pendingCount_ -= 8;
        }
    }

    void Finish() {
        if (pendingCount_ != 0) { bytes_.push_back(static_cast<std::uint8_t>(pendingBits_)); }
    }

private:
    std::vector<std::uint8_t>& bytes_;
    std::uint32_t pendingBits_ = 0;
    int pendingCount_ = 0;
};

} // namespace

LzwEncoder::LzwEncoder() : dictionaryKeys_(8192, -1), dictionaryCodes_(8192) {}

std::vector<std::uint8_t> LzwEncoder::Encode(std::span<const std::uint8_t> pixels, int minimumCodeSize) {
    std::vector<std::uint8_t> destination;
    EncodeTo(pixels, minimumCodeSize, destination);
    return destination;
}

void LzwEncoder::EncodeTo(std::span<const std::uint8_t> pixels, int minimumCodeSize,
                          std::vector<std::uint8_t>& destination, int paletteSize) {
    if (minimumCodeSize < 2 || minimumCodeSize > 8) {
        throw std::invalid_argument("GIF LZW minimum code size must be 2..8");
    }
    const int clearCode = 1 << minimumCodeSize;
    const int alphabetSize = paletteSize == 0 ? clearCode : paletteSize;
    if (alphabetSize < 1 || alphabetSize > clearCode) {
        throw std::invalid_argument("Palette exceeds GIF LZW alphabet");
    }
    const int endCode = clearCode + 1;
    // At most 4096 codes; an 8192-slot table keeps probing short without heap nodes.
    std::fill(dictionaryKeys_.begin(), dictionaryKeys_.end(), -1);
    int nextCode = clearCode + 2;
    int codeSize = minimumCodeSize + 1;
    BitWriter writer(destination);
    writer.Write(clearCode, codeSize);
    if (pixels.empty()) {
        writer.Write(endCode, codeSize);
        writer.Finish();
        return;
    }

    int prefix = pixels.front();
    if (prefix >= alphabetSize) { throw std::invalid_argument("Pixel index exceeds GIF LZW alphabet"); }
    for (std::size_t pixelIndex = 1; pixelIndex < pixels.size(); ++pixelIndex) {
        const int symbol = pixels[pixelIndex];
        if (symbol >= alphabetSize) { throw std::invalid_argument("Pixel index exceeds GIF LZW alphabet"); }
        const int dictionaryKey = (prefix << 8) | symbol;
        auto bucket =
            static_cast<std::size_t>(((static_cast<std::uint32_t>(dictionaryKey) * 2654435761U) >> 19) & 8191);
        while (dictionaryKeys_[bucket] != -1 && dictionaryKeys_[bucket] != dictionaryKey) {
            bucket = (bucket + 1) & 8191;
        }
        if (dictionaryKeys_[bucket] == dictionaryKey) {
            prefix = dictionaryCodes_[bucket];
            continue;
        }

        writer.Write(prefix, codeSize);
        // The decoder creates entries one emitted code later than the encoder.
        // Grow only AFTER writing the code that completes its smaller table.
        if (nextCode == (1 << codeSize) && codeSize < 12) { ++codeSize; }
        if (nextCode < 4096) {
            dictionaryKeys_[bucket] = dictionaryKey;
            dictionaryCodes_[bucket] = static_cast<std::uint16_t>(nextCode++);
        } else {
            writer.Write(clearCode, codeSize);
            std::fill(dictionaryKeys_.begin(), dictionaryKeys_.end(), -1);
            nextCode = clearCode + 2;
            codeSize = minimumCodeSize + 1;
        }
        prefix = symbol;
    }
    writer.Write(prefix, codeSize);
    if (nextCode == (1 << codeSize) && codeSize < 12) { ++codeSize; }
    writer.Write(endCode, codeSize);
    writer.Finish();
}

std::vector<std::uint8_t> EncodeLzw(std::span<const std::uint8_t> pixels, int minimumCodeSize) {
    LzwEncoder encoder;
    return encoder.Encode(pixels, minimumCodeSize);
}

} // namespace vispick::gif
