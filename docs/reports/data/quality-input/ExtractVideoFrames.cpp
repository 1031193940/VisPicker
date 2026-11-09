// Developer-only dataset preparation; not part of any VisPick build target.
// Decode genuine consecutive source frames with Windows Media Foundation and
// write lossless PNGs using WIC. See SOURCES.json for the pinned source/hash.
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

void Check(HRESULT status, const char* operation) {
    if (FAILED(status)) {
        std::ostringstream message;
        message << operation << " failed: HRESULT=0x" << std::hex << static_cast<unsigned long>(status);
        throw std::runtime_error(message.str());
    }
}

class MediaRuntime {
public:
    MediaRuntime() {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        const auto status = MFStartup(MF_VERSION);
        if (FAILED(status)) { CoUninitialize(); Check(status, "MFStartup"); }
    }
    ~MediaRuntime() { MFShutdown(); CoUninitialize(); }
};

class LockedFrame {
public:
    LockedFrame(IMFMediaBuffer* buffer, LONG defaultStride, UINT height) : buffer_(buffer) {
        if (SUCCEEDED(buffer_.As(&twoDimensional_))) {
            Check(twoDimensional_->Lock2D(&firstRow, &stride), "Lock2D");
            locked2d_ = true;
        } else {
            Check(buffer_->Lock(&firstRow, nullptr, nullptr), "Lock");
            locked_ = true;
            stride = defaultStride;
            if (stride < 0) { firstRow += static_cast<std::ptrdiff_t>(height - 1) * -stride; }
        }
    }
    ~LockedFrame() {
        if (locked2d_) { twoDimensional_->Unlock2D(); }
        else if (locked_) { buffer_->Unlock(); }
    }
    BYTE* firstRow = nullptr;
    LONG stride = 0;
private:
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> twoDimensional_;
    bool locked2d_ = false;
    bool locked_ = false;
};

void SavePng(IWICImagingFactory* factory, const std::filesystem::path& path,
             UINT width, UINT height, std::span<const BYTE> pixels) {
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "CreateStream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    Check(frame->Initialize(nullptr), "Initialize frame");
    Check(frame->SetSize(width, height), "SetSize");
    GUID format = GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&format), "SetPixelFormat");
    if (format != GUID_WICPixelFormat24bppBGR) { throw std::runtime_error("PNG encoder changed requested BGR format"); }
    Check(frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data())), "WritePixels");
    Check(frame->Commit(), "Commit frame");
    Check(encoder->Commit(), "Commit encoder");
}

int wmain(int argumentCount, wchar_t** arguments) {
    try {
        if (argumentCount != 5) { throw std::invalid_argument("Usage: ExtractVideoFrames INPUT OUTPUT_DIR START_SECONDS FRAME_COUNT"); }
        const std::filesystem::path input(arguments[1]);
        const std::filesystem::path output(arguments[2]);
        const auto startTimestamp = std::stoll(arguments[3]) * 10000000LL;
        const int wantedFrames = std::stoi(arguments[4]);
        if (startTimestamp < 0 || wantedFrames < 1 || wantedFrames > 60) { throw std::invalid_argument("Invalid seek/count"); }
        std::filesystem::create_directories(output);
        MediaRuntime runtime;
        ComPtr<IMFAttributes> attributes;
        Check(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
        Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enable video processing");
        Check(attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE), "Disable DXVA for reproducible CPU extraction");
        ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromURL(input.c_str(), attributes.Get(), &reader), "Create source reader");
        constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Disable streams");
        Check(reader->SetStreamSelection(videoStream, TRUE), "Enable video stream");
        ComPtr<IMFMediaType> requested;
        Check(MFCreateMediaType(&requested), "Create media type");
        Check(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
        Check(requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set RGB32");
        Check(reader->SetCurrentMediaType(videoStream, nullptr, requested.Get()), "Select RGB32 output");
        ComPtr<IMFMediaType> current;
        Check(reader->GetCurrentMediaType(videoStream, &current), "Get output type");
        UINT width = 0, height = 0, rateNumerator = 0, rateDenominator = 0;
        Check(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height), "Read frame size");
        Check(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &rateNumerator, &rateDenominator), "Read frame rate");
        if (width == 0 || height == 0 || width > 4096 || height > 4096) { throw std::runtime_error("Unsupported preparation dimensions"); }
        UINT32 storedStride = 0;
        LONG defaultStride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &storedStride))) { defaultStride = static_cast<LONG>(storedStride); }
        else { Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &defaultStride), "Read stride"); }
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = startTimestamp;
        Check(reader->SetCurrentPosition(GUID_NULL, position), "Seek");
        ComPtr<IWICImagingFactory> factory;
        Check(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())), "Create WIC factory");
        std::cout << "file,timestamp_100ns,width,height,fps_numerator,fps_denominator\n";
        int saved = 0;
        while (saved < wantedFrames) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            Check(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample), "ReadSample");
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) { break; }
            if (!sample || timestamp < startTimestamp) { continue; }
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(&buffer), "Get contiguous video frame");
            std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 3);
            {
                LockedFrame locked(buffer.Get(), defaultStride, height);
                for (UINT row = 0; row < height; ++row) {
                    const auto* source = locked.firstRow + static_cast<std::ptrdiff_t>(row) * locked.stride;
                    for (UINT column = 0; column < width; ++column) {
                        const auto target = (static_cast<std::size_t>(row) * width + column) * 3;
                        for (UINT channel = 0; channel < 3; ++channel) { pixels[target + channel] = source[column * 4 + channel]; }
                    }
                }
            }
            std::ostringstream name;
            name << "frame-" << std::setw(4) << std::setfill('0') << saved << ".png";
            SavePng(factory.Get(), output / name.str(), width, height, pixels);
            std::cout << name.str() << ',' << timestamp << ',' << width << ',' << height << ',' << rateNumerator << ',' << rateDenominator << '\n';
            ++saved;
        }
        if (saved != wantedFrames) { throw std::runtime_error("Source ended before requested consecutive frame count"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
