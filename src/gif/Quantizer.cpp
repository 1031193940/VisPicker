#include "gif/Quantizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <emmintrin.h>

namespace vispick::gif {
namespace {

constexpr int Side = 33;
constexpr int CellCount = Side * Side * Side;

int Cell(int red, int green, int blue) {
    return (red * Side + green) * Side + blue;
}

struct Moments {
    double count = 0;
    double red = 0;
    double green = 0;
    double blue = 0;
    double squared = 0;

    Moments& operator+=(const Moments& value) {
        count += value.count;
        red += value.red;
        green += value.green;
        blue += value.blue;
        squared += value.squared;
        return *this;
    }

    Moments& operator-=(const Moments& value) {
        count -= value.count;
        red -= value.red;
        green -= value.green;
        blue -= value.blue;
        squared -= value.squared;
        return *this;
    }
};

struct Box {
    std::array<int, 3> lower = {0, 0, 0};
    std::array<int, 3> upper = {32, 32, 32};
    double variance = 0;
};

Moments Volume(const std::vector<Moments>& integral, const Box& box) {
    Moments result;
    for (int corner = 0; corner < 8; ++corner) {
        const int red = (corner & 1) != 0 ? box.upper[0] : box.lower[0];
        const int green = (corner & 2) != 0 ? box.upper[1] : box.lower[1];
        const int blue = (corner & 4) != 0 ? box.upper[2] : box.lower[2];
        const int upperCount = ((corner & 1) != 0) + ((corner & 2) != 0) + ((corner & 4) != 0);
        if ((upperCount & 1) != 0) {
            result += integral[static_cast<std::size_t>(Cell(red, green, blue))];
        } else {
            result -= integral[static_cast<std::size_t>(Cell(red, green, blue))];
        }
    }
    return result;
}

double MeanSquared(const Moments& moments) {
    if (moments.count == 0) {
        return 0;
    }
    return (moments.red * moments.red + moments.green * moments.green +
            moments.blue * moments.blue) / moments.count;
}

double Variance(const std::vector<Moments>& integral, const Box& box) {
    const auto moments = Volume(integral, box);
    return std::max(0.0, moments.squared - MeanSquared(moments));
}

bool Split(const std::vector<Moments>& integral, Box& first, Box& second) {
    double bestScore = -1.0;
    int bestAxis = -1;
    int bestPosition = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const auto coordinate = static_cast<std::size_t>(axis);
        for (int position = first.lower[coordinate] + 1;
             position < first.upper[coordinate]; ++position) {
            Box left = first;
            Box right = first;
            left.upper[coordinate] = position;
            right.lower[coordinate] = position;
            const auto leftMoments = Volume(integral, left);
            const auto rightMoments = Volume(integral, right);
            if (leftMoments.count == 0 || rightMoments.count == 0) {
                continue;
            }
            const double score = MeanSquared(leftMoments) + MeanSquared(rightMoments);
            if (score > bestScore) {
                bestScore = score;
                bestAxis = axis;
                bestPosition = position;
            }
        }
    }
    if (bestAxis < 0) {
        return false;
    }
    second = first;
    first.upper[static_cast<std::size_t>(bestAxis)] = bestPosition;
    second.lower[static_cast<std::size_t>(bestAxis)] = bestPosition;
    first.variance = Variance(integral, first);
    second.variance = Variance(integral, second);
    return true;
}

int ChannelError(std::uint32_t source, std::uint32_t mapped) {
    return std::max({std::abs(static_cast<int>((source >> 16) & 255) - static_cast<int>((mapped >> 16) & 255)),
                     std::abs(static_cast<int>((source >> 8) & 255) - static_cast<int>((mapped >> 8) & 255)),
                     std::abs(static_cast<int>(source & 255) - static_cast<int>(mapped & 255))});
}

IndexedImage QuantizeWu(std::span<const std::uint32_t> pixels,
                        std::span<std::int16_t> reuseSeeds) {
    std::vector<Moments> integral(CellCount);
    for (const auto pixel : pixels) {
        const int red = static_cast<int>((pixel >> 16) & 255);
        const int green = static_cast<int>((pixel >> 8) & 255);
        const int blue = static_cast<int>(pixel & 255);
        auto& moments = integral[static_cast<std::size_t>(Cell((red >> 3) + 1,
                                                              (green >> 3) + 1,
                                                              (blue >> 3) + 1))];
        moments.count += 1;
        moments.red += red;
        moments.green += green;
        moments.blue += blue;
        moments.squared += red * red + green * green + blue * blue;
    }
    // Three separable prefix sums produce the 3D summed-area table.
    for (int red = 1; red < Side; ++red) {
        for (int green = 1; green < Side; ++green) {
            for (int blue = 1; blue < Side; ++blue) {
                integral[static_cast<std::size_t>(Cell(red, green, blue))] +=
                    integral[static_cast<std::size_t>(Cell(red - 1, green, blue))];
            }
        }
    }
    for (int red = 1; red < Side; ++red) {
        for (int green = 1; green < Side; ++green) {
            for (int blue = 1; blue < Side; ++blue) {
                integral[static_cast<std::size_t>(Cell(red, green, blue))] +=
                    integral[static_cast<std::size_t>(Cell(red, green - 1, blue))];
            }
        }
    }
    for (int red = 1; red < Side; ++red) {
        for (int green = 1; green < Side; ++green) {
            for (int blue = 1; blue < Side; ++blue) {
                integral[static_cast<std::size_t>(Cell(red, green, blue))] +=
                    integral[static_cast<std::size_t>(Cell(red, green, blue - 1))];
            }
        }
    }

    std::vector<Box> boxes(1);
    boxes.front().variance = Variance(integral, boxes.front());
    while (boxes.size() < 255) {
        const auto largest = std::max_element(boxes.begin(), boxes.end(),
            [](const Box& left, const Box& right) { return left.variance < right.variance; });
        if (largest->variance <= 0) {
            break;
        }
        Box second;
        if (Split(integral, *largest, second)) {
            boxes.push_back(second);
        } else {
            largest->variance = 0;
        }
    }

    IndexedImage result;
    result.exact = false;
    std::vector<std::uint8_t> lookup(CellCount);
    for (std::size_t paletteIndex = 0; paletteIndex < boxes.size(); ++paletteIndex) {
        const auto& box = boxes[paletteIndex];
        const auto moments = Volume(integral, box);
        const auto channel = [count = moments.count](double sum) {
            return static_cast<std::uint32_t>(std::clamp(std::lround(sum / count), 0L, 255L));
        };
        result.palette.push_back(0xFF000000U | (channel(moments.red) << 16) |
                                 (channel(moments.green) << 8) | channel(moments.blue));
        for (int red = box.lower[0] + 1; red <= box.upper[0]; ++red) {
            for (int green = box.lower[1] + 1; green <= box.upper[1]; ++green) {
                for (int blue = box.lower[2] + 1; blue <= box.upper[2]; ++blue) {
                    lookup[static_cast<std::size_t>(Cell(red, green, blue))] =
                        static_cast<std::uint8_t>(paletteIndex);
                }
            }
        }
    }
    result.pixels.reserve(pixels.size());
    __m128i maximumChannelErrors = _mm_setzero_si128();
    for (const auto pixel : pixels) {
        const int red = static_cast<int>((pixel >> 19) & 31);
        const int green = static_cast<int>((pixel >> 11) & 31);
        const int blue = static_cast<int>((pixel >> 3) & 31);
        const auto paletteIndex = lookup[static_cast<std::size_t>(Cell(red + 1, green + 1, blue + 1))];
        result.pixels.push_back(paletteIndex);
        // Byte-wise saturated differences avoid three scalar abs/max dependency
        // chains per pixel. Alpha is ignored when reducing the RGB lanes below.
        const __m128i sourceChannels = _mm_cvtsi32_si128(static_cast<int>(pixel));
        const __m128i mappedChannels = _mm_cvtsi32_si128(static_cast<int>(result.palette[paletteIndex]));
        const __m128i channelErrors = _mm_or_si128(_mm_subs_epu8(sourceChannels, mappedChannels),
                                                 _mm_subs_epu8(mappedChannels, sourceChannels));
        maximumChannelErrors = _mm_max_epu8(maximumChannelErrors, channelErrors);
        if (!reuseSeeds.empty()) {
            const auto cell = static_cast<std::size_t>((red << 10) | (green << 5) | blue);
            reuseSeeds[cell] = paletteIndex;
        }
    }
    const auto channelMaxima = static_cast<std::uint32_t>(_mm_cvtsi128_si32(maximumChannelErrors));
    result.maximumChannelError = std::max({static_cast<int>(channelMaxima & 255U),
        static_cast<int>((channelMaxima >> 8) & 255U), static_cast<int>((channelMaxima >> 16) & 255U)});
    return result;
}

std::size_t ColorBucket(std::uint32_t color) {
    return static_cast<std::size_t>((color * 2654435761U) >> 22) & 1023;
}

} // namespace

