#include "spikes/QuantizerBenchmark.h"
#include "gif/GifWriter.h"
#include "gif/Quantizer.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <memory>
#include <numeric>
#include <stdexcept>

// Kept only in the developer benchmark target, never in vispick_core/product.
#include "spikes/QuantizerBenchmark.baseline.inl"

namespace vispick {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::uint32_t> MakePattern(int width, int height, int colorCount) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex) {
        const auto colorIndex =
            (pixelIndex * 37 + pixelIndex / static_cast<std::size_t>(width) / 7) % static_cast<std::size_t>(colorCount);
        pixels[pixelIndex] = 0xFF000000U | ((static_cast<std::uint32_t>(colorIndex) * 2654435761U) & 0x00FFFFFFU);
    }
    return pixels;
}

void AddSamples(SpikeResult& result, const std::string& prefix, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    result.measurements.push_back({prefix + "_median", samples[samples.size() / 2], "ms"});
    result.measurements.push_back(
        {prefix + "_p95", samples[static_cast<std::size_t>(0.95 * static_cast<double>(samples.size() - 1))], "ms"});
    result.measurements.push_back(
        {prefix + "_mean", std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()),
         "ms"});
}

double ElapsedMilliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void CheckWic(HRESULT status, const char* operation) {
    if (FAILED(status)) { throw std::runtime_error(operation); }
}

class QualityComScope {
public:
    QualityComScope() : status_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
        if (FAILED(status_) && status_ != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("Quality input COM initialization failed");
        }
    }
    ~QualityComScope() {
        if (SUCCEEDED(status_)) { CoUninitialize(); }
    }
    QualityComScope(const QualityComScope&) = delete;
    QualityComScope& operator=(const QualityComScope&) = delete;

private:
    HRESULT status_;
};

struct QualityImage {
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint32_t> pixels;
};

QualityImage LoadQualityImage(IWICImagingFactory* factory, const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    CheckWic(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                decoder.GetAddressOf()),
             "Cannot decode quality image");
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    CheckWic(decoder->GetFrame(0, frame.GetAddressOf()), "Cannot read quality image frame");
    QualityImage image;
    CheckWic(frame->GetSize(&image.sourceWidth, &image.sourceHeight), "Cannot read quality image dimensions");
    if (image.sourceWidth == 0 || image.sourceHeight == 0 ||
        static_cast<std::uint64_t>(image.sourceWidth) * image.sourceHeight > 64000000) {
        throw std::runtime_error("Quality image must contain 1..64 million pixels");
    }
    const double scale = std::min({1.0, 1920.0 / image.sourceWidth, 1080.0 / image.sourceHeight});
    image.width = std::max(1U, static_cast<UINT>(image.sourceWidth * scale));
    image.height = std::max(1U, static_cast<UINT>(image.sourceHeight * scale));
    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    if (image.width != image.sourceWidth || image.height != image.sourceHeight) {
        CheckWic(factory->CreateBitmapScaler(scaler.GetAddressOf()), "Cannot create quality image scaler");
        CheckWic(scaler->Initialize(frame.Get(), image.width, image.height, WICBitmapInterpolationModeFant),
                 "Cannot scale quality image");
        source = scaler;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    CheckWic(factory->CreateFormatConverter(converter.GetAddressOf()), "Cannot create quality image converter");
    CheckWic(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom),
             "Cannot convert quality image to BGRA");
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
    CheckWic(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.pixels.size() * 4),
                                   reinterpret_cast<BYTE*>(image.pixels.data())),
             "Cannot read quality pixels");
    return image;
}

