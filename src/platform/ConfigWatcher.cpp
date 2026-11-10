#include "platform/ConfigWatcher.h"

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace vispick {
namespace {

class WatchHandle {
public:
    ~WatchHandle() { Reset(); }
    WatchHandle() = default;
    WatchHandle(const WatchHandle&) = delete;
    WatchHandle& operator=(const WatchHandle&) = delete;
    void Reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); }
        handle_ = replacement;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

[[noreturn]] void WatchError(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

} // namespace

struct ConfigWatcher::State {
    std::filesystem::path path;
    std::wstring fileName;
    HWND window = nullptr;
    UINT message = 0;
    WatchHandle directory;
    WatchHandle stop;
    WatchHandle completed;
    OVERLAPPED operation{};
    alignas(DWORD) std::array<std::byte, 8192> buffer{};
    std::thread worker;

    [[nodiscard]] bool BeginRead() noexcept {
        ResetEvent(completed.Get());
        operation = {};
        operation.hEvent = completed.Get();
        const BOOL started = ReadDirectoryChangesW(
            directory.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &operation,
            nullptr);
        return started != FALSE || GetLastError() == ERROR_IO_PENDING;
    }

    void CancelRead() noexcept {
        static_cast<void>(CancelIoEx(directory.Get(), &operation));
        DWORD ignored = 0;
        // CancelIoEx only requests cancellation. Keep OVERLAPPED and buffer alive until completion.
        static_cast<void>(GetOverlappedResult(directory.Get(), &operation, &ignored, TRUE));
    }

    [[nodiscard]] bool RelevantChange(DWORD length) const noexcept {
        if (length == 0) { return true; } // Buffer overflow: UI must reload the known filename.
        std::size_t offset = 0;
        constexpr auto headerBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        while (offset < length) {
            if (length - offset < headerBytes) { return true; }
            const auto* change = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            if (change->FileNameLength % sizeof(wchar_t) != 0 ||
                change->FileNameLength > length - offset - headerBytes) {
                return true;
            }
            const auto nameLength = change->FileNameLength / sizeof(wchar_t);
            if (nameLength == fileName.size() &&
                CompareStringOrdinal(change->FileName, static_cast<int>(nameLength), fileName.c_str(),
                                     static_cast<int>(fileName.size()), TRUE) == CSTR_EQUAL) {
                return true;
            }
            if (change->NextEntryOffset == 0) { break; }
            if (change->NextEntryOffset < headerBytes || change->NextEntryOffset > length - offset ||
                change->NextEntryOffset % alignof(DWORD) != 0) {
                return true;
            }
            offset += change->NextEntryOffset;
        }
        return false;
    }

    void Run() noexcept {
        const HANDLE events[] = {stop.Get(), completed.Get()};
        for (;;) {
            const DWORD waited = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (waited != WAIT_OBJECT_0 + 1) {
                const DWORD error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                CancelRead();
                if (error != ERROR_SUCCESS) { PostMessageW(window, message, error, 0); }
                return;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(directory.Get(), &operation, &bytes, FALSE)) {
                const DWORD error = GetLastError();
                if (error == ERROR_NOTIFY_ENUM_DIR) {
                    // The change list was lost; only one known config file needs to be reread.
                    if (!BeginRead()) {
                        PostMessageW(window, message, GetLastError(), 0);
                        return;
                    }
                    PostMessageW(window, message, 0, 0);
                    continue;
                }
                if (error != ERROR_OPERATION_ABORTED) { PostMessageW(window, message, error, 0); }
                return;
            }
            const bool changed = RelevantChange(bytes);
            // Re-arm before notifying: an atomic replace can generate several adjacent changes.
            if (!BeginRead()) {
                PostMessageW(window, message, GetLastError(), 0);
                return;
            }
            if (changed) { PostMessageW(window, message, 0, 0); }
        }
    }
};

ConfigWatcher::ConfigWatcher(std::filesystem::path path, HWND notificationWindow, UINT notificationMessage)
    : state_(std::make_unique<State>()) {
    if (path.empty() || notificationWindow == nullptr || notificationMessage < WM_APP || notificationMessage > 0xBFFF) {
        throw std::invalid_argument("Config watcher requires a path, window, and WM_APP message");
    }
    state_->path = std::filesystem::absolute(path).lexically_normal();
    state_->fileName = state_->path.filename().native();
    if (state_->fileName.empty() ||
        state_->fileName.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Invalid configuration filename");
    }
    state_->window = notificationWindow;
    state_->message = notificationMessage;
}

ConfigWatcher::~ConfigWatcher() {
    Stop();
}

void ConfigWatcher::Start() {
    if (state_->worker.joinable()) { return; }
    state_->directory.Reset(CreateFileW(state_->path.parent_path().c_str(), FILE_LIST_DIRECTORY,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    if (state_->directory.Get() == INVALID_HANDLE_VALUE) { WatchError("Open configuration directory"); }
    state_->stop.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (state_->stop.Get() == nullptr) { WatchError("Create configuration stop event"); }
    state_->completed.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (state_->completed.Get() == nullptr) { WatchError("Create configuration completion event"); }
    if (!state_->BeginRead()) { WatchError("ReadDirectoryChangesW"); }
    try {
        state_->worker = std::thread([state = state_.get()] { state->Run(); });
    } catch (...) {
        state_->CancelRead();
        throw;
    }
}

void ConfigWatcher::Stop() noexcept {
    if (state_->worker.joinable()) {
        SetEvent(state_->stop.Get());
        state_->worker.join();
    }
    state_->directory.Reset();
    state_->completed.Reset();
    state_->stop.Reset();
}

} // namespace vispick