Quantizer::Quantizer(QuantizerOptions options)
    : options_(options), lookupColors_(1024, 0xFFFFFFFFU), lookupIndices_(1024),
      nearestLookup_(options.reusePalette ? 32768 : 0, -1) {
    if (options.maximumReuseChannelError < 0 || options.maximumReuseChannelError > 255) {
        throw std::invalid_argument("Palette reuse channel error must be 0..255");
    }
}

void Quantizer::ClearLookup() {
    std::fill(lookupColors_.begin(), lookupColors_.end(), 0xFFFFFFFFU);
}

int Quantizer::FindColor(std::uint32_t color) const {
    color &= 0x00FFFFFFU;
    auto bucket = ColorBucket(color);
    while (lookupColors_[bucket] != 0xFFFFFFFFU) {
        if (lookupColors_[bucket] == color) {
            return lookupIndices_[bucket];
        }
        bucket = (bucket + 1) & 1023;
    }
    return -1;
}

void Quantizer::AddColor(std::uint32_t color, std::uint8_t paletteIndex) {
    color &= 0x00FFFFFFU;
    auto bucket = ColorBucket(color);
    while (lookupColors_[bucket] != 0xFFFFFFFFU && lookupColors_[bucket] != color) {
        bucket = (bucket + 1) & 1023;
    }
    lookupColors_[bucket] = color;
    lookupIndices_[bucket] = paletteIndex;
}