void AddQualityError(SpikeResult& result, const std::string& prefix, std::span<const std::uint32_t> source,
                     const gif::IndexedImage& quantized) {
    std::array<double, 3> squaredErrors{};
    std::array<int, 3> maximumErrors{};
    for (std::size_t pixelIndex = 0; pixelIndex < source.size(); ++pixelIndex) {
        const auto mapped = quantized.palette[quantized.pixels[pixelIndex]];
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const unsigned shift = static_cast<unsigned>(channel) * 8;
            const int error =
                static_cast<int>((source[pixelIndex] >> shift) & 255) - static_cast<int>((mapped >> shift) & 255);
            maximumErrors[channel] = std::max(maximumErrors[channel], std::abs(error));
            squaredErrors[channel] += error * error;
        }
    }
    constexpr std::array<const char*, 3> names{"blue", "green", "red"};
    double totalSquaredError = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double rmse = std::sqrt(squaredErrors[channel] / static_cast<double>(source.size()));
        result.measurements.push_back(
            {prefix + "_" + names[channel] + "_max_error", static_cast<double>(maximumErrors[channel]), "levels"});
        result.measurements.push_back({prefix + "_" + names[channel] + "_rmse", rmse, "levels"});
        totalSquaredError += squaredErrors[channel];
    }
    const double meanSquaredError = totalSquaredError / static_cast<double>(source.size() * 3);
    result.measurements.push_back({prefix + "_rgb_rmse", std::sqrt(meanSquaredError), "levels"});
    result.measurements.push_back({prefix + "_rgb_lossless", meanSquaredError == 0 ? 1.0 : 0.0, "boolean"});
    if (meanSquaredError > 0) {
        result.measurements.push_back(
            {prefix + "_rgb_psnr", 10.0 * std::log10(255.0 * 255.0 / meanSquaredError), "dB"});
    }
}

