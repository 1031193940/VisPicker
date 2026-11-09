#include "imaging/Clipboard.h"
#include "imaging/ImageCodec.h"
#include "platform/FileCommit.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace vispick {
namespace {
struct ClipboardScope {
    explicit ClipboardScope(HWND owner) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (OpenClipboard(owner)) { return; }
            if (attempt != 9) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        }
        throw std::runtime_error("Clipboard is busy; please try again");
    }
    ~ClipboardScope() { CloseClipboard(); }
};
struct Memory {
    HGLOBAL handle = nullptr;
    explicit Memory(std::size_t size, UINT flags = GMEM_MOVEABLE) {
        handle = GlobalAlloc(flags, size);
        if (!handle) { throw std::bad_alloc(); }
    }
    explicit Memory(HGLOBAL encoded) noexcept : handle(encoded) {}
    ~Memory() { if (handle) { GlobalFree(handle); } }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    [[nodiscard]] bool Publish(UINT format) noexcept {
        if (!handle || !SetClipboardData(format, handle)) { return false; }
        handle = nullptr;
        return true;
    }
};
struct LockedMemory {
    HGLOBAL handle;
    std::uint8_t* data;
    explicit LockedMemory(HGLOBAL memory) : handle(memory), data(static_cast<std::uint8_t*>(GlobalLock(handle))) {
        if (!data) { throw std::runtime_error("Lock clipboard memory failed"); }
    }
    ~LockedMemory() { GlobalUnlock(handle); }
    LockedMemory(const LockedMemory&) = delete;
    LockedMemory& operator=(const LockedMemory&) = delete;
};
std::vector<std::uint8_t> ReadBytes(UINT format) {
    const auto memory = GetClipboardData(format);
    if (!memory) { throw std::runtime_error("Read clipboard format failed"); }
    const auto size = GlobalSize(memory);
    if (!size || size > 512ULL * 1024 * 1024) { throw std::length_error("Clipboard image is empty or too large"); }
    const LockedMemory locked(memory);
    return {locked.data, locked.data + size};
}
template<typename T> T Read(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) { throw std::invalid_argument("Truncated DIB header or palette"); }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}
unsigned Channel(std::uint32_t pixel, std::uint32_t mask) {
    if (!mask) { return 0; }
    while (!(mask & 1U)) { mask >>= 1; pixel >>= 1; }
    return static_cast<unsigned>((static_cast<std::uint64_t>(pixel & mask) * 255 + mask / 2) / mask);
}
bool Contiguous(std::uint32_t mask) {
    if (!mask) { return false; }
    while (!(mask & 1U)) { mask >>= 1; }
    return (mask & (mask + 1)) == 0;
}
std::size_t DibSize(BitmapView image) {
    if (image.Empty() || image.Pixels().size_bytes() > (std::numeric_limits<DWORD>::max)()) {
        throw std::length_error("Image does not fit a clipboard DIB");
    }
    return sizeof(BITMAPINFOHEADER) + image.Pixels().size_bytes();
}

void WriteDib(std::uint8_t* bytes, BitmapView image) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = image.Width();
    header.biHeight = -image.Height();
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(image.Pixels().size_bytes());
    std::memcpy(bytes, &header, sizeof(header));
    auto* destination = bytes + sizeof(header);
    for (auto pixel : image.Pixels()) {
        // CF_DIB has no reliable alpha interpretation. Composite onto white.
        const auto alpha = pixel >> 24;
        if (alpha != 255) {
            pixel = 0xFF000000U | (((pixel & 255) * alpha + 255 * (255 - alpha) + 127) / 255) |
                (((((pixel >> 8) & 255) * alpha + 255 * (255 - alpha) + 127) / 255) << 8) |
                (((((pixel >> 16) & 255) * alpha + 255 * (255 - alpha) + 127) / 255) << 16);
        }
        std::memcpy(destination, &pixel, sizeof(pixel));
        destination += sizeof(pixel);
    }
}
}