void Quantizer::BuildPreviousLookup() {
    ClearLookup();
    for (std::size_t paletteIndex = 0; paletteIndex < previousPalette_.size(); ++paletteIndex) {
        AddColor(previousPalette_[paletteIndex], static_cast<std::uint8_t>(paletteIndex));
    }
}

void Quantizer::RememberPalette(const std::vector<std::uint32_t>& palette) {
    if (previousPalette_ != palette) {
        previousPalette_ = palette;
        std::fill(nearestLookup_.begin(), nearestLookup_.end(), std::int16_t{-1});
    }
}

void Quantizer::ResetPalette() noexcept {
    previousPalette_.clear();
    std::fill(nearestLookup_.begin(), nearestLookup_.end(), std::int16_t{-1});
}

bool Quantizer::TryReusePalette(std::span<const std::uint32_t> pixels, IndexedImage& destination) {
    if (!options_.reusePalette || previousPalette_.empty()) {
        return false;
    }
    BuildPreviousLookup();
    int maximumError = 0;
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        const auto color = pixels[pixelIndex] & 0x00FFFFFFU;
        int paletteIndex = FindColor(color);
        if (paletteIndex < 0) {
            const auto cell = static_cast<std::size_t>(((color >> 9) & 0x7C00U) |
                ((color >> 6) & 0x03E0U) | ((color >> 3) & 0x001FU));
            paletteIndex = nearestLookup_[cell];
            if (paletteIndex < 0) {
                const int red = static_cast<int>((color >> 19) & 31) * 8 + 4;
                const int green = static_cast<int>((color >> 11) & 31) * 8 + 4;
                const int blue = static_cast<int>((color >> 3) & 31) * 8 + 4;
                int nearestDistance = (std::numeric_limits<int>::max)();
                for (std::size_t candidate = 0; candidate < previousPalette_.size(); ++candidate) {
                    const auto mapped = previousPalette_[candidate];
                    const int redDelta = red - static_cast<int>((mapped >> 16) & 255);
                    const int greenDelta = green - static_cast<int>((mapped >> 8) & 255);
                    const int blueDelta = blue - static_cast<int>(mapped & 255);
                    const int distance = redDelta * redDelta + greenDelta * greenDelta + blueDelta * blueDelta;
                    if (distance < nearestDistance) {
                        nearestDistance = distance;
                        paletteIndex = static_cast<int>(candidate);
                    }
                }
                nearestLookup_[cell] = static_cast<std::int16_t>(paletteIndex);
            }
        }
        const int error = ChannelError(color, previousPalette_[static_cast<std::size_t>(paletteIndex)]);
        if (error > options_.maximumReuseChannelError) {
            return false;
        }
        maximumError = std::max(maximumError, error);
        destination.pixels[pixelIndex] = static_cast<std::uint8_t>(paletteIndex);
    }
    destination.palette = previousPalette_;
    destination.exact = maximumError == 0;
    destination.paletteReused = true;
    destination.maximumChannelError = maximumError;
    return true;
}

