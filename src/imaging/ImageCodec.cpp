#include "imaging/ImageCodec.h"
#include "platform/FileCommit.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vispick {
namespace {
using Microsoft::WRL::ComPtr;
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { Close(); }
    void Close() noexcept { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
};
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) { throw std::runtime_error(operation); }
}
struct Wic {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IWICImagingFactory> factory;
    Wic() {
        if (FAILED(status) && status != RPC_E_CHANGED_MODE) { throw std::runtime_error("Initialize imaging COM failed"); }
        const auto created = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(created)) { if (SUCCEEDED(status)) { CoUninitialize(); } Check(created, "Create WIC factory failed"); }
    }
    ~Wic() { factory.Reset(); if (SUCCEEDED(status)) { CoUninitialize(); } }
};
void CheckSize(UINT width, UINT height) {
    constexpr std::uint64_t maximumPixels = sizeof(void*) == 8 ? 64ULL * 1024 * 1024 : 32ULL * 1024 * 1024;
    if (!width || !height || static_cast<std::uint64_t>(width) * height > maximumPixels) {
        throw std::length_error("Image dimensions exceed the supported memory limit");
    }
}
Bitmap Convert(Wic& context, IWICBitmapSource* source) {
    UINT width = 0, height = 0;
    Check(source->GetSize(&width, &height), "Read image dimensions failed");
    CheckSize(width, height);
    ComPtr<IWICFormatConverter> converter;
    Check(context.factory->CreateFormatConverter(&converter), "Create image converter failed");
    Check(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom), "Convert image to BGRA failed");
    Bitmap result(static_cast<int>(width), static_cast<int>(height));
    Check(converter->CopyPixels(nullptr, static_cast<UINT>(result.StrideBytes()),
        static_cast<UINT>(result.Pixels().size_bytes()), reinterpret_cast<BYTE*>(result.Pixels().data())), "Read image pixels failed");
    return result;
}
ComPtr<IWICBitmap> Source(Wic& context, BitmapView image) {
    CheckSize(static_cast<UINT>(image.Width()), static_cast<UINT>(image.Height()));
    ComPtr<IWICBitmap> bitmap;
    // WIC copies the memory supplied to CreateBitmapFromMemory.
    Check(context.factory->CreateBitmapFromMemory(static_cast<UINT>(image.Width()), static_cast<UINT>(image.Height()),
        GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(image.StrideBytes()), static_cast<UINT>(image.Pixels().size_bytes()),
        reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(image.Pixels().data())), &bitmap), "Create WIC bitmap failed");
    return bitmap;
}
Bitmap Decode(Wic& context, IWICBitmapDecoder* decoder) {
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "Read first image frame failed");
    return Convert(context, frame.Get());
}
}

Bitmap ReadImage(const std::filesystem::path& path) {
    const auto source = ExtendedFilePath(path);
    Wic context;
    ComPtr<IWICBitmapDecoder> decoder;
    Check(context.factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder), "Open image file failed");
    return Decode(context, decoder.Get());
}

Bitmap DecodeImage(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 512ULL * 1024 * 1024) { throw std::length_error("Encoded image buffer is empty or too large"); }
    Wic context;
    ComPtr<IWICStream> stream;
    Check(context.factory->CreateStream(&stream), "Create image stream failed");
    Check(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())), "Read image memory failed");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(context.factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "Decode image memory failed");
    return Decode(context, decoder.Get());
}

