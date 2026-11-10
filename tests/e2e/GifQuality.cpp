// Developer-only image analysis; no screen capture, HWND, or acceptance threshold.
// Source attribution and pinned SHA256 values: docs/reports/data/quality-input/SOURCES.json.
#include "gif/GifWriter.h"
#include "imaging/ImageCodec.h"
#include "platform/Console.h"
#include "platform/Gdi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using vispick::Bitmap;
using vispick::BitmapView;
namespace gif = vispick::gif;
namespace filesystem = std::filesystem;

void Require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

int Channel(std::uint32_t pixel, unsigned channel) {
    return static_cast<int>((pixel >> (channel * 8)) & 255U);
}

Bitmap DecodeIndices(const gif::IndexedImage& image, int width, int height) {
    Bitmap decoded(width, height);
    Require(decoded.Pixels().size() == image.pixels.size(), "Indexed image dimensions differ");
    for (std::size_t pixel = 0; pixel < image.pixels.size(); ++pixel) {
        decoded.Pixels()[pixel] = image.palette.at(image.pixels[pixel]) | 0xFF000000U;
    }
    return decoded;
}

struct ErrorStatistics {
    std::array<double, 3> meanAbsolute{};
    std::array<double, 3> rootMeanSquare{};
    std::array<int, 3> maximum{};
    double rgbRootMeanSquare = 0;
    double meanRgbDistance = 0;
    double percentile95RgbDistance = 0;
    double maximumRgbDistance = 0;
    std::size_t differingPixels = 0;
};

ErrorStatistics Measure(BitmapView original, BitmapView mapped) {
    Require(original.Pixels().size() == mapped.Pixels().size(), "Error measurement dimensions differ");
    ErrorStatistics statistics;
    std::vector<double> distances(original.Pixels().size());
    std::array<double, 3> squared{};
    for (std::size_t pixel = 0; pixel < distances.size(); ++pixel) {
        double distanceSquared = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            const int error =
                std::abs(Channel(original.Pixels()[pixel], channel) - Channel(mapped.Pixels()[pixel], channel));
            statistics.meanAbsolute[channel] += error;
            squared[channel] += static_cast<double>(error) * error;
            statistics.maximum[channel] = std::max(statistics.maximum[channel], error);
            distanceSquared += static_cast<double>(error) * error;
        }
        distances[pixel] = std::sqrt(distanceSquared);
        statistics.meanRgbDistance += distances[pixel];
        statistics.maximumRgbDistance = std::max(statistics.maximumRgbDistance, distances[pixel]);
        if (distanceSquared != 0) { ++statistics.differingPixels; }
    }
    const auto count = static_cast<double>(distances.size());
    for (unsigned channel = 0; channel < 3; ++channel) {
        statistics.meanAbsolute[channel] /= count;
        statistics.rootMeanSquare[channel] = std::sqrt(squared[channel] / count);
    }
    statistics.rgbRootMeanSquare = std::sqrt(std::accumulate(squared.begin(), squared.end(), 0.0) / (count * 3));
    statistics.meanRgbDistance /= count;
    const auto percentile = (distances.size() - 1) * 95 / 100;
    std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(percentile), distances.end());
    statistics.percentile95RgbDistance = distances[percentile];
    return statistics;
}

void WriteStatistics(std::ostream& stream, const ErrorStatistics& statistics) {
    stream << "{\"mae_rgb\":"
           << std::accumulate(statistics.meanAbsolute.begin(), statistics.meanAbsolute.end(), 0.0) / 3
           << ",\"rmse_rgb\":" << statistics.rgbRootMeanSquare << ",\"psnr_rgb_db\":";
    if (statistics.rgbRootMeanSquare == 0) {
        stream << "null";
    } else {
        stream << 20 * std::log10(255 / statistics.rgbRootMeanSquare);
    }
    stream << ",\"differing_pixels\":" << statistics.differingPixels
           << ",\"rgb_distance_mean\":" << statistics.meanRgbDistance
           << ",\"rgb_distance_p95\":" << statistics.percentile95RgbDistance
           << ",\"rgb_distance_max\":" << statistics.maximumRgbDistance;
    constexpr std::array<const char*, 3> names{"b", "g", "r"};
    for (unsigned channel = 0; channel < 3; ++channel) {
        stream << ",\"" << names[channel] << "_mae\":" << statistics.meanAbsolute[channel] << ",\"" << names[channel]
               << "_rmse\":" << statistics.rootMeanSquare[channel] << ",\"" << names[channel]
               << "_max_error\":" << statistics.maximum[channel];
    }
    stream << '}';
}

