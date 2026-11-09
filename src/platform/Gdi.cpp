#include "platform/Gdi.h"

#include <limits>
#include <stdexcept>
#include <system_error>

namespace vispick {
namespace {

[[noreturn]] void ThrowWin32(const char* operation) {
    const DWORD error = GetLastError();
    throw std::system_error(static_cast<int>(error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE),
                            std::system_category(), operation);
}

int ValidateDimension(int dimension) {
    if (dimension <= 0) { throw std::invalid_argument("DIB dimensions must be positive"); }
    return dimension;
}

HBITMAP CreateBitmap(int width, int height, std::uint32_t*& pixels) {
    const auto pixelWidth = static_cast<std::size_t>(width);
    const auto pixelHeight = static_cast<std::size_t>(height);
    // GDI's image-size metadata is a DWORD even in a 64-bit process.
    if (pixelWidth > (std::numeric_limits<DWORD>::max() / sizeof(std::uint32_t)) / pixelHeight) {
        throw std::length_error("DIB exceeds the GDI image-size limit");
    }
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    void* storage = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (bitmap == nullptr) { ThrowWin32("CreateDIBSection"); }
    pixels = static_cast<std::uint32_t*>(storage);
    return bitmap;
}

} // namespace

ScreenDc::ScreenDc(HWND window) : window_(window), deviceContext_(GetDC(window)) {
    if (deviceContext_ == nullptr) { ThrowWin32("GetDC"); }
}

ScreenDc::~ScreenDc() { ReleaseDC(window_, deviceContext_); }

MemoryDc::MemoryDc(HDC compatibleWith) : deviceContext_(CreateCompatibleDC(compatibleWith)) {
    if (deviceContext_ == nullptr) { ThrowWin32("CreateCompatibleDC"); }
}

MemoryDc::~MemoryDc() { DeleteDC(deviceContext_); }

SelectObjectGuard::SelectObjectGuard(HDC deviceContext, HGDIOBJ object)
    : deviceContext_(deviceContext), previousObject_(SelectObject(deviceContext, object)) {
    if (previousObject_ == nullptr || previousObject_ == HGDI_ERROR) { ThrowWin32("SelectObject"); }
}

SelectObjectGuard::~SelectObjectGuard() { SelectObject(deviceContext_, previousObject_); }

DibSection::DibSection(int width, int height)
    : width_(ValidateDimension(width)), height_(ValidateDimension(height)),
      bitmap_(CreateBitmap(width_, height_, pixels_)),
      selectedBitmap_(deviceContext_.Get(), bitmap_.Get()) {}

std::span<std::uint32_t> DibSection::Pixels() noexcept {
    return {pixels_, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)};
}

std::span<const std::uint32_t> DibSection::Pixels() const noexcept {
    return {pixels_, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)};
}

} // namespace vispick