void Quantizer::QuantizeTo(std::span<const std::uint32_t> pixels, IndexedImage& result) {
    result.palette.clear();
    result.pixels.reserve(pixels.size());
    result.pixels.resize(pixels.size());
    result.exact = true;
    result.paletteReused = false;
    result.maximumChannelError = 0;
    ClearLookup();
    std::uint32_t previousColor = 0xFFFFFFFFU;
    std::uint8_t previousIndex = 0;
    bool needsQuantization = false;
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        const auto color = pixels[pixelIndex] & 0x00FFFFFFU;
        if (color != previousColor) {
            const int found = FindColor(color);
            if (found >= 0) {
                previousIndex = static_cast<std::uint8_t>(found);
            } else {
                if (result.palette.size() == 255) {
                    needsQuantization = true;
                    break;
                }
                previousIndex = static_cast<std::uint8_t>(result.palette.size());
                AddColor(color, previousIndex);
                result.palette.push_back(color | 0xFF000000U);
            }
            previousColor = color;
        }
        result.pixels[pixelIndex] = previousIndex;
    }
    if (pixels.empty()) {
        return;
    }
    if (needsQuantization) {
        if (TryReusePalette(pixels, result)) {
            return;
        }
        // Seed only observed Wu bins. Unobserved bins retain the previous
        // nearest-centroid cache when the palette is unchanged, as before.
        std::vector<std::int16_t> reuseSeeds(options_.reusePalette ? 32768 : 0, -1);
        result = QuantizeWu(pixels, reuseSeeds);
        if (options_.reusePalette) {
            RememberPalette(result.palette);
            for (std::size_t cell = 0; cell < reuseSeeds.size(); ++cell) {
                if (reuseSeeds[cell] >= 0) { nearestLookup_[cell] = reuseSeeds[cell]; }
            }
        }
        return;
    } else if (options_.reusePalette && !previousPalette_.empty()) {
        BuildPreviousLookup();
        std::array<std::uint8_t, 255> remap{};
        bool exactReuse = true;
        for (std::size_t paletteIndex = 0; paletteIndex < result.palette.size(); ++paletteIndex) {
            const int previous = FindColor(result.palette[paletteIndex]);
            if (previous < 0) {
                exactReuse = false;
                break;
            }
            remap[paletteIndex] = static_cast<std::uint8_t>(previous);
        }
        if (exactReuse) {
            for (auto& paletteIndex : result.pixels) {
                paletteIndex = remap[paletteIndex];
            }
            result.palette = previousPalette_;
            result.paletteReused = true;
        }
    }
    if (options_.reusePalette) { RememberPalette(result.palette); }
}

