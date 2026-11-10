#pragma once

#include "core/Paths.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vispick::test {
namespace detail {

[[noreturn]] inline void GifWorkerError(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

struct GifWorkerHandleCloser {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

using GifWorkerHandle = std::unique_ptr<void, GifWorkerHandleCloser>;

struct GifWorkerRequest {
    std::uint64_t durationCentiseconds;
    unsigned frameCount;
    bool validated;
    char error[384];
    wchar_t path[32768];
};

struct GifWorkerViewCloser {
    void operator()(GifWorkerRequest* view) const noexcept { UnmapViewOfFile(view); }
};

using GifWorkerView = std::unique_ptr<GifWorkerRequest, GifWorkerViewCloser>;

class GifWorkerAttributes {
public:
    explicit GifWorkerAttributes(std::array<HANDLE, 5>& handles) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) { GifWorkerError("Size GIF worker handle list"); }
        storage_.resize(size);
        list_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            GifWorkerError("Initialize GIF worker handle list");
        }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(handles),
                                       nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list_);
            GifWorkerError("Restrict GIF worker inherited handles", error);
        }
    }

    ~GifWorkerAttributes() { DeleteProcThreadAttributeList(list_); }
    GifWorkerAttributes(const GifWorkerAttributes&) = delete;
    GifWorkerAttributes& operator=(const GifWorkerAttributes&) = delete;
    [[nodiscard]] PPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_; }

private:
    std::vector<std::byte> storage_;
    PPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

inline HANDLE StartGifWorker(std::array<HANDLE, 5>& handles) {
    const auto executable = ExecutablePath();
    std::wstring command = L"\"" + executable.native() + L"\" --gif-validation-worker";
    for (const HANDLE handle : handles) { command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle)); }
    GifWorkerAttributes attributes(handles);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                        &created)) {
        GifWorkerError("Start persistent GIF validation worker");
    }
    CloseHandle(created.hThread);
    return created.hProcess;
}

inline GifWorkerHandle ParseGifWorkerHandle(std::wstring_view argument) {
    std::uintptr_t value = 0;
    for (const wchar_t character : argument) {
        if (character < L'0' || character > L'9' ||
            value > ((std::numeric_limits<std::uintptr_t>::max)() - static_cast<unsigned>(character - L'0')) / 10) {
            throw std::invalid_argument("Invalid GIF worker inherited handle");
        }
        value = value * 10 + static_cast<unsigned>(character - L'0');
    }
    if (value == 0 || value == (std::numeric_limits<std::uintptr_t>::max)()) {
        throw std::invalid_argument("Invalid GIF worker inherited handle");
    }
    return GifWorkerHandle(reinterpret_cast<HANDLE>(value));
}

inline void CALLBACK StopOrphanedGifWorker(void*, BOOLEAN) noexcept {
    TerminateProcess(GetCurrentProcess(), ERROR_CANCELLED);
}

inline void SaveGifWorkerError(GifWorkerRequest& request, std::string_view error) noexcept {
    const auto length = std::min(error.size(), sizeof(request.error) - 1);
    std::copy_n(error.data(), length, request.error);
    request.error[length] = '\0';
}

} // namespace detail

