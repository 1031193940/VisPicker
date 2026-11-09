#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vispick::gif {

// One instance per encoder worker. Dictionary storage is allocated once and
// reused; input indices are still checked in the main encoding pass.
class LzwEncoder {
public:
    LzwEncoder();
    std::vector<std::uint8_t> Encode(std::span<const std::uint8_t> pixels,
                                    int minimumCodeSize = 8);
    void EncodeTo(std::span<const std::uint8_t> pixels, int minimumCodeSize,
                  std::vector<std::uint8_t>& destination, int paletteSize = 0);

private:
    std::vector<int> dictionaryKeys_;
    std::vector<std::uint16_t> dictionaryCodes_;
};

// GIF's LSB-first variable-width LZW stream, without GIF sub-block framing.
std::vector<std::uint8_t> EncodeLzw(std::span<const std::uint8_t> pixels,
                                  int minimumCodeSize = 8);

} // namespace vispick::gif
