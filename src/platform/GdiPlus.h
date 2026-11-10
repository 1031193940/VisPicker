#pragma once

#include <windows.h>
#include <objidl.h>
// SDK 10.0.22621's FontCollection wrappers contain bounds/SAL defects.
#pragma warning(push)
#pragma warning(disable: 6385 28252 28253)
#include <gdiplus.h>
#pragma warning(pop)

namespace vispick {

// One process lifetime for rendering and toolbar icons. Callers destroy their
// short-lived GDI+ objects before this function's static lifetime ends.
[[nodiscard]] inline bool EnsureGdiPlus() noexcept {
    class Lifetime {
    public:
        Lifetime() noexcept {
            const Gdiplus::GdiplusStartupInput input;
            available_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
        }
        ~Lifetime() { if (available_) { Gdiplus::GdiplusShutdown(token_); } }
        [[nodiscard]] bool Available() const noexcept { return available_; }
    private:
        ULONG_PTR token_ = 0;
        bool available_ = false;
    };
    static const Lifetime lifetime;
    return lifetime.Available();
}

} // namespace vispick
