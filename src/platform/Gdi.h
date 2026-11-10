#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <utility>

namespace vispick {

template <typename Handle> class UniqueGdiObject {
public:
    UniqueGdiObject() noexcept = default;
    explicit UniqueGdiObject(Handle handle) noexcept : handle_(handle) {}
    ~UniqueGdiObject() { Reset(); }
    UniqueGdiObject(const UniqueGdiObject&) = delete;
    UniqueGdiObject& operator=(const UniqueGdiObject&) = delete;
    UniqueGdiObject(UniqueGdiObject&& other) noexcept : handle_(other.Release()) {}
    UniqueGdiObject& operator=(UniqueGdiObject&& other) noexcept {
        if (this != &other) { Reset(other.Release()); }
        return *this;
    }
    [[nodiscard]] Handle Get() const noexcept { return handle_; }
    [[nodiscard]] Handle Release() noexcept { return std::exchange(handle_, nullptr); }
    void Reset(Handle handle = nullptr) noexcept {
        if (handle_ != nullptr) { DeleteObject(handle_); }
        handle_ = handle;
    }

private:
    Handle handle_ = nullptr;
};

class ScreenDc {
public:
    explicit ScreenDc(HWND window = nullptr);
    ~ScreenDc();
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;
    [[nodiscard]] HDC Get() const noexcept { return deviceContext_; }

private:
    HWND window_ = nullptr;
    HDC deviceContext_ = nullptr;
};

class MemoryDc {
public:
    explicit MemoryDc(HDC compatibleWith = nullptr);
    ~MemoryDc();
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;
    [[nodiscard]] HDC Get() const noexcept { return deviceContext_; }

private:
    HDC deviceContext_ = nullptr;
};

// Restore before destroying either the selected object or its DC.
class SelectObjectGuard {
public:
    SelectObjectGuard(HDC deviceContext, HGDIOBJ object);
    ~SelectObjectGuard();
    SelectObjectGuard(const SelectObjectGuard&) = delete;
    SelectObjectGuard& operator=(const SelectObjectGuard&) = delete;

private:
    HDC deviceContext_ = nullptr;
    HGDIOBJ previousObject_ = nullptr;
};

class DibSection {
public:
    DibSection(int width, int height);
    DibSection(const DibSection&) = delete;
    DibSection& operator=(const DibSection&) = delete;
    [[nodiscard]] HDC DeviceContext() const noexcept { return deviceContext_.Get(); }
    [[nodiscard]] HBITMAP Handle() const noexcept { return bitmap_.Get(); }
    [[nodiscard]] int Width() const noexcept { return width_; }
    [[nodiscard]] int Height() const noexcept { return height_; }
    [[nodiscard]] std::span<std::uint32_t> Pixels() noexcept;
    [[nodiscard]] std::span<const std::uint32_t> Pixels() const noexcept;

private:
    int width_;
    int height_;
    MemoryDc deviceContext_;
    std::uint32_t* pixels_ = nullptr;
    UniqueGdiObject<HBITMAP> bitmap_;
    SelectObjectGuard selectedBitmap_;
};

} // namespace vispick