IndexedImage Quantizer::Quantize(std::span<const std::uint32_t> pixels) {
    IndexedImage result;
    QuantizeTo(pixels, result);
    return result;
}

IndexedImage Quantize(std::span<const std::uint32_t> pixels) {
    Quantizer quantizer;
    return quantizer.Quantize(pixels);
}

void DitherFloydSteinberg(std::span<const std::uint32_t> source, int width,
                        IndexedImage& image, int transparentColorIndex) {
    if (width < 1 || source.empty() || source.size() % static_cast<std::size_t>(width) != 0 ||
        source.size() != image.pixels.size() || image.palette.empty() || image.palette.size() > 256 ||
        transparentColorIndex < -1 || transparentColorIndex >= static_cast<int>(image.palette.size())) {
        throw std::invalid_argument("Invalid Floyd-Steinberg image or palette");
    }
    if (image.exact) { return; }
    const auto rowSize = static_cast<std::size_t>(width) + 2;
    std::vector<std::array<double, 3>> currentErrors(rowSize);
    std::vector<std::array<double, 3>> nextErrors(rowSize);
    struct NearestColor {
        std::uint32_t key = std::numeric_limits<std::uint32_t>::max();
        std::uint8_t index = 0;
    };
    std::vector<NearestColor> nearest(32768);
    int maximumError = 0;
    for (std::size_t row = 0; row < source.size() / static_cast<std::size_t>(width); ++row) {
        for (int column = 0; column < width; ++column) {
            const auto pixel = row * static_cast<std::size_t>(width) + static_cast<std::size_t>(column);
            if (image.pixels[pixel] == transparentColorIndex) { continue; }
            std::array<int, 3> adjusted{};
            const auto errorColumn = static_cast<std::size_t>(column) + 1;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto original = static_cast<int>((source[pixel] >> (channel * 8)) & 255);
                adjusted[channel] = std::clamp(static_cast<int>(std::lround(original + currentErrors[errorColumn][channel])), 0, 255);
            }
            const auto key = static_cast<std::uint32_t>((adjusted[2] << 16) | (adjusted[1] << 8) | adjusted[0]);
            // An exact-key cache preserves distinct decisions for nearby colors
            // on opposite sides of a palette boundary.
            auto& cached = nearest[(key * 0x9E3779B1U) >> 17];
            int selected = cached.key == key ? cached.index : -1;
            if (selected < 0) {
                int bestDistance = std::numeric_limits<int>::max();
                for (std::size_t paletteIndex = 0; paletteIndex < image.palette.size(); ++paletteIndex) {
                    if (static_cast<int>(paletteIndex) == transparentColorIndex) { continue; }
                    int distance = 0;
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const auto difference = adjusted[channel] - static_cast<int>((image.palette[paletteIndex] >> (channel * 8)) & 255);
                        distance += difference * difference;
                    }
                    if (distance < bestDistance) { bestDistance = distance; selected = static_cast<int>(paletteIndex); }
                }
                if (selected < 0) { throw std::invalid_argument("Dither palette has no opaque color"); }
                cached = {key, static_cast<std::uint8_t>(selected)};
            }
            image.pixels[pixel] = static_cast<std::uint8_t>(selected);
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto mapped = static_cast<int>((image.palette[static_cast<std::size_t>(selected)] >> (channel * 8)) & 255);
                const auto original = static_cast<int>((source[pixel] >> (channel * 8)) & 255);
                maximumError = std::max(maximumError, std::abs(original - mapped));
                const double error = static_cast<double>(adjusted[channel] - mapped) / 16.0;
                currentErrors[errorColumn + 1][channel] += error * 7;
                nextErrors[errorColumn - 1][channel] += error * 3;
                nextErrors[errorColumn][channel] += error * 5;
                nextErrors[errorColumn + 1][channel] += error;
            }
        }
        currentErrors.swap(nextErrors);
        std::fill(nextErrors.begin(), nextErrors.end(), std::array<double, 3>{});
    }
    image.maximumChannelError = maximumError;
    image.exact = maximumError == 0;
}

} // namespace vispick::gif