struct TemporalStatistics {
    std::size_t unchangedSourcePixels = 0;
    std::size_t changedOutputAtUnchangedSource = 0;
    double meanResidualChange = 0;
    void Measure(BitmapView currentSource, BitmapView previousSource, BitmapView currentOutput,
                 BitmapView previousOutput) {
        if (previousSource.Empty()) { return; }
        Require(currentSource.Pixels().size() == previousSource.Pixels().size(), "Sequence dimensions changed");
        for (std::size_t pixel = 0; pixel < currentSource.Pixels().size(); ++pixel) {
            const auto source = currentSource.Pixels()[pixel] & 0xFFFFFFU;
            const auto previous = previousSource.Pixels()[pixel] & 0xFFFFFFU;
            if (source == previous) {
                ++unchangedSourcePixels;
                if ((currentOutput.Pixels()[pixel] & 0xFFFFFFU) != (previousOutput.Pixels()[pixel] & 0xFFFFFFU)) {
                    ++changedOutputAtUnchangedSource;
                }
            }
            for (unsigned channel = 0; channel < 3; ++channel) {
                const int error = Channel(currentOutput.Pixels()[pixel], channel) - Channel(source, channel);
                const int previousError = Channel(previousOutput.Pixels()[pixel], channel) - Channel(previous, channel);
                meanResidualChange += std::abs(error - previousError);
            }
        }
        meanResidualChange /= static_cast<double>(currentSource.Pixels().size()) * 3;
    }
    void Write(std::ostream& stream) const {
        stream << "{\"unchanged_source_pixels\":" << unchangedSourcePixels
               << ",\"changed_output_at_unchanged_source\":" << changedOutputAtUnchangedSource
               << ",\"mean_absolute_temporal_error_residual_change\":" << meanResidualChange << '}';
    }
};

