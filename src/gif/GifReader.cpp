#include "gif/GifReader.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick::gif {
namespace {

void Check(HRESULT status, const char* operation) {
    if (FAILED(status)) { throw std::runtime_error(operation); }
}

class ComScope {
public:
    ComScope() : status_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
        if (FAILED(status_) && status_ != RPC_E_CHANGED_MODE) { throw std::runtime_error("CoInitializeEx failed"); }
    }
    ~ComScope() {
        if (SUCCEEDED(status_)) { CoUninitialize(); }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT status_;
};

class PropVariantScope {
public:
    PropVariantScope() { PropVariantInit(&value); }
    ~PropVariantScope() { PropVariantClear(&value); }
    PropVariantScope(const PropVariantScope&) = delete;
    PropVariantScope& operator=(const PropVariantScope&) = delete;
    PROPVARIANT value;
};

unsigned int ReadUnsigned(IWICMetadataQueryReader* reader, const wchar_t* path, unsigned int fallback = 0,
                          bool required = false) {
    PropVariantScope property;
    const HRESULT status = reader->GetMetadataByName(path, &property.value);
    if (FAILED(status)) {
        if (!required && status == WINCODEC_ERR_PROPERTYNOTFOUND) { return fallback; }
        throw std::runtime_error("Read GIF metadata field failed");
    }
    switch (property.value.vt) {
    case VT_UI1:
        return property.value.bVal;
    case VT_UI2:
        return property.value.uiVal;
    case VT_UI4:
        return property.value.ulVal;
    case VT_BOOL:
        return property.value.boolVal != VARIANT_FALSE ? 1U : 0U;
    default:
        throw std::runtime_error("Unexpected GIF metadata type");
    }
}

std::size_t CheckedPixelCount(unsigned int width, unsigned int height) {
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || count > (std::numeric_limits<UINT>::max)() / 4) {
        throw std::invalid_argument("GIF image dimensions exceed the supported buffer size");
    }
    return static_cast<std::size_t>(count);
}

void ValidateRegion(const GifDocumentMetadata& document, const GifFrameMetadata& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.left >= document.width || frame.top >= document.height ||
        frame.width > document.width - frame.left || frame.height > document.height - frame.top ||
        static_cast<unsigned int>(frame.disposal) > 3) {
        throw std::invalid_argument("GIF frame rectangle or disposal method is invalid");
    }
}

GifFrameMetadata ReadMetadata(IWICBitmapFrameDecode* frame, const GifDocumentMetadata& document) {
    Microsoft::WRL::ComPtr<IWICMetadataQueryReader> reader;
    Check(frame->GetMetadataQueryReader(reader.GetAddressOf()), "Read GIF frame metadata failed");
    GifFrameMetadata metadata;
    metadata.left = ReadUnsigned(reader.Get(), L"/imgdesc/Left", 0, true);
    metadata.top = ReadUnsigned(reader.Get(), L"/imgdesc/Top", 0, true);
    metadata.width = ReadUnsigned(reader.Get(), L"/imgdesc/Width", 0, true);
    metadata.height = ReadUnsigned(reader.Get(), L"/imgdesc/Height", 0, true);
    metadata.delayCentiseconds = static_cast<std::uint16_t>(ReadUnsigned(reader.Get(), L"/grctlext/Delay"));
    const auto disposal = ReadUnsigned(reader.Get(), L"/grctlext/Disposal");
    if (disposal > 3) { throw std::runtime_error("Unsupported reserved GIF disposal value"); }
    metadata.disposal = static_cast<DisposalMethod>(disposal);
    if (ReadUnsigned(reader.Get(), L"/grctlext/TransparencyFlag") != 0) {
        metadata.transparentColorIndex =
            static_cast<int>(ReadUnsigned(reader.Get(), L"/grctlext/TransparentColorIndex"));
    }
    ValidateRegion(document, metadata);
    return metadata;
}

} // namespace

struct GifReader::Implementation {
    ComScope com;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    GifDocumentMetadata metadata;

