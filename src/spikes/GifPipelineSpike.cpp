#include "spikes/Spikes.h"

#include "capture/ScreenGrab.h"
#include "gif/GifWriter.h"
#include "gif/Timing.h"
#include "platform/Compat.h"
#include "spikes/PatternProcess.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace vispick {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t QueueCapacity = 8;
constexpr int FramesPerSecond = 30;

double Milliseconds(const Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

class TemporaryGif {
public:
    explicit TemporaryGif(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryGif() {
        if (!keep_) { std::error_code ignored; std::filesystem::remove(path_, ignored); }
    }
    TemporaryGif(const TemporaryGif&) = delete;
    TemporaryGif& operator=(const TemporaryGif&) = delete;
    void Keep() noexcept { keep_ = true; }
private:
    std::filesystem::path path_;
    bool keep_ = false;
};

enum class SlotState { Free, Writing, Queued, Encoding };
struct CapturedSlot {
    Bitmap image{GifPatternWidth, GifPatternHeight};
    std::int64_t completionNanoseconds = 0;
    std::int64_t captureBeginQpc = 0;
    std::int64_t captureEndQpc = 0;
    PatternSnapshot sourceBefore;
    PatternSnapshot sourceAfter;
    SlotState state = SlotState::Free;
};

struct EncodingStats {
    std::uint64_t validatedFrames = 0;
    std::uint64_t rejectedFrames = 0;
    std::uint64_t changedSnapshotBrackets = 0;
    std::uint64_t unavailableBrackets = 0;
    std::uint64_t outsideSourceBracket = 0;
    std::uint64_t duplicateSourceFrames = 0;
    std::uint64_t skippedSourceIds = 0;
    std::uint64_t zeroDelayCoalesced = 0;
    std::int64_t firstCompletionNanoseconds = 0;
    std::int64_t finalStopNanoseconds = 0;
    std::vector<std::uint64_t> writtenHashes;
    std::vector<double> verificationTimes;
    std::vector<double> encodeTimes;
    std::vector<double> writeTimes;
    std::size_t maximumBusySlots = 0;
};

class EncoderWorker {
public:
    explicit EncoderWorker(const std::filesystem::path& output) : output_(output), thread_([this] { Run(); }) {}
    ~EncoderWorker() { StopAndJoin(0); }
    EncoderWorker(const EncoderWorker&) = delete;
    EncoderWorker& operator=(const EncoderWorker&) = delete;

    std::optional<std::size_t> Acquire() {
        std::lock_guard lock(mutex_);
        if (failure_) { std::rethrow_exception(failure_); }
        for (std::size_t slotIndex = 0; slotIndex < slots_.size(); ++slotIndex) {
            if (slots_[slotIndex].state == SlotState::Free) {
                slots_[slotIndex].state = SlotState::Writing;
                ++busySlots_;
                maximumBusySlots_ = std::max(maximumBusySlots_, busySlots_);
                return slotIndex;
            }
        }
        return std::nullopt;
    }

    CapturedSlot& Slot(const std::size_t index) { return slots_.at(index); }

    void Submit(const std::size_t index) {
        {
            std::lock_guard lock(mutex_);
            slots_.at(index).state = SlotState::Queued;
            queue_.push_back(index);
        }
        condition_.notify_one();
    }

    const EncodingStats& Finish(const std::int64_t stopNanoseconds) {
        StopAndJoin(stopNanoseconds);
        if (failure_) { std::rethrow_exception(failure_); }
        statistics_.maximumBusySlots = maximumBusySlots_;
        return statistics_;
    }

private:
    void StopAndJoin(const std::int64_t stopNanoseconds) noexcept {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
            if (stopNanoseconds > 0) { stopNanoseconds_ = stopNanoseconds; }
        }
        condition_.notify_one();
        if (thread_.joinable()) { thread_.join(); }
    }

    void Run() noexcept {
        try {
            gif::GifWriter writer(output_, GifPatternWidth, GifPatternHeight);
            gif::FrameEncoder encoder;
            std::optional<gif::EncodedFrame> pending;
            std::optional<gif::TimestampDelayAccumulator> delays;
            std::optional<std::uint32_t> previousSourceId;
            std::uint64_t pendingHash = 0;
            const auto writePending = [&](const std::uint64_t delay) {
                if (delay == 0) { ++statistics_.zeroDelayCoalesced; return; }
                if (delay > UINT16_MAX) { throw std::runtime_error("GIF frame delay exceeds format limit"); }
                const auto begin = Clock::now();
                writer.WriteFrame(*pending, static_cast<std::uint16_t>(delay));
                statistics_.writeTimes.push_back(Milliseconds(Clock::now() - begin));
                statistics_.writtenHashes.push_back(pendingHash);
            };
            while (true) {
                std::size_t slotIndex = 0;
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, [&] { return done_ || !queue_.empty(); });
                    if (queue_.empty()) { break; }
                    slotIndex = queue_.front();
                    queue_.pop_front();
                    slots_[slotIndex].state = SlotState::Encoding;
                }
                auto& slot = slots_[slotIndex];
                const auto verificationStart = Clock::now();
                const auto frameId = ValidateGifPattern(slot.image.Pixels());
                const auto& before = slot.sourceBefore;
                const auto& after = slot.sourceAfter;
                if (!before.valid || !after.valid) { ++statistics_.unavailableBrackets; }
                else if (before.frameNumber != after.frameNumber ||
                         before.updateBeginQpc != after.updateBeginQpc ||
                         before.updateEndQpc != after.updateEndQpc) {
                    ++statistics_.changedSnapshotBrackets;
                }
                if (!frameId) {
                    ++statistics_.rejectedFrames;
                    // A torn/occluded/unrecognized capture is never passed to the GIF writer.
                    // Metadata cannot distinguish those causes, so report rejection only.
                } else {
                    ++statistics_.validatedFrames;
                    if (before.valid && after.valid && (*frameId < before.frameNumber || *frameId > after.frameNumber)) {
                        ++statistics_.outsideSourceBracket;
                    }
                    if (previousSourceId) {
                        if (*frameId == *previousSourceId) { ++statistics_.duplicateSourceFrames; }
                        else if (*frameId > *previousSourceId + 1U) { statistics_.skippedSourceIds += *frameId - *previousSourceId - 1U; }
                    }
                    previousSourceId = frameId;
                    const auto hash = gif::PixelHash(slot.image.Pixels());
                    statistics_.verificationTimes.push_back(Milliseconds(Clock::now() - verificationStart));
                    if (!delays) {
                        delays.emplace(slot.completionNanoseconds);
                        statistics_.firstCompletionNanoseconds = slot.completionNanoseconds;
                    } else { writePending(delays->AdvanceTo(slot.completionNanoseconds)); }
                    const auto encodingStart = Clock::now();
                    pending = encoder.Encode(slot.image.Pixels());
                    statistics_.encodeTimes.push_back(Milliseconds(Clock::now() - encodingStart));
                    pendingHash = hash;
                }
                {
                    std::lock_guard lock(mutex_);
                    slot.state = SlotState::Free;
                    --busySlots_;
                }
            }
            if (!pending || !delays || stopNanoseconds_ == 0) { throw std::runtime_error("GIF pipeline has no completed verified recording"); }
            statistics_.finalStopNanoseconds = stopNanoseconds_;
            writePending(delays->AdvanceTo(stopNanoseconds_));
            writer.Finish();
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
        }
    }

    std::filesystem::path output_;
    std::array<CapturedSlot, QueueCapacity> slots_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::size_t> queue_;
    bool done_ = false;
    std::int64_t stopNanoseconds_ = 0;
    std::size_t busySlots_ = 0;
    std::size_t maximumBusySlots_ = 0;
    std::exception_ptr failure_;
    EncodingStats statistics_;
    std::thread thread_;
};