struct GifClipboardData::Implementation {
    UINT nativeFormat = 0;
    UINT preferredDropEffectFormat = 0;
    std::optional<Memory> animation;
    std::optional<Memory> fileDrop;
    std::optional<Memory> preferredDropEffect;

    explicit Implementation(const std::filesystem::path& file) {
        const auto& path = file.native();
        if (!file.is_absolute() || path.find(L'\0') != std::wstring::npos || path.size() >= 32768) {
            throw std::invalid_argument("Invalid clipboard GIF file path");
        }
        const auto extended = ExtendedFilePath(file);
        const auto size = std::filesystem::file_size(extended);
        if (size == 0) { throw std::invalid_argument("Clipboard GIF file is empty"); }
        fileDrop.emplace(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), GHND);
        {
            const LockedMemory drop(fileDrop->handle);
            const DROPFILES header{sizeof(DROPFILES), {}, FALSE, TRUE};
            std::memcpy(drop.data, &header, sizeof(header));
            std::memcpy(drop.data + sizeof(header), path.data(), path.size() * sizeof(wchar_t));
        }
        preferredDropEffectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (preferredDropEffectFormat) {
            preferredDropEffect.emplace(sizeof(DWORD), GHND);
            const LockedMemory effect(preferredDropEffect->handle);
            const DWORD copy = DROPEFFECT_COPY;
            std::memcpy(effect.data, &copy, sizeof(copy));
        }
        // Avoid duplicating large recordings into a contiguous clipboard allocation.
        // The file drop still carries the complete animation at any size.
        constexpr std::uintmax_t MaximumNativeGifBytes = 64ULL * 1024 * 1024;
        if (size > MaximumNativeGifBytes) { return; }
        nativeFormat = RegisterClipboardFormatW(L"image/gif");
        if (nativeFormat == 0) { return; }
        try { animation.emplace(static_cast<std::size_t>(size), GHND); }
        catch (const std::bad_alloc&) { return; }
        std::ifstream input(extended, std::ios::binary);
        const LockedMemory bytes(animation->handle);
        if (!input.read(reinterpret_cast<char*>(bytes.data), static_cast<std::streamsize>(size))) {
            throw std::runtime_error("Read clipboard GIF file failed");
        }
    }
};

GifClipboardData::GifClipboardData(const std::filesystem::path& file)
    : implementation_(std::make_unique<Implementation>(file)) {}

GifClipboardData::~GifClipboardData() = default;

GifClipboardFormats GifClipboardData::Publish(HWND owner) {
    if (!owner) { throw std::invalid_argument("Clipboard publishing requires an owner window"); }
    auto& data = *implementation_;
    if (!data.fileDrop->handle) { throw std::logic_error("Clipboard GIF file was already published"); }
    const ClipboardScope clipboard(owner);
    if (!EmptyClipboard()) { throw std::runtime_error("Empty clipboard failed"); }
    const bool animationPublished = data.animation && data.animation->Publish(data.nativeFormat);
    const bool filePublished = data.fileDrop->Publish(CF_HDROP);
    if (filePublished && data.preferredDropEffect) {
        static_cast<void>(data.preferredDropEffect->Publish(data.preferredDropEffectFormat));
    }
    if (!animationPublished && !filePublished) { throw std::runtime_error("Publish clipboard GIF failed"); }
    return {animationPublished, filePublished};
}

std::vector<std::uint8_t> EncodeDib(BitmapView image) {
    std::vector<std::uint8_t> bytes(DibSize(image));
    WriteDib(bytes.data(), image);
    return bytes;
}