    [[nodiscard]] Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> Frame(unsigned int frameIndex) const {
        if (frameIndex >= metadata.frameCount) { throw std::out_of_range("GIF frame index is outside the animation"); }
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(frameIndex, frame.GetAddressOf()), "Read GIF frame failed");
        return frame;
    }

    explicit Implementation(const std::filesystem::path& path) {
        Check(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(factory.GetAddressOf())),
              "Create WIC factory failed");
        Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf()),
              "Decode GIF failed");
        GUID container{};
        Check(decoder->GetContainerFormat(&container), "Read GIF container failed");
        if (container != GUID_ContainerFormatGif) { throw std::invalid_argument("Image is not a GIF container"); }
        Check(decoder->GetFrameCount(&metadata.frameCount), "Read GIF frame count failed");
        if (metadata.frameCount == 0) { throw std::runtime_error("GIF has no image frames"); }
        Microsoft::WRL::ComPtr<IWICMetadataQueryReader> reader;
        Check(decoder->GetMetadataQueryReader(reader.GetAddressOf()), "Read GIF document metadata failed");
        metadata.width = ReadUnsigned(reader.Get(), L"/logscrdesc/Width", 0, true);
        metadata.height = ReadUnsigned(reader.Get(), L"/logscrdesc/Height", 0, true);
        (void)CheckedPixelCount(metadata.width, metadata.height);
        if (ReadUnsigned(reader.Get(), L"/logscrdesc/GlobalColorTableFlag") != 0) {
            Microsoft::WRL::ComPtr<IWICPalette> palette;
            Check(factory->CreatePalette(palette.GetAddressOf()), "Create WIC GIF palette failed");
            Check(decoder->CopyPalette(palette.Get()), "Read GIF global palette failed");
            UINT colorCount = 0;
            Check(palette->GetColorCount(&colorCount), "Read GIF palette size failed");
            std::vector<WICColor> colors(colorCount);
            UINT actualCount = 0;
            Check(palette->GetColors(colorCount, colors.data(), &actualCount), "Read GIF palette colors failed");
            const auto backgroundIndex = ReadUnsigned(reader.Get(), L"/logscrdesc/BackgroundColorIndex");
            if (backgroundIndex >= actualCount) {
                throw std::runtime_error("GIF background index exceeds global palette");
            }
            metadata.backgroundColor = colors[backgroundIndex] | 0xFF000000U;
        }
    }
};

GifReader::GifReader(const std::filesystem::path& path) : implementation_(std::make_unique<Implementation>(path)) {}
GifReader::~GifReader() = default;

const GifDocumentMetadata& GifReader::Metadata() const noexcept {
    return implementation_->metadata;
}

GifFrameMetadata GifReader::ReadFrameMetadata(unsigned int frameIndex) const {
    return ReadMetadata(implementation_->Frame(frameIndex).Get(), Metadata());
}

DecodedGifFrame GifReader::ReadFrame(unsigned int frameIndex) const {
    const auto frame = implementation_->Frame(frameIndex);
    DecodedGifFrame decoded{ReadMetadata(frame.Get(), Metadata()), {}};
    const auto& metadata = decoded.metadata;
    UINT decodedWidth = 0;
    UINT decodedHeight = 0;
    Check(frame->GetSize(&decodedWidth, &decodedHeight), "Read raw GIF frame size failed");
    if (decodedWidth != metadata.width || decodedHeight != metadata.height) {
        throw std::runtime_error("GIF descriptor differs from raw frame dimensions");
    }
    decoded.pixels.resize(CheckedPixelCount(metadata.width, metadata.height));
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    Check(implementation_->factory->CreateFormatConverter(converter.GetAddressOf()), "Create WIC converter failed");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom),
          "Convert GIF subframe failed");
    Check(converter->CopyPixels(nullptr, metadata.width * 4, static_cast<UINT>(decoded.pixels.size() * 4),
                                reinterpret_cast<BYTE*>(decoded.pixels.data())),
          "Copy GIF subframe pixels failed");
    return decoded;
}

GifCanvas::GifCanvas(const GifDocumentMetadata& metadata)
    : document_(metadata), pixels_(CheckedPixelCount(metadata.width, metadata.height), metadata.backgroundColor) {}

void GifCanvas::Compose(const DecodedGifFrame& frame) {
    ValidateRegion(document_, frame.metadata);
    if (frame.pixels.size() != static_cast<std::size_t>(frame.metadata.width) * frame.metadata.height) {
        throw std::invalid_argument("GIF raw frame pixel count does not match its rectangle");
    }
    if (hasPrevious_ && previous_.disposal == DisposalMethod::Background) {
        for (unsigned int row = 0; row < previous_.height; ++row) {
            const auto offset = static_cast<std::size_t>(previous_.top + row) * document_.width + previous_.left;
            std::fill_n(pixels_.begin() + static_cast<std::ptrdiff_t>(offset), previous_.width,
                        document_.backgroundColor);
        }
    } else if (hasPrevious_ && previous_.disposal == DisposalMethod::Previous) {
        pixels_ = savedPixels_;
    }
    if (frame.metadata.disposal == DisposalMethod::Previous) {
        savedPixels_ = pixels_;
    } else {
        savedPixels_.clear();
    }
    for (unsigned int row = 0; row < frame.metadata.height; ++row) {
        const auto sourceOffset = static_cast<std::size_t>(row) * frame.metadata.width;
        const auto targetOffset =
            static_cast<std::size_t>(frame.metadata.top + row) * document_.width + frame.metadata.left;
        for (unsigned int column = 0; column < frame.metadata.width; ++column) {
            const auto pixel = frame.pixels[sourceOffset + column];
            if ((pixel >> 24) != 0) { pixels_[targetOffset + column] = pixel | 0xFF000000U; }
        }
    }
    previous_ = frame.metadata;
    hasPrevious_ = true;
}

} // namespace vispick::gif
