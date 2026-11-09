#pragma once

#include "imaging/Bitmap.h"
#include "platform/Gdi.h"

#include <memory>

namespace vispick {

struct CaptureTimings {
    double bitBltMilliseconds = 0;
    double gdiFlushMilliseconds = 0;
};

// One reusable DIB, without alpha normalization or a second full-size buffer.
// Image() borrows this storage until the next Capture or destruction. The caller
// must finish reading before another thread captures into the same session.
class CaptureSession {
public:
    explicit CaptureSession(RECT bounds);
    void Capture(bool includeLayeredWindows = true);
    [[nodiscard]] CaptureTimings CaptureTimed(bool includeLayeredWindows = true);
    [[nodiscard]] BitmapView Image() const;
    [[nodiscard]] const RECT& Bounds() const noexcept { return bounds_; }
private:
    RECT bounds_;
    ScreenDc desktopContext_;
    DibSection captureSurface_;
};

// Immutable shared snapshot, captured directly into the single DIB retained here.
// BlitTo exposes read access without handing callers a mutable source HDC.
class FrozenScreen {
public:
    FrozenScreen(RECT bounds, bool includeLayeredWindows, bool includeCursor = false);
    [[nodiscard]] BitmapView Image() const;
    [[nodiscard]] const RECT& Bounds() const noexcept { return bounds_; }
    void BlitTo(HDC destination, RECT localSource, POINT destinationOrigin) const;
private:
    RECT bounds_;
    DibSection surface_;
};

[[nodiscard]] std::shared_ptr<const FrozenScreen> CaptureScreen(RECT bounds, bool includeLayeredWindows = true, bool includeCursor = false);

} // namespace vispick