void AddTiming(SpikeResult& result, const std::string& prefix, std::vector<double> values) {
    if (values.empty()) { return; }
    std::sort(values.begin(), values.end());
    result.measurements.push_back({prefix + "_mean", std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()), "ms"});
    result.measurements.push_back({prefix + "_p95", values[static_cast<std::size_t>(0.95 * static_cast<double>(values.size() - 1))], "ms"});
    result.measurements.push_back({prefix + "_max", values.back(), "ms"});
}

} // namespace

SpikeResult RunGifPipelineSpike(const SpikeOptions& options) {
    if (options.seconds < 1 || options.seconds > 60) { throw std::invalid_argument("GIF pipeline duration must be 1..60 seconds"); }
    SpikeResult result{"gif-pipeline", false, {}, {}};
    const auto monitors = EnumerateMonitors();
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& information) {
        return information.bounds.right - information.bounds.left >= GifPatternWidth &&
               information.bounds.bottom - information.bounds.top >= GifPatternHeight;
    });
    if (monitor == monitors.end()) { throw std::runtime_error("No monitor fits the 1280x720 pipeline pattern"); }
    const LONG left = monitor->bounds.left + (monitor->bounds.right - monitor->bounds.left - GifPatternWidth) / 2;
    const LONG top = monitor->bounds.top + (monitor->bounds.bottom - monitor->bounds.top - GifPatternHeight) / 2;
    const RECT bounds{left, top, left + GifPatternWidth, top + GifPatternHeight};
    const auto output = options.outputDirectory / L"gif-pipeline.gif";
    TemporaryGif artifact(output);
    EncoderWorker encoder(output);
    PatternProcess pattern(bounds, options.seconds);
    CaptureSession capture(bounds);
    MessageTimer timer;
    // Give the separately scheduled window time to become composited; no DwmFlush
    // or source rendering occurs in the measured acquisition loop.
    if (!timer.WaitUntil(Clock::now() + std::chrono::milliseconds(150))) {
        throw std::runtime_error("GIF pipeline cancelled during pattern startup");
    }
    const auto start = Clock::now();
    const auto finish = start + std::chrono::seconds(options.seconds);
    const int plannedTicks = options.seconds * FramesPerSecond;
    int tick = 0;
    int scheduleSkipped = 0;
    int queueSkipped = 0;
    int capturedFrames = 0;
    std::vector<double> captureTimes;
    std::vector<double> copyTimes;
    std::vector<double> latenessTimes;
    while (tick < plannedTicks) {
        const auto deadline = start + std::chrono::nanoseconds(static_cast<std::int64_t>(tick) * 1000000000LL / FramesPerSecond);
        if (!timer.WaitUntil(deadline)) {
            throw std::runtime_error("GIF pipeline cancelled during sampling");
        }
        if (!pattern.Running()) { throw std::runtime_error("Independent pattern process exited during recording"); }
        const auto wake = Clock::now();
        if (wake >= finish) { scheduleSkipped += plannedTicks - tick; break; }
        const auto elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(wake - start).count();
        const int currentTick = std::min(plannedTicks - 1, static_cast<int>(elapsedNanoseconds * FramesPerSecond / 1000000000LL));
        if (currentTick > tick) { scheduleSkipped += currentTick - tick; tick = currentTick; }
        const auto actualDeadline = start + std::chrono::nanoseconds(static_cast<std::int64_t>(tick) * 1000000000LL / FramesPerSecond);
        latenessTimes.push_back(Milliseconds(wake - actualDeadline));
        const auto slotIndex = encoder.Acquire();
        if (!slotIndex) { ++queueSkipped; ++tick; continue; }
        auto& slot = encoder.Slot(*slotIndex);
        slot.sourceBefore = pattern.Snapshot();
        slot.captureBeginQpc = QpcNow();
        const auto captureBegin = Clock::now();
        capture.Capture(false);
        slot.captureEndQpc = QpcNow();
        slot.completionNanoseconds = QpcNanoseconds(slot.captureEndQpc);
        slot.sourceAfter = pattern.Snapshot();
        const auto captureEnd = Clock::now();
        captureTimes.push_back(Milliseconds(captureEnd - captureBegin));
        std::copy(capture.Image().Pixels().begin(), capture.Image().Pixels().end(), slot.image.Pixels().begin());
        copyTimes.push_back(Milliseconds(Clock::now() - captureEnd));
        encoder.Submit(*slotIndex);
        ++capturedFrames;
        ++tick;
    }
    if (!timer.WaitUntil(finish)) {
        throw std::runtime_error("GIF pipeline cancelled while waiting for recording stop");
    }
    const auto stopNanoseconds = QpcNanoseconds(QpcNow());
    const double wallSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    pattern.Stop();
    const auto drainBegin = Clock::now();
    const auto& statistics = encoder.Finish(stopNanoseconds);
    const double drainMilliseconds = Milliseconds(Clock::now() - drainBegin);
    const auto inspection = gif::InspectGif(output, statistics.writtenHashes);
    const double timelineSeconds = static_cast<double>(statistics.finalStopNanoseconds - statistics.firstCompletionNanoseconds) / 1000000000.0;
    const double gifSeconds = static_cast<double>(inspection.durationCentiseconds) / 100.0;
    const double durationError = timelineSeconds > 0 ? std::abs(gifSeconds - timelineSeconds) / timelineSeconds * 100.0 : 100.0;
    artifact.Keep();
    result.measurements = {
        {"width", GifPatternWidth, "px"}, {"height", GifPatternHeight, "px"},
        {"target_fps", FramesPerSecond, "fps"}, {"planned_ticks", static_cast<double>(plannedTicks), "ticks"},
        {"captured_frames", static_cast<double>(capturedFrames), "frames"},
        {"schedule_skipped_ticks", static_cast<double>(scheduleSkipped), "ticks"},
        {"queue_full_skipped_ticks", static_cast<double>(queueSkipped), "ticks"},
        {"verified_source_frames", static_cast<double>(statistics.validatedFrames), "frames"},
        {"rejected_unknown_or_torn_frames", static_cast<double>(statistics.rejectedFrames), "frames"},
        {"source_snapshot_bracket_changed", static_cast<double>(statistics.changedSnapshotBrackets), "frames"},
        {"unavailable_source_brackets", static_cast<double>(statistics.unavailableBrackets), "frames"},
        {"captured_id_outside_source_bracket", static_cast<double>(statistics.outsideSourceBracket), "frames"},
        {"duplicate_source_ids", static_cast<double>(statistics.duplicateSourceFrames), "frames"},
        {"skipped_source_ids", static_cast<double>(statistics.skippedSourceIds), "frames"},
        {"zero_delay_coalesced_frames", static_cast<double>(statistics.zeroDelayCoalesced), "frames"},
        {"decoded_frames", static_cast<double>(inspection.frameCount), "frames"},
        {"decoded_mismatched_frames", static_cast<double>(inspection.mismatchedFrames), "frames"},
        {"wall_duration", wallSeconds, "s"}, {"captured_timeline_duration", timelineSeconds, "s"},
        {"gif_duration", gifSeconds, "s"}, {"duration_error", durationError, "%"},
        {"queue_capacity", QueueCapacity, "frames"},
        {"maximum_busy_frame_slots", static_cast<double>(statistics.maximumBusySlots), "frames"},
        {"reserved_raw_queue_bytes", QueueCapacity * GifPatternWidth * GifPatternHeight * 4.0, "bytes"},
        {"encoder_drain", drainMilliseconds, "ms"},
        {"file_size", static_cast<double>(std::filesystem::file_size(output)), "bytes"}
    };
    AddTiming(result, "capture", std::move(captureTimes));
    AddTiming(result, "raw_queue_copy", std::move(copyTimes));
    AddTiming(result, "scheduler_lateness", std::move(latenessTimes));
    AddTiming(result, "source_verification_worker", statistics.verificationTimes);
    AddTiming(result, "encoding_worker", statistics.encodeTimes);
    AddTiming(result, "writing_worker", statistics.writeTimes);
    result.notes.emplace_back("Independent pattern process animates at 30fps. Acquisition only waits for its own timer, BitBlts actual desktop RGB, timestamps completion, and transfers ownership through eight preallocated raw frame slots to one encoding worker. No per-frame DwmFlush, pattern generation or pixel validation runs on the acquisition thread.");
    result.notes.emplace_back("Worker decodes the source frame ID from captured pixels and verifies every RGB pixel against that known pattern BEFORE writing. Unknown/torn/occluded frames are rejected without persisting their pixels. WIC validation runs offline after recording; no synthetic frames replace captures.");
    result.notes.emplace_back("Source QPC intervals describe GDI submission, not DWM presentation. source_snapshot_bracket_changed counts differences between the snapshots read before and after capture. Changed/unavailable brackets, duplicate IDs and IDs outside brackets report correlation ambiguity only: they cannot establish which source update was presented during capture, measure compositor latency, or identify an occlusion cause.");
    result.notes.emplace_back("GIF delays use actual capture-completion timestamps with carried 10ms remainder, from first verified completion through recording stop. Zero-delay samples are coalesced; rejected or queue-skipped intervals extend the previous verified frame. Leading acquisition latency is excluded from the captured timeline.");
    result.notes.emplace_back("This bounded one-worker prototype measures capture/encode separation. P5 still requires differential frames, worker-pool scheduling, cursor and recording UI. Performance acceptance additionally requires a local physical-display environment, enforced centrally by the report runner.");
    result.passed = scheduleSkipped == 0 && queueSkipped == 0 && statistics.rejectedFrames == 0 &&
        inspection.mismatchedFrames == 0 && inspection.frameCount == statistics.writtenHashes.size() && durationError < 1.0;
    return result;
}

} // namespace vispick
