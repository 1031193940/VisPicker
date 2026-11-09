#pragma once

#include "imaging/Bitmap.h"

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace vispick {

constexpr int GifPatternWidth = 1280;
constexpr int GifPatternHeight = 720;

// Shared known source for both correctness and independently paced pipeline tests.
void FillGifPattern(Bitmap& image, std::uint32_t frameNumber);
[[nodiscard]] std::optional<std::uint32_t> ValidateGifPattern(std::span<const std::uint32_t> pixels);
[[nodiscard]] std::int64_t QpcNow();
[[nodiscard]] std::int64_t QpcNanoseconds(std::int64_t counter);

// A waitable timer pumps GUI messages; optional stop/process handles cancel waits.
class MessageTimer {
public:
    MessageTimer();
    ~MessageTimer();
    MessageTimer(const MessageTimer&) = delete;
    MessageTimer& operator=(const MessageTimer&) = delete;
    bool WaitUntil(std::chrono::steady_clock::time_point deadline,
                   std::span<const HANDLE> cancellationHandles = {});
private:
    HANDLE timer_ = nullptr;
    bool timerResolution_ = false;
};

struct PatternSnapshot {
    std::uint32_t frameNumber = 0;
    std::int64_t updateBeginQpc = 0;
    std::int64_t updateEndQpc = 0;
    bool valid = false;
};

class PatternProcess {
public:
    PatternProcess(RECT bounds, int seconds);
    ~PatternProcess();
    PatternProcess(const PatternProcess&) = delete;
    PatternProcess& operator=(const PatternProcess&) = delete;
    [[nodiscard]] PatternSnapshot Snapshot() const noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void Stop() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Private numeric IPC arguments, excluding the --pattern-service switch.
int RunPatternService(std::span<const std::wstring> arguments);

} // namespace vispick