class ContactSheet {
public:
    explicit ContactSheet(int rows) : image_(960, rows * 210) {
        std::fill(image_.Pixels().begin(), image_.Pixels().end(), 0xFF20242AU);
    }
    void Add(int row, int column, BitmapView image, const std::wstring& label) {
        const double scale = std::min(320.0 / image.Width(), 180.0 / image.Height());
        const int width = std::max(1, static_cast<int>(image.Width() * scale));
        const int height = std::max(1, static_cast<int>(image.Height() * scale));
        const auto thumbnail = vispick::ScaleImage(image, width, height, vispick::ImageInterpolation::Fant);
        const int left = column * 320 + (320 - width) / 2;
        const int top = row * 210 + 28 + (180 - height) / 2;
        // Flush preceding text drawing before modifying the same DIB from the CPU.
        Require(GdiFlush() != FALSE, "Cannot flush contact-sheet drawing");
        for (int line = 0; line < height; ++line) {
            std::copy_n(thumbnail.Row(line), width,
                        image_.Pixels().begin() + static_cast<std::ptrdiff_t>((top + line) * 960 + left));
        }
        const auto context = image_.DeviceContext();
        const vispick::SelectObjectGuard font(context, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context, RGB(245, 245, 245));
        RECT title{column * 320 + 5, row * 210 + 3, (column + 1) * 320 - 5, row * 210 + 25};
        DrawTextW(context, label.c_str(), static_cast<int>(label.size()), &title,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    void Save(const filesystem::path& path) {
        Require(GdiFlush() != FALSE, "Cannot finish contact sheet");
        for (auto& pixel : image_.Pixels()) { pixel |= 0xFF000000U; }
        vispick::SaveImage(path, BitmapView(image_.Width(), image_.Height(), image_.Pixels()),
                           vispick::SaveFormat::Png);
    }

private:
    vispick::DibSection image_;
};

void VerifySingleGif(const filesystem::path& path, const gif::IndexedImage& image, BitmapView decoded,
                     gif::FrameEncoder& encoder) {
    {
        gif::GifWriter writer(path, decoded.Width(), decoded.Height());
        writer.WriteFrame(encoder.EncodeIndexed(image), 100);
        writer.Finish();
    }
    const std::array<std::uint64_t, 1> expected{gif::PixelHash(decoded.Pixels())};
    const auto inspection = gif::InspectGif(path, expected);
    Require(inspection.frameCount == 1 && inspection.mismatchedFrames == 0,
            "WIC GIF pixels differ from indexed output");
}

void VerifyExactColors(const filesystem::path& output) {
    gif::Quantizer quantizer({false, 0});
    gif::FrameEncoder encoder;
    for (const unsigned colors : {2U, 3U, 16U, 255U}) {
        Bitmap source(256, 64);
        for (std::size_t pixel = 0; pixel < source.Pixels().size(); ++pixel) {
            const auto color = static_cast<std::uint32_t>(pixel % colors);
            source.Pixels()[pixel] = 0xFF000000U | color | (((color * 47) % 256) << 8) | (((color * 113) % 256) << 16);
        }
        auto indexed = quantizer.Quantize(source.Pixels());
        const auto before = indexed;
        gif::DitherFloydSteinberg(source.Pixels(), source.Width(), indexed);
        Require(indexed.exact && indexed.palette == before.palette && indexed.pixels == before.pixels,
                "Dithering changed an exact <=255-color mapping");
        const auto decoded = DecodeIndices(indexed, source.Width(), source.Height());
        Require(Measure(source.View(), decoded.View()).differingPixels == 0, "Exact-color input lost RGB values");
        VerifySingleGif(output / (L"exact-" + std::to_wstring(colors) + L".gif"), indexed, source.View(), encoder);
    }
}

void Run(const filesystem::path& input, const filesystem::path& output) {
    filesystem::create_directories(output);
    VerifyExactColors(output);
    std::ofstream json(output / L"quality.json", std::ios::binary);
    Require(static_cast<bool>(json), "Cannot create quality report");
    json << std::setprecision(9)
         << "{\n  \"exact_color_cases\":[2,3,16,255],\n  \"comparison_fps\":10,\n  \"source_video_fps\":24,\n  "
            "\"frames\":[\n";
    ContactSheet photographs(2);
    ContactSheet video(4);
    gif::Quantizer quantizer({false, 0}); // Production workers likewise disable temporal palette reuse.
    gif::FrameEncoder encoder;
    std::unique_ptr<gif::GifWriter> sequencePlain;
    std::unique_ptr<gif::GifWriter> sequenceDithered;
    std::vector<std::uint64_t> plainHashes;
    std::vector<std::uint64_t> ditheredHashes;
    Bitmap previousSource, previousPlain, previousDithered;
    int videoContactRow = 0;
    for (unsigned item = 0; item < 14; ++item) {
        const auto filename =
            item == 0   ? filesystem::path(L"nasa-iss068e023088-sunset.jpg")
            : item == 1 ? filesystem::path(L"nasa-iss23-atmosphere.jpg")
                        : filesystem::path(L"video-bbb") / (L"frame-00" + std::wstring(item - 2 < 10 ? L"0" : L"") +
                                                            std::to_wstring(item - 2) + L".png");
        auto original = vispick::ReadImage(input / filename);
        const int sourceWidth = original.Width(), sourceHeight = original.Height();
        const double scale = std::min({1.0, 1920.0 / original.Width(), 1080.0 / original.Height()});
        if (scale < 1) {
            original = vispick::ScaleImage(original.View(), std::max(1, static_cast<int>(original.Width() * scale)),
                                           std::max(1, static_cast<int>(original.Height() * scale)),
                                           vispick::ImageInterpolation::Fant);
        }
        const auto quantizeStart = Clock::now();
        const auto plain = quantizer.Quantize(original.Pixels());
        const double quantizeMs = std::chrono::duration<double, std::milli>(Clock::now() - quantizeStart).count();
        auto dithered = plain;
        const auto ditherStart = Clock::now();
        gif::DitherFloydSteinberg(original.Pixels(), original.Width(), dithered);
        const double ditherMs = std::chrono::duration<double, std::milli>(Clock::now() - ditherStart).count();
        Require(plain.palette == dithered.palette, "Dithering changed the comparison palette");
        auto repeated = quantizer.Quantize(original.Pixels());
        Require(plain.palette == repeated.palette && plain.pixels == repeated.pixels,
                "Repeated static quantization differs");
        gif::DitherFloydSteinberg(original.Pixels(), original.Width(), repeated);
        Require(dithered.pixels == repeated.pixels, "Repeated static dithering differs");
        auto plainImage = DecodeIndices(plain, original.Width(), original.Height());
        auto ditheredImage = DecodeIndices(dithered, original.Width(), original.Height());
        const auto plainError = Measure(original.View(), plainImage.View());
        const auto ditheredError = Measure(original.View(), ditheredImage.View());
        if (plain.exact) {
            Require(plainError.differingPixels == 0 && ditheredError.differingPixels == 0, "Exact input was altered");
        }
        const auto stem = filename.stem().wstring();
        const auto plainPng = output / (stem + L"-wu.png");
        const auto ditheredPng = output / (stem + L"-fs.png");
        vispick::SaveImage(plainPng, plainImage.View(), vispick::SaveFormat::Png);
        vispick::SaveImage(ditheredPng, ditheredImage.View(), vispick::SaveFormat::Png);
        Require(Measure(plainImage.View(), vispick::ReadImage(plainPng).View()).differingPixels == 0 &&
                    Measure(ditheredImage.View(), vispick::ReadImage(ditheredPng).View()).differingPixels == 0,
                "WIC PNG roundtrip differs");
        VerifySingleGif(output / (stem + L"-wu.gif"), plain, plainImage.View(), encoder);
        VerifySingleGif(output / (stem + L"-fs.gif"), dithered, ditheredImage.View(), encoder);
        TemporalStatistics plainTemporal, ditheredTemporal;
        if (item >= 2) {
            if (!sequencePlain) {
                sequencePlain =
                    std::make_unique<gif::GifWriter>(output / L"bbb-wu-10fps.gif", original.Width(), original.Height());
                sequenceDithered =
                    std::make_unique<gif::GifWriter>(output / L"bbb-fs-10fps.gif", original.Width(), original.Height());
            }
            sequencePlain->WriteFrame(encoder.EncodeIndexed(plain), 10);
            sequenceDithered->WriteFrame(encoder.EncodeIndexed(dithered), 10);
            plainHashes.push_back(gif::PixelHash(plainImage.Pixels()));
            ditheredHashes.push_back(gif::PixelHash(ditheredImage.Pixels()));
            plainTemporal.Measure(original.View(), previousSource.View(), plainImage.View(), previousPlain.View());
            ditheredTemporal.Measure(original.View(), previousSource.View(), ditheredImage.View(),
                                     previousDithered.View());
        }
        if (item < 2 || item == 2 || item == 5 || item == 9 || item == 13) {
            auto& sheet = item < 2 ? photographs : video;
            const int row = item < 2 ? static_cast<int>(item) : videoContactRow++;
            const auto title = item < 2 ? (item == 0 ? L"NASA sunset" : L"NASA atmosphere") : stem;
            sheet.Add(row, 0, original.View(), title + std::wstring(L" | Original"));
            sheet.Add(row, 1, plainImage.View(), title + std::wstring(L" | Wu"));
            sheet.Add(row, 2, ditheredImage.View(), title + std::wstring(L" | Wu + FS"));
        }
        json << (item == 0 ? "    " : ",\n    ") << "{\"input\":\"" << vispick::WideToUtf8(filename.generic_wstring())
             << "\",\"source_width\":" << sourceWidth << ",\"source_height\":" << sourceHeight
             << ",\"width\":" << original.Width() << ",\"height\":" << original.Height()
             << ",\"palette_colors\":" << plain.palette.size() << ",\"exact\":" << (plain.exact ? "true" : "false")
             << ",\"same_comparison_palette\":true,\"static_repeat_equal\":true,\"wic_png_gif_bit_exact\":true"
             << ",\"quantize_ms\":" << quantizeMs << ",\"dither_ms\":" << ditherMs << ",\"wu\":";
        WriteStatistics(json, plainError);
        json << ",\"fs\":";
        WriteStatistics(json, ditheredError);
        json << ",\"wu_temporal\":";
        plainTemporal.Write(json);
        json << ",\"fs_temporal\":";
        ditheredTemporal.Write(json);
        json << '}';
        std::cout << "PASS " << vispick::WideToUtf8(filename.generic_wstring()) << std::endl;
        if (item >= 2) {
            previousSource = std::move(original);
            previousPlain = std::move(plainImage);
            previousDithered = std::move(ditheredImage);
        }
    }
    sequencePlain->Finish();
    sequenceDithered->Finish();
    const auto plainInspection = gif::InspectGif(output / L"bbb-wu-10fps.gif", plainHashes);
    const auto ditheredInspection = gif::InspectGif(output / L"bbb-fs-10fps.gif", ditheredHashes);
    Require(plainInspection.frameCount == 12 && ditheredInspection.frameCount == 12 &&
                plainInspection.mismatchedFrames == 0 && ditheredInspection.mismatchedFrames == 0,
            "WIC sequence output differs");
    photographs.Save(output / L"photo-contact.png");
    video.Save(output / L"video-contact.png");
    json << "\n  ],\n  \"numeric_checks_passed\":true,\n  \"visual_acceptance\":\"requires_human_review\"\n}\n";
    Require(static_cast<bool>(json), "Cannot finish quality JSON");
    std::ofstream notes(output / L"README.txt", std::ios::binary);
    notes << "Columns in contact sheets: Original / Wu / same Wu palette + Floyd-Steinberg.\n"
             "Full-resolution per-input PNG and GIF files retain the -wu / -fs distinction.\n"
             "Inputs and attribution: docs/reports/data/quality-input/SOURCES.json.\n"
             "NASA / ISS Expedition 23. Big Buck Bunny: (c) copyright 2008 Blender Foundation / www.bigbuckbunny.org, "
             "CC BY 3.0.\n"
             "BBB input: 12 consecutive 24fps frames, source timestamps 30.0000000 to 30.4583333 seconds.\n"
             "Both animated comparison GIFs deliberately play at 10fps; this is not the source cadence.\n"
             "Wu palette is independently computed per frame (as in production parallel workers); no temporal palette "
             "reuse.\n"
             "These are full-frame comparisons. Production changed-rectangle/transparency composition is not exercised "
             "here; temporal counts are not production-recorder flicker measurements.\n"
             "Errors use WIC-decoded 8-bit RGB code values. RGB distance is Euclidean in that code-value space, not "
             "CIE delta-E.\n"
             "No ICC transform or EXIF orientation correction. PSNR null denotes zero RMSE / infinite PSNR.\n"
             "Temporal statistics compare same-position pixels and temporal quantization residual differences, not "
             "motion-compensated perceptual flicker.\n"
             "Static repeat, same comparison palette, exact <=255 colors and WIC PNG/GIF pixel equality are "
             "correctness checks.\n"
             "MAE/PSNR/distance/timing are observations with no quality threshold. Dither may increase numeric error "
             "while reducing banding.\n"
             "This small corpus does not establish broad photo/video quality or 1080p video performance acceptance.\n";
    Require(static_cast<bool>(notes), "Cannot finish quality notes");
}
} // namespace

int wmain() {
    try {
        auto input = filesystem::path(L"docs/reports/data/quality-input");
        auto output = filesystem::path(L"out/reports/p5-quality");
        const auto arguments = vispick::ProcessArguments();
        for (std::size_t argument = 0; argument < arguments.size(); ++argument) {
            const auto option = arguments[argument];
            if (++argument >= arguments.size()) { throw std::invalid_argument("Missing GIF quality path"); }
            if (option == L"--input") {
                input = arguments[argument];
            } else if (option == L"--output") {
                output = arguments[argument];
            } else {
                throw std::invalid_argument("Usage: VisPickGifQuality [--input corpus-directory] [--output directory]");
            }
        }
        Run(input, output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