void RunRealQualityInputs(SpikeResult& result, const SpikeOptions& options) {
    std::filesystem::path inputDirectory = std::filesystem::current_path() / L"docs/reports/data/quality-input";
    const DWORD environmentSize = GetEnvironmentVariableW(L"VISPICK_QUALITY_INPUT", nullptr, 0);
    if (environmentSize != 0) {
        std::vector<wchar_t> value(environmentSize);
        const DWORD written = GetEnvironmentVariableW(L"VISPICK_QUALITY_INPUT", value.data(), environmentSize);
        if (written == 0 || written >= environmentSize) {
            throw std::runtime_error("Cannot read VISPICK_QUALITY_INPUT");
        }
        inputDirectory = std::wstring(value.data(), written);
    }
    std::vector<std::filesystem::path> files;
    if (std::filesystem::exists(inputDirectory)) {
        for (const auto& entry : std::filesystem::directory_iterator(inputDirectory)) {
            if (!entry.is_regular_file()) { continue; }
            auto extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            if (extension == L".png" || extension == L".jpg" || extension == L".jpeg" || extension == L".bmp") {
                files.push_back(entry.path());
            }
        }
    } else if (environmentSize != 0) {
        throw std::runtime_error("VISPICK_QUALITY_INPUT directory does not exist");
    }
    if (files.size() > 64) { throw std::runtime_error("Quality benchmark accepts at most 64 input images per run"); }
    std::sort(files.begin(), files.end());
    result.measurements.push_back({"real_input_files", static_cast<double>(files.size()), "files"});
    if (files.empty()) {
        result.notes.push_back(
            "No PNG/JPG/BMP quality inputs found. Real photograph/video quality remains unvalidated; "
            "place inputs in docs/reports/data/quality-input or set VISPICK_QUALITY_INPUT.");
        return;
    }
    QualityComScope com;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    CheckWic(
        CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())),
        "Cannot create quality input WIC factory");
    gif::Quantizer temporal;
    gif::FrameEncoder encoder;
    std::unique_ptr<gif::GifWriter> sequence;
    UINT sequenceWidth = 0;
    UINT sequenceHeight = 0;
    std::size_t sequenceNumber = 0;
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const auto image = LoadQualityImage(factory.Get(), files[fileIndex]);
        const std::string prefix = "real_" + std::to_string(fileIndex);
        const auto name = files[fileIndex].filename().u8string();
        result.notes.push_back(prefix +
                               " input=" + std::string(reinterpret_cast<const char*>(name.data()), name.size()));
        result.measurements.push_back({prefix + "_source_width", static_cast<double>(image.sourceWidth), "px"});
        result.measurements.push_back({prefix + "_source_height", static_cast<double>(image.sourceHeight), "px"});
        result.measurements.push_back({prefix + "_width", static_cast<double>(image.width), "px"});
        result.measurements.push_back({prefix + "_height", static_cast<double>(image.height), "px"});
        gif::Quantizer fresh({false, 0});
        gif::IndexedImage independentlyQuantized;
        std::vector<double> samples;
        for (int iteration = 0; iteration < 3; ++iteration) {
            const auto start = Clock::now();
            fresh.QuantizeTo(image.pixels, independentlyQuantized);
            samples.push_back(ElapsedMilliseconds(start));
        }
        std::sort(samples.begin(), samples.end());
        result.measurements.push_back({prefix + "_wu_path", independentlyQuantized.exact ? 0.0 : 1.0, "boolean"});
        result.measurements.push_back({prefix + "_quantize_median", samples[1], "ms"});
        result.measurements.push_back({prefix + "_quantize_throughput",
                                       static_cast<double>(image.pixels.size()) / (samples[1] * 1000.0), "MPixels/s"});
        AddQualityError(result, prefix + "_fresh", image.pixels, independentlyQuantized);
        const auto temporalStart = Clock::now();
        const auto quantized = temporal.Quantize(image.pixels);
        result.measurements.push_back({prefix + "_temporal_quantize", ElapsedMilliseconds(temporalStart), "ms"});
        result.measurements.push_back({prefix + "_palette_reused", quantized.paletteReused ? 1.0 : 0.0, "boolean"});
        AddQualityError(result, prefix + "_temporal", image.pixels, quantized);
        const auto repeated = temporal.Quantize(image.pixels);
        const bool stablePalette = quantized.palette == repeated.palette;
        std::size_t changedIndices = 0;
        for (std::size_t pixelIndex = 0; pixelIndex < quantized.pixels.size(); ++pixelIndex) {
            changedIndices += quantized.pixels[pixelIndex] != repeated.pixels[pixelIndex] ? 1 : 0;
        }
        result.measurements.push_back({prefix + "_static_palette_changed", stablePalette ? 0.0 : 1.0, "boolean"});
        result.measurements.push_back(
            {prefix + "_static_changed_indices", static_cast<double>(changedIndices), "pixels"});
        result.passed = result.passed && stablePalette && changedIndices == 0;
        const auto encoded = encoder.EncodeIndexed(quantized);
        const auto gifPath = options.outputDirectory / (prefix + "_quantized.gif");
        {
            gif::GifWriter writer(gifPath, static_cast<int>(image.width), static_cast<int>(image.height));
            writer.WriteFrame(encoded, 100);
            writer.Finish();
        }
        std::vector<std::uint32_t> mapped(image.pixels.size());
        for (std::size_t pixelIndex = 0; pixelIndex < mapped.size(); ++pixelIndex) {
            mapped[pixelIndex] = quantized.palette[quantized.pixels[pixelIndex]];
        }
        const std::array<std::uint64_t, 1> expected{gif::PixelHash(mapped)};
        const auto decoded = gif::InspectGif(gifPath, expected);
        result.measurements.push_back(
            {prefix + "_wic_mismatched_frames", static_cast<double>(decoded.mismatchedFrames), "frames"});
        result.passed = result.passed && decoded.mismatchedFrames == 0;
        if (!sequence || image.width != sequenceWidth || image.height != sequenceHeight) {
            if (sequence) { sequence->Finish(); }
            sequenceWidth = image.width;
            sequenceHeight = image.height;
            sequence = std::make_unique<gif::GifWriter>(
                options.outputDirectory / ("quality_ordered_inputs_" + std::to_string(sequenceNumber++) + ".gif"),
                static_cast<int>(image.width), static_cast<int>(image.height));
        }
        sequence->WriteFrame(encoded, 10); // Deliberate 10fps comparison playback, not source cadence.
    }
    if (sequence) { sequence->Finish(); }
    result.notes.push_back(
        "Real inputs are decoded by Windows WIC, aspect-preserving Fant-scaled only when exceeding 1920x1080. "
        "Metrics compare decoded 8-bit RGB code values, without ICC conversion, EXIF orientation handling, "
        "linear-light conversion or dithering. "
        "Source decoding, GIF encoding and error measurement are excluded from quantization timings.");
    result.notes.push_back(
        "Per-file GIFs and ordered-input GIFs are saved for visual review. Ordered inputs are filename-sorted; "
        "equal-size contiguous groups play at 10fps solely for comparison, not the original video timing. "
        "Input provenance determines whether a sequence is real video; arbitrary photographs are not labelled video. "
        "PSNR is omitted for lossless RGB (mathematically infinite). Numeric success does not certify subjective "
        "banding/flicker quality.");
}

} // namespace