namespace {
void EncodeToStream(Wic& context, BitmapView image, SaveFormat format, int jpgQuality, IStream* stream) {
    if (jpgQuality < 1 || jpgQuality > 100) { throw std::invalid_argument("JPEG quality must be 1..100"); }
    CheckSize(static_cast<UINT>(image.Width()), static_cast<UINT>(image.Height()));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapEncoder> encoder;
    const auto container = format == SaveFormat::Png ? GUID_ContainerFormatPng :
        format == SaveFormat::Jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatBmp;
    Check(context.factory->CreateEncoder(container, nullptr, &encoder), "Create image encoder failed");
    Check(encoder->Initialize(stream, WICBitmapEncoderNoCache), "Initialize image encoder failed");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&frame, &properties), "Create output frame failed");
    if (format == SaveFormat::Jpg) {
        PROPBAG2 property{};
        property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(jpgQuality) / 100.0F;
        Check(properties->Write(1, &property, &value), "Set JPEG quality failed");
    }
    Check(frame->Initialize(properties.Get()), "Initialize output frame failed");
    Check(frame->SetSize(static_cast<UINT>(image.Width()), static_cast<UINT>(image.Height())), "Set output dimensions failed");
    // Baseline Win7 BMP has no BGRA encoder. Use BGR for JPEG and BMP;
    // PNG retains straight alpha without requiring a platform update.
    GUID pixelFormat = format == SaveFormat::Png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&pixelFormat), "Set output pixel format failed");
    ComPtr<IWICFormatConverter> converted;
    if (format == SaveFormat::Png && pixelFormat == GUID_WICPixelFormat32bppBGRA) {
        // The encoder accepts the packed source pixels directly.
        Check(frame->WritePixels(static_cast<UINT>(image.Height()), static_cast<UINT>(image.StrideBytes()),
            static_cast<UINT>(image.Pixels().size_bytes()),
            reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(image.Pixels().data()))), "Encode image pixels failed");
    } else {
        bitmap = Source(context, image);
        Check(context.factory->CreateFormatConverter(&converted), "Create encoding converter failed");
        Check(converted->Initialize(bitmap.Get(), pixelFormat, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Convert encoding pixels failed");
        Check(frame->WriteSource(converted.Get(), nullptr), "Encode image pixels failed");
    }
    Check(frame->Commit(), "Commit image frame failed");
    Check(encoder->Commit(), "Commit image container failed");
}

std::uint64_t EncodedSize(IStream* stream) {
    STATSTG information{};
    Check(stream->Stat(&information, STATFLAG_NONAME), "Read encoded image size failed");
    if (!information.cbSize.QuadPart || information.cbSize.QuadPart > 512ULL * 1024 * 1024) {
        throw std::length_error("Encoded image is empty or exceeds size limit");
    }
    return information.cbSize.QuadPart;
}
}

std::vector<std::uint8_t> EncodeImage(BitmapView image, SaveFormat format, int jpgQuality) {
    Wic context;
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create encoded image stream failed");
    EncodeToStream(context, image, format, jpgQuality, stream.Get());
    std::vector<std::uint8_t> result(static_cast<std::size_t>(EncodedSize(stream.Get())));
    Check(stream->Seek({}, STREAM_SEEK_SET, nullptr), "Seek encoded image failed");
    ULONG read = 0;
    Check(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read), "Read encoded image failed");
    if (read != result.size()) { throw std::runtime_error("Encoded image stream was truncated"); }
    return result;
}

HGLOBAL EncodeClipboardPng(BitmapView image) {
    Wic context;
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, FALSE, &stream), "Create PNG clipboard stream failed");
    HGLOBAL encoded = nullptr;
    try {
        EncodeToStream(context, image, SaveFormat::Png, 92, stream.Get());
        static_cast<void>(EncodedSize(stream.Get()));
        Check(GetHGlobalFromStream(stream.Get(), &encoded), "Read PNG clipboard memory failed");
    } catch (...) {
        if (SUCCEEDED(GetHGlobalFromStream(stream.Get(), &encoded))) {
            stream.Reset();
            GlobalFree(encoded);
        }
        throw;
    }
    return encoded;
}

void SaveImage(const std::filesystem::path& path, BitmapView image, SaveFormat format, int jpgQuality) {
    auto bytes = EncodeImage(image, format, jpgQuality);
    // Encode before opening any output. A failed write never truncates the
    // user's existing file; rename a complete sibling file on the same volume.
    const auto destination = ExtendedFilePath(path);
    const auto temporary = UniqueSiblingPath(destination, L".tmp");
    FileHandle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) { throw std::runtime_error("Create temporary output file failed"); }
    DWORD written = 0;
    const bool saved = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.value);
    file.Close();
    try {
        if (!saved) { throw std::runtime_error("Write temporary image file failed"); }
        CommitStagedFile(temporary, destination);
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

Bitmap ScaleImage(BitmapView image, int width, int height, ImageInterpolation interpolation) {
    CheckSize(static_cast<UINT>(width), static_cast<UINT>(height));
    Wic context;
    auto source = Source(context, image);
    ComPtr<IWICFormatConverter> premultiplied;
    Check(context.factory->CreateFormatConverter(&premultiplied), "Create scaling converter failed");
    Check(premultiplied->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom), "Premultiply scaling source failed");
    ComPtr<IWICBitmapScaler> scaler;
    Check(context.factory->CreateBitmapScaler(&scaler), "Create image scaler failed");
    const auto mode = interpolation == ImageInterpolation::Nearest ? WICBitmapInterpolationModeNearestNeighbor :
        interpolation == ImageInterpolation::Linear ? WICBitmapInterpolationModeLinear :
        interpolation == ImageInterpolation::Cubic ? WICBitmapInterpolationModeCubic : WICBitmapInterpolationModeFant;
    Check(scaler->Initialize(premultiplied.Get(), static_cast<UINT>(width), static_cast<UINT>(height), mode), "Scale image failed");
    return Convert(context, scaler.Get());
}

} // namespace vispick
