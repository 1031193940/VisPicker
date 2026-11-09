#include "gif/Timing.h"

#include <stdexcept>

namespace vispick::gif {

TimestampDelayAccumulator::TimestampDelayAccumulator(std::int64_t firstTimestampNanoseconds) noexcept
    : previousTimestampNanoseconds_(firstTimestampNanoseconds) {}

std::uint64_t TimestampDelayAccumulator::AdvanceTo(std::int64_t nextTimestampNanoseconds) {
    if (nextTimestampNanoseconds < previousTimestampNanoseconds_) {
        throw std::invalid_argument("GIF capture timestamps must not move backwards");
    }
    constexpr std::uint64_t nanosecondsPerCentisecond = 10000000;
    // Unsigned subtraction also handles intervals crossing the signed epoch
    // without signed overflow. Split quotient/remainder before adding carry.
    const auto elapsed = static_cast<std::uint64_t>(nextTimestampNanoseconds) -
        static_cast<std::uint64_t>(previousTimestampNanoseconds_);
    const auto fraction = elapsed % nanosecondsPerCentisecond +
        static_cast<std::uint64_t>(remainderNanoseconds_);
    const auto delay = elapsed / nanosecondsPerCentisecond + fraction / nanosecondsPerCentisecond;
    remainderNanoseconds_ = static_cast<std::int64_t>(fraction % nanosecondsPerCentisecond);
    totalCentiseconds_ += delay;
    previousTimestampNanoseconds_ = nextTimestampNanoseconds;
    return delay;
}

} // namespace vispick::gif