SpikeResult RunQuantizerBenchmark(const SpikeOptions& options) {
    SpikeResult result{"quantizer", true, {}, {}};
    const int iterations = std::clamp(options.iterations, 3, 15);
    result.measurements.push_back({"width", 1920, "px"});
    result.measurements.push_back({"height", 1080, "px"});
    result.measurements.push_back({"iterations_per_case", static_cast<double>(iterations), "runs"});
    for (const int colorCount : {2, 16, 200, 300}) {
        const auto pixels = MakePattern(1920, 1080, colorCount);
        const std::string prefix = "colors_" + std::to_string(colorCount);
        gif::Quantizer updated({false, 0}); // Compare quantization, independent of temporal reuse.
        gif::IndexedImage after;
        updated.QuantizeTo(pixels, after); // Warm allocation/code paths before timing.
        auto before = quantizer_baseline::Quantize(pixels);
        std::vector<double> beforeSamples;
        std::vector<double> afterSamples;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            // Alternate order to avoid assigning every warmed cache to one implementation.
            const auto measureBefore = [&] {
                const auto start = Clock::now();
                before = quantizer_baseline::Quantize(pixels);
                beforeSamples.push_back(ElapsedMilliseconds(start));
            };
            const auto measureAfter = [&] {
                const auto start = Clock::now();
                updated.QuantizeTo(pixels, after);
                afterSamples.push_back(ElapsedMilliseconds(start));
            };
            if ((iteration & 1) == 0) {
                measureBefore();
                measureAfter();
            } else {
                measureAfter();
                measureBefore();
            }
        }
        const bool identical = before.palette == after.palette && before.pixels == after.pixels;
        result.passed = result.passed && identical;
        result.measurements.push_back({prefix + "_baseline_mapping_equal", identical ? 1.0 : 0.0, "boolean"});
        result.measurements.push_back({prefix + "_exact", after.exact ? 1.0 : 0.0, "boolean"});
        result.measurements.push_back(
            {prefix + "_maximum_channel_error", static_cast<double>(after.maximumChannelError), "levels"});
        AddSamples(result, prefix + "_before", std::move(beforeSamples));
        AddSamples(result, prefix + "_after", std::move(afterSamples));
    }

    for (const int colorCount : {2, 3, 16, 255}) {
        const auto pixels = MakePattern(640, 360, colorCount);
        const auto indexed = gif::Quantize(pixels);
        const auto compact = gif::EncodeIndexedFrame(indexed);
        auto legacy = compact;
        legacy.minimumCodeSize = 8;
        legacy.compressedPixels = gif::EncodeLzw(indexed.pixels, 8);
        legacy.palette.resize(256, 0); // Reproduce reviewed fixed-256-table wire format.
        const auto prefix = "palette_" + std::to_string(colorCount);
        const auto compactPath = options.outputDirectory / (prefix + "_compact.gif");
        const auto legacyPath = options.outputDirectory / (prefix + "_baseline.gif");
        {
            gif::GifWriter compactWriter(compactPath, 640, 360);
            gif::GifWriter legacyWriter(legacyPath, 640, 360);
            compactWriter.WriteFrame(compact, 3);
            legacyWriter.WriteFrame(legacy, 3);
            compactWriter.Finish();
            legacyWriter.Finish();
        }
        const std::array<std::uint64_t, 1> expected{gif::PixelHash(pixels)};
        const auto compactInspection = gif::InspectGif(compactPath, expected);
        const auto legacyInspection = gif::InspectGif(legacyPath, expected);
        const bool decodedCorrectly = compactInspection.mismatchedFrames == 0 && legacyInspection.mismatchedFrames == 0;
        const auto compactBytes = std::filesystem::file_size(compactPath);
        const auto legacyBytes = std::filesystem::file_size(legacyPath);
        result.passed = result.passed && decodedCorrectly && (colorCount > 3 || compactBytes < legacyBytes);
        result.measurements.push_back({prefix + "_before_bytes", static_cast<double>(legacyBytes), "bytes"});
        result.measurements.push_back({prefix + "_after_bytes", static_cast<double>(compactBytes), "bytes"});
        result.measurements.push_back(
            {prefix + "_minimum_lzw_bits", static_cast<double>(compact.minimumCodeSize), "bits"});
        result.measurements.push_back({prefix + "_wic_bit_exact", decodedCorrectly ? 1.0 : 0.0, "boolean"});
    }

    // Explicitly synthetic quality smoke test. Real photograph/video validation
    // remains a separate manual/materials-dependent acceptance item.
    auto gradient = MakePattern(1920, 1080, 256);
    for (std::size_t pixelIndex = 0; pixelIndex < gradient.size(); ++pixelIndex) {
        gradient[pixelIndex] = 0xFF000000U | static_cast<std::uint32_t>(pixelIndex % 256) * 0x010101U;
    }
    gif::Quantizer temporal;
    const auto first = temporal.Quantize(gradient);
    const auto reuseStart = Clock::now();
    const auto second = temporal.Quantize(gradient);
    const double reuseMilliseconds = ElapsedMilliseconds(reuseStart);
    const bool staticStable = first.palette == second.palette && first.pixels == second.pixels;
    result.passed = result.passed && staticStable && second.paletteReused;
    result.measurements.push_back({"synthetic_static_palette_reused", second.paletteReused ? 1.0 : 0.0, "boolean"});
    result.measurements.push_back({"synthetic_static_mapping_unchanged", staticStable ? 1.0 : 0.0, "boolean"});
    result.measurements.push_back({"synthetic_static_reuse", reuseMilliseconds, "ms"});
    result.measurements.push_back(
        {"synthetic_static_maximum_channel_error", static_cast<double>(second.maximumChannelError), "levels"});
    result.notes.push_back(
        "Before = exact reviewed Quantizer.cpp from Git commit 70faa85, including unordered_map and Wu. "
        "After = reused open-address workspace and indexed buffer, temporal palette reuse disabled for the four "
        "throughput cases. "
        "Input is a deterministic synthetic low-run-locality pattern; run order alternates. No absolute speedup is "
        "assumed.");
    result.notes.push_back(
        "300-color input exercises Wu plus its measured error accounting; it is not a real photograph/video workload. "
        "Compact-size baselines reproduce the prior 256-entry local table and minimum LZW code size 8; both outputs "
        "are independently decoded through WIC.");
    result.notes.push_back(
        "Temporal reuse never approximates <=255-color UI input. For higher-color inputs, every accepted reused RGB "
        "channel "
        "is checked against a maximum error of 8 levels. Static synthetic-gradient indices are checked unchanged. "
        "No dithering is implemented; real photograph/video subjective flicker and banding acceptance requires "
        "inspecting the optional quality artifacts.");
    RunRealQualityInputs(result, options);
    return result;
}

} // namespace vispick