// One request at a time on the soak thread. All IPC resources exist before warmup.
class GifValidationWorker {
public:
    GifValidationWorker() {
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        const detail::GifWorkerHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0,
                                                                 static_cast<DWORD>(sizeof(detail::GifWorkerRequest)),
                                                                 nullptr));
        const detail::GifWorkerHandle parent(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
        requestEvent_.reset(CreateEventW(&inheritable, FALSE, FALSE, nullptr));
        responseEvent_.reset(CreateEventW(&inheritable, FALSE, FALSE, nullptr));
        shutdownEvent_.reset(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
        if (!mapping || !parent || !requestEvent_ || !responseEvent_ || !shutdownEvent_) {
            throw std::runtime_error("Create persistent GIF validation IPC handles failed");
        }
        request_.reset(static_cast<detail::GifWorkerRequest*>(
            MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(detail::GifWorkerRequest))));
        if (!request_) { detail::GifWorkerError("Map GIF validation request"); }
        std::construct_at(request_.get());
        std::array handles{mapping.get(), requestEvent_.get(), responseEvent_.get(), shutdownEvent_.get(),
                           parent.get()};
        child_.reset(detail::StartGifWorker(handles));
        const HANDLE readyOrExit[] = {responseEvent_.get(), child_.get()};
        if (WaitForMultipleObjects(2, readyOrExit, FALSE, 10000) != WAIT_OBJECT_0) {
            Stop();
            throw std::runtime_error("Persistent GIF validation worker did not become ready");
        }
    }

    ~GifValidationWorker() { Stop(); }
    GifValidationWorker(const GifValidationWorker&) = delete;
    GifValidationWorker& operator=(const GifValidationWorker&) = delete;

    bool Validate(const std::filesystem::path& path, unsigned frameCount, std::uint64_t durationCentiseconds,
                  HANDLE stop) {
        const DWORD stopState = WaitForSingleObject(stop, 0);
        if (stopState == WAIT_OBJECT_0) {
            RequireStopped();
            return false;
        }
        if (stopState == WAIT_FAILED) { detail::GifWorkerError("Check GIF validation stop event"); }
        if (!child_) { throw std::logic_error("Persistent GIF validation worker is stopped"); }
        const auto& filename = path.native();
        if (!path.is_absolute() || !filename.ends_with(L".gif") || filename.size() >= std::size(request_->path) ||
            filename.find(L'\0') != std::wstring::npos) {
            throw std::invalid_argument("GIF validation requires an absolute GIF file path");
        }
        std::copy(filename.begin(), filename.end(), request_->path);
        request_->path[filename.size()] = L'\0';
        request_->frameCount = frameCount;
        request_->durationCentiseconds = durationCentiseconds;
        request_->validated = false;
        request_->error[0] = '\0';
        if (!SetEvent(requestEvent_.get())) {
            const DWORD error = GetLastError();
            RequireStopped();
            detail::GifWorkerError("Signal GIF validation request", error);
        }

        const HANDLE completed[] = {stop, responseEvent_.get(), child_.get()};
        const DWORD result = WaitForMultipleObjects(3, completed, FALSE, 15000);
        if (result == WAIT_OBJECT_0 + 1 && request_->validated) { return true; }
        const DWORD waitError = result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
        RequireStopped();
        if (result == WAIT_OBJECT_0) { return false; }
        if (result == WAIT_OBJECT_0 + 1) {
            throw std::runtime_error(std::string("GIF validation worker failed: ") + request_->error);
        }
        if (result == WAIT_OBJECT_0 + 2) {
            throw std::runtime_error("Persistent GIF validation worker exited during a request");
        }
        if (result == WAIT_TIMEOUT) {
            throw std::runtime_error("GIF validation worker timed out after fifteen seconds");
        }
        detail::GifWorkerError("Wait for GIF validation worker", waitError);
    }

private:
    bool Stop() noexcept {
        if (!child_) { return true; }
        SetEvent(shutdownEvent_.get());
        if (WaitForSingleObject(child_.get(), 1000) != WAIT_OBJECT_0) {
            TerminateProcess(child_.get(), ERROR_CANCELLED);
            if (WaitForSingleObject(child_.get(), 2000) != WAIT_OBJECT_0) { return false; }
        }
        child_.reset();
        return true;
    }

    void RequireStopped() {
        if (!Stop()) { throw std::runtime_error("Persistent GIF validation worker did not stop"); }
    }

    detail::GifWorkerHandle requestEvent_;
    detail::GifWorkerHandle responseEvent_;
    detail::GifWorkerHandle shutdownEvent_;
    detail::GifWorkerHandle child_;
    detail::GifWorkerView request_;
};

inline int ServeGifValidationRequests(int count, wchar_t** arguments, const auto& validateGif) {
    if (count != 7) { throw std::invalid_argument("Invalid GIF validation worker argument count"); }
    const auto mapping = detail::ParseGifWorkerHandle(arguments[2]);
    const auto requestEvent = detail::ParseGifWorkerHandle(arguments[3]);
    const auto responseEvent = detail::ParseGifWorkerHandle(arguments[4]);
    const auto shutdownEvent = detail::ParseGifWorkerHandle(arguments[5]);
    const auto parent = detail::ParseGifWorkerHandle(arguments[6]);
    const detail::GifWorkerView request(static_cast<detail::GifWorkerRequest*>(
        MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(detail::GifWorkerRequest))));
    if (!request) { detail::GifWorkerError("Map GIF worker request"); }
    HANDLE watchdog = nullptr;
    if (!RegisterWaitForSingleObject(&watchdog, parent.get(), detail::StopOrphanedGifWorker, nullptr, INFINITE,
                                     WT_EXECUTEONLYONCE)) {
        detail::GifWorkerError("Monitor GIF worker parent process");
    }

    int status = 1;
    if (SetEvent(responseEvent.get())) {
        const HANDLE pending[] = {shutdownEvent.get(), requestEvent.get()};
        for (;;) {
            const DWORD ready = WaitForMultipleObjects(2, pending, FALSE, INFINITE);
            if (ready != WAIT_OBJECT_0 + 1) {
                status = ready == WAIT_OBJECT_0 ? 0 : 1;
                break;
            }
            try {
                validateGif(std::filesystem::path(request->path), request->frameCount, request->durationCentiseconds);
                request->validated = true;
            } catch (const std::exception& error) { detail::SaveGifWorkerError(*request, error.what()); } catch (...) {
                detail::SaveGifWorkerError(*request, "Unexpected WIC error");
            }
            if (!SetEvent(responseEvent.get())) { break; }
        }
    }
    return UnregisterWaitEx(watchdog, INVALID_HANDLE_VALUE) ? status : 1;
}

} // namespace vispick::test
