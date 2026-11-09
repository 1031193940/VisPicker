// Developer-only reviewed baseline from commit 70faa85 for before/after timing.
// It deliberately preserves the old node-based lookup and old Wu path.
#include "gif/Quantizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace vispick::quantizer_baseline {
using gif::IndexedImage;
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

IndexedImage QuantizeWu(std::span<const std::uint32_t> pixels) {
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
    for (const auto pixel : pixels) {
        result.pixels.push_back(lookup[static_cast<std::size_t>(Cell(
            static_cast<int>((pixel >> 19) & 31) + 1,
            static_cast<int>((pixel >> 11) & 31) + 1,
            static_cast<int>((pixel >> 3) & 31) + 1))]);
    }
    return result;
}

} // namespace

IndexedImage Quantize(std::span<const std::uint32_t> pixels) {
    IndexedImage result;
    result.pixels.reserve(pixels.size());
    std::unordered_map<std::uint32_t, std::uint8_t> colors;
    colors.reserve(255);
    std::uint32_t previousColor = 0;
    std::uint8_t previousIndex = 0;
    for (const auto pixel : pixels) {
        const auto color = pixel | 0xFF000000U;
        if (color != previousColor) {
            const auto found = colors.find(color);
            if (found != colors.end()) {
                previousIndex = found->second;
            } else {
                if (colors.size() == 255) {
                    return QuantizeWu(pixels);
                }
                previousIndex = static_cast<std::uint8_t>(colors.size());
                colors.emplace(color, previousIndex);
                result.palette.push_back(color);
            }
            previousColor = color;
        }
        result.pixels.push_back(previousIndex);
    }
    return result;
}

} // namespace vispick::gif
