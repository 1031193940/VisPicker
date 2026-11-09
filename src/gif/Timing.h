#pragma once

#include <cstdint>

namespace vispick::gif {

// Consumes monotonically increasing capture-completion timestamps, in ns.
// Integer residuals preserve elapsed time to within one GIF centisecond over
// any number of irregular or dropped frames. A sub-centisecond interval can
// return zero: callers should coalesce such frames, never silently clamp it.
class TimestampDelayAccumulator {
public:
    explicit TimestampDelayAccumulator(std::int64_t firstTimestampNanoseconds) noexcept;
    [[nodiscard]] std::uint64_t AdvanceTo(std::int64_t nextTimestampNanoseconds);
    [[nodiscard]] std::uint64_t TotalCentiseconds() const noexcept { return totalCentiseconds_; }
    [[nodiscard]] std::int64_t RemainderNanoseconds() const noexcept { return remainderNanoseconds_; }

private:
    std::int64_t previousTimestampNanoseconds_;
    std::int64_t remainderNanoseconds_ = 0;
    std::uint64_t totalCentiseconds_ = 0;
};

} // namespace vispick::gif