Bitmap DecodeDib(std::span<const std::uint8_t> bytes) {
    const auto header = Read<BITMAPINFOHEADER>(bytes, 0);
    if ((header.biSize != 40 && header.biSize != 52 && header.biSize != 56 && header.biSize != 108 && header.biSize != 124) ||
        header.biSize > bytes.size() || header.biPlanes != 1 || header.biWidth <= 0 || header.biHeight == 0 ||
        header.biHeight == (std::numeric_limits<LONG>::min)() ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) {
        throw std::invalid_argument("Unsupported or malformed clipboard DIB header");
    }
    const auto bits = header.biBitCount;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        throw std::invalid_argument("Unsupported clipboard DIB bit depth");
    }
    const auto height = static_cast<unsigned>(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    constexpr std::uint64_t maximumPixels = sizeof(void*) == 8 ? 64ULL * 1024 * 1024 : 32ULL * 1024 * 1024;
    if (static_cast<std::uint64_t>(header.biWidth) * height > maximumPixels) { throw std::length_error("Clipboard DIB exceeds memory limit"); }
    std::size_t offset = header.biSize;
    std::array<std::uint32_t, 4> masks{bits == 16 ? 0x7C00U : 0xFF0000U, bits == 16 ? 0x3E0U : 0xFF00U, bits == 16 ? 0x1FU : 0xFFU, 0};
    if (header.biCompression == BI_BITFIELDS) {
        if (bits != 16 && bits != 32) { throw std::invalid_argument("Invalid DIB bitfields depth"); }
        for (std::size_t channel = 0; channel < 3; ++channel) { masks[channel] = Read<DWORD>(bytes, 40 + channel * 4); }
        if (header.biSize == 40) { offset += 12; }
        else if (header.biSize >= 56) { masks[3] = Read<DWORD>(bytes, 52); }
        std::uint32_t used = 0;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto mask = masks[channel];
            if ((channel != 3 || mask) && (!Contiguous(mask) || (mask & used) || (bits == 16 && mask > 0xFFFF))) {
                throw std::invalid_argument("Invalid overlapping or discontiguous DIB masks");
            }
            used |= mask;
        }
    }
    const auto paletteCount = bits <= 8 ? (header.biClrUsed ? header.biClrUsed : 1U << bits) : header.biClrUsed;
    if (paletteCount > 256 || (bits <= 8 && paletteCount > (1U << bits))) { throw std::invalid_argument("Invalid DIB palette size"); }
    const auto paletteOffset = offset;
    offset += static_cast<std::size_t>(paletteCount) * sizeof(RGBQUAD);
    const auto stride = ((static_cast<std::uint64_t>(header.biWidth) * bits + 31) / 32) * 4;
    if (offset > bytes.size() || stride * height > bytes.size() - offset) { throw std::invalid_argument("Truncated DIB pixels"); }
    // In a packed clipboard DIB, a profile follows the pixels. Do not mistake
    // its bytes for the first rows, and never dereference linked profile paths.
    if (header.biSize == 124) {
        const auto profileStart = Read<DWORD>(bytes, 112);
        const auto profileBytes = Read<DWORD>(bytes, 116);
        const auto colorSpace = Read<DWORD>(bytes, 56);
        if (profileBytes && (colorSpace == PROFILE_LINKED || colorSpace == PROFILE_EMBEDDED)) {
            if (profileStart < offset + stride * height || profileStart > bytes.size() || profileBytes > bytes.size() - profileStart) {
                throw std::invalid_argument("Invalid clipboard DIB color profile");
            }
        }
        // Some clipboard producers put BGRA in a V5 BI_RGB DIB. Treat all-zero
        // reserved bytes as conventional opaque data. BI_BITFIELDS uses its
        // explicit masks above, including an intentionally transparent image.
        if (header.biCompression == BI_RGB && bits == 32) {
            const auto firstAlpha = offset + 3;
            const auto pixelEnd = offset + static_cast<std::size_t>(stride * height);
            for (auto alpha = firstAlpha; alpha < pixelEnd; alpha += 4) {
                if (bytes[alpha] != 0) { masks[3] = 0xFF000000U; break; }
            }
        }
    }
    Bitmap result(header.biWidth, static_cast<int>(height));
    for (unsigned row = 0; row < height; ++row) {
        const auto sourceRow = header.biHeight < 0 ? row : height - row - 1;
        const auto* source = bytes.data() + offset + static_cast<std::size_t>(stride * sourceRow);
        auto* destination = result.Row(static_cast<int>(row));
        for (int column = 0; column < header.biWidth; ++column) {
            std::uint32_t color = 0;
            if (bits <= 8) {
                const auto bitOffset = static_cast<std::size_t>(column) * bits;
                const auto paletteIndex = (source[bitOffset / 8] >> (8 - bits - bitOffset % 8)) & ((1U << bits) - 1);
                if (paletteIndex >= paletteCount) { throw std::invalid_argument("DIB pixel exceeds color palette"); }
                color = Read<DWORD>(bytes, paletteOffset + paletteIndex * 4) | 0xFF000000;
            } else if (bits == 24) {
                const auto* pixel = source + static_cast<std::size_t>(column) * 3;
                color = 0xFF000000U | pixel[0] | (static_cast<DWORD>(pixel[1]) << 8) | (static_cast<DWORD>(pixel[2]) << 16);
            } else {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, source + static_cast<std::size_t>(column) * (bits / 8), bits / 8);
                color = (masks[3] ? Channel(pixel, masks[3]) : 255U) << 24 |
                    Channel(pixel, masks[0]) << 16 | Channel(pixel, masks[1]) << 8 | Channel(pixel, masks[2]);
            }
            destination[column] = color;
        }
    }
    return result;
}

