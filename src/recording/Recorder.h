#pragma once

#include "core/Config.h"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vispick {

struct RecordingResult {
    std::filesystem::path path;
    int width = 0;
    int height = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t durationCentiseconds = 0;
    // Actual active-clock capture-completion and stop times, relative to Start.
    // GIF duration spans these endpoints, excluding pauses and initial latency.
    std::int64_t firstFrameCompletionNanoseconds = 0;
    std::int64_t stopNanoseconds = 0;
    std::uint64_t capturedFrames = 0;
    std::uint64_t droppedTicks = 0;
    std::uint64_t captureFailures = 0;
    std::uint64_t cursorFailures = 0;
    std::uint64_t unchangedTicks = 0;
    std::uint64_t coalescedTicks = 0;
    std::uint64_t peakQueuedFrames = 0;
    std::uint64_t peakInFlightFrames = 0;
    unsigned workerCount = 0;
    unsigned slotCount = 0;
    // Conservative bound for owned pipeline buffers/workspaces, including
    // capture DIBs, queued/in-flight frames and the writer's pending frame.
    // This is not an OS process-working-set measurement.
    std::uint64_t peakPipelineBytes = 0;
    bool captureLayeredWindows = false;
    bool captureInterrupted = false;
    bool cancelled = false;
    std::wstring error;
};

class Recorder final {
public:
    Recorder(RECT bounds, Config::Gif options, HWND completionOwner, UINT completionMessage, bool english = false);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    // UI-thread lifecycle. Start returns after scheduling work; Stop/Pause do
    // not wait. Destroy only after completion, including application shutdown.
    void Start();
    void Stop(bool cancel = false) noexcept;
    void Pause(bool paused) noexcept;
    [[nodiscard]] bool Finished() const noexcept;
    [[nodiscard]] std::optional<RecordingResult> TakeResult();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

// Only rec_<VisPick-owned identifier>.gif in TEMP/VisPick; no recursive cleanup.
void CleanOldRecordings();

} // namespace vispick
