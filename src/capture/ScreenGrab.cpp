#include "capture/ScreenGrab.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace vispick {
namespace {

int Extent(LONG start, LONG end) {
    const auto difference = static_cast<std::int64_t>(end) - start;
    if (difference <= 0 || difference > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Capture rectangle has invalid dimensions");
    }
    return static_cast<int>(difference);
}

void CopyDesktop(HDC destination, HDC source, const RECT& bounds, bool includeLayeredWindows) {
    const DWORD operation = SRCCOPY | (includeLayeredWindows ? CAPTUREBLT : 0UL);
    if (!BitBlt(destination, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                source, bounds.left, bounds.top, operation)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "BitBlt");
    }
}

void CompleteGdiWrites() {
    // CreateDIBSection requires synchronization before CPU reads of its storage.
    if (!GdiFlush()) { throw std::runtime_error("GdiFlush failed after screen capture"); }
}

void DrawCapturedCursor(const HDC destination, const RECT bounds) {
    CURSORINFO cursor{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&cursor)) { throw std::runtime_error("GetCursorInfo failed during capture"); }
    if ((cursor.flags & CURSOR_SHOWING) == 0 || !cursor.hCursor) { return; }
    ICONINFO information{};
    if (!GetIconInfo(cursor.hCursor, &information)) { throw std::runtime_error("GetIconInfo failed during capture"); }
    const UniqueGdiObject<HBITMAP> mask(information.hbmMask);
    const UniqueGdiObject<HBITMAP> color(information.hbmColor);
    if (!DrawIconEx(destination, cursor.ptScreenPos.x - bounds.left - static_cast<int>(information.xHotspot),
                    cursor.ptScreenPos.y - bounds.top - static_cast<int>(information.yHotspot),
                    cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL)) {
        throw std::runtime_error("DrawIconEx failed during capture");
    }
}

} // namespace

CaptureSession::CaptureSession(RECT bounds)
    : bounds_(bounds), captureSurface_(Extent(bounds.left, bounds.right), Extent(bounds.top, bounds.bottom)) {}

void CaptureSession::Capture(bool includeLayeredWindows) {
    CopyDesktop(captureSurface_.DeviceContext(), desktopContext_.Get(), bounds_, includeLayeredWindows);
    CompleteGdiWrites();
}

CaptureTimings CaptureSession::CaptureTimed(bool includeLayeredWindows) {
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    CopyDesktop(captureSurface_.DeviceContext(), desktopContext_.Get(), bounds_, includeLayeredWindows);
    const auto copied = Clock::now();
    CompleteGdiWrites();
    const auto completed = Clock::now();
    return {std::chrono::duration<double, std::milli>(copied - begin).count(),
            std::chrono::duration<double, std::milli>(completed - copied).count()};
}

BitmapView CaptureSession::Image() const {
    return BitmapView(captureSurface_.Width(), captureSurface_.Height(), captureSurface_.Pixels());
}

FrozenScreen::FrozenScreen(RECT bounds, bool includeLayeredWindows, bool includeCursor)
    : bounds_(bounds), surface_(Extent(bounds.left, bounds.right), Extent(bounds.top, bounds.bottom)) {
    const ScreenDc desktop;
    CopyDesktop(surface_.DeviceContext(), desktop.Get(), bounds_, includeLayeredWindows);
    if (includeCursor) { DrawCapturedCursor(surface_.DeviceContext(), bounds_); }
    CompleteGdiWrites();
}

BitmapView FrozenScreen::Image() const {
    return BitmapView(surface_.Width(), surface_.Height(), surface_.Pixels());
}

void FrozenScreen::BlitTo(HDC destination, RECT localSource, POINT destinationOrigin) const {
    if (localSource.left < 0 || localSource.top < 0 || localSource.right > surface_.Width() ||
        localSource.bottom > surface_.Height() || localSource.left >= localSource.right || localSource.top >= localSource.bottom) {
        throw std::out_of_range("Frozen screen source rectangle outside image");
    }
    if (!BitBlt(destination, destinationOrigin.x, destinationOrigin.y,
                localSource.right - localSource.left, localSource.bottom - localSource.top,
                surface_.DeviceContext(), localSource.left, localSource.top, SRCCOPY)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "FrozenScreen::BlitTo");
    }
}

std::shared_ptr<const FrozenScreen> CaptureScreen(RECT bounds, bool includeLayeredWindows, bool includeCursor) {
    return std::make_shared<const FrozenScreen>(bounds, includeLayeredWindows, includeCursor);
}

} // namespace vispick