void CopyImage(HWND owner, BitmapView image) {
    if (!owner) { throw std::invalid_argument("Clipboard publishing requires an owner window"); }
    const auto pngFormat = RegisterClipboardFormatW(L"PNG");
    // All allocations/encoding precede EmptyClipboard, retaining old contents
    // if preparation fails. Successful SetClipboardData transfers ownership.
    Memory dib(DibSize(image));
    {
        const LockedMemory pixels(dib.handle);
        WriteDib(pixels.data, image);
    }
    std::optional<Memory> png;
    if (pngFormat) {
        try { png.emplace(EncodeClipboardPng(image)); }
        catch (const std::exception&) { /* DIB remains available when PNG preparation fails. */ }
    }
    ClipboardScope clipboard(owner);
    if (!EmptyClipboard()) { throw std::runtime_error("Empty clipboard failed"); }
    const bool dibPublished = dib.Publish(CF_DIB);
    const bool pngPublished = png && png->Publish(pngFormat);
    if (!dibPublished && !pngPublished) { throw std::runtime_error("Publish clipboard image failed"); }
}

std::optional<Bitmap> ReadClipboardImage(HWND owner) {
    std::optional<DWORD> sequence;
    std::exception_ptr formatFailure;
    const auto png = RegisterClipboardFormatW(L"PNG");
    const auto checkSequence = [&] {
        if (sequence && GetClipboardSequenceNumber() != *sequence) {
            throw std::runtime_error("Clipboard changed while reading the image; please try again");
        }
    };
    for (const auto format : std::array<UINT, 3>{png, CF_DIBV5, CF_DIB}) {
        if (!format) { continue; }
        std::vector<std::uint8_t> encoded;
        {
            const ClipboardScope clipboard(owner);
            checkSequence();
            try {
                if (IsClipboardFormatAvailable(format)) { encoded = ReadBytes(format); }
            } catch (const std::bad_alloc&) { throw; }
            catch (const std::exception&) { if (!formatFailure) { formatFailure = std::current_exception(); } }
            sequence = GetClipboardSequenceNumber();
        }
        if (encoded.empty()) { continue; }
        try { return format == png ? DecodeImage(encoded) : DecodeDib(encoded); }
        catch (const std::bad_alloc&) { throw; }
        catch (const std::exception&) { if (!formatFailure) { formatFailure = std::current_exception(); } }
    }
    std::filesystem::path file;
    {
        ClipboardScope clipboard(owner);
        checkSequence();
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            const auto length = drop ? DragQueryFileW(drop, 0, nullptr, 0) : 0;
            if (length && length < 32768) {
                std::wstring path(length + 1, L'\0');
                if (DragQueryFileW(drop, 0, path.data(), length + 1) == length) { path.resize(length); file = path; }
            }
        }
    }
    if (!file.empty()) { return ReadImage(file); }
    if (formatFailure) { std::rethrow_exception(formatFailure); }
    return std::nullopt;
}
} // namespace vispick
