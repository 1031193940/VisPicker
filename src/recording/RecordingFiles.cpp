#include "recording/RecordingFiles.h"
#include "platform/FileCommit.h"

#include <Windows.h>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace vispick::recording {
namespace {

class File final {
public:
    explicit File(HANDLE handle) noexcept : handle_(handle) {}
    ~File() { Close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
    }
private:
    HANDLE handle_;
};

class StagingFile final {
public:
    explicit StagingFile(const std::filesystem::path& destination)
        : path(UniqueSiblingPath(destination,L".tmp")), file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)) {
        if (file.Get() == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Create GIF save staging file");
        }
    }
    ~StagingFile() { file.Close(); DeleteFileW(path.c_str()); }
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    std::filesystem::path path;
    File file;
};

} // namespace

void CommitFile(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
    const auto sourcePath = ExtendedFilePath(temporary);
    const auto targetPath = ExtendedFilePath(destination);
    std::error_code equivalentError;
    if (std::filesystem::equivalent(sourcePath,targetPath,equivalentError)) {
        throw std::invalid_argument("Choose a destination outside the original temporary recording file");
    }
    if (GetFileAttributesW(targetPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto inspectError = GetLastError();
        if (inspectError != ERROR_FILE_NOT_FOUND) {
            throw std::system_error(static_cast<int>(inspectError),std::system_category(),"Inspect GIF save destination");
        }
        if (MoveFileExW(sourcePath.c_str(),targetPath.c_str(),MOVEFILE_WRITE_THROUGH)) { return; }
        const DWORD moveError = GetLastError();
        if (moveError != ERROR_NOT_SAME_DEVICE) {
            throw std::system_error(static_cast<int>(moveError),std::system_category(),"Move GIF to destination");
        }
    }
    // Overwrites and cross-volume saves retain the original recording until
    // the destination has committed. Keep the stage open through its flush.
    StagingFile staging(targetPath);
    {
        const File source(CreateFileW(sourcePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        if (source.Get() == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Open temporary GIF");
        }
        std::vector<std::uint8_t> buffer(64 * 1024);
        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(source.Get(),buffer.data(),static_cast<DWORD>(buffer.size()),&bytesRead,nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Read temporary GIF");
            }
            if (bytesRead == 0) { break; }
            DWORD bytesWritten = 0;
            if (!WriteFile(staging.file.Get(),buffer.data(),bytesRead,&bytesWritten,nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Write staged GIF");
            }
            if (bytesWritten != bytesRead) { throw std::runtime_error("Could not write the entire staged GIF"); }
        }
    }
    if (!FlushFileBuffers(staging.file.Get())) {
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Flush staged GIF");
    }
    staging.file.Close();
    CommitStagedFile(staging.path,targetPath);
    // The saved destination is committed. A locked temporary copy can be cleaned
    // by the next startup; never report a committed save as a failed copy.
    static_cast<void>(DeleteFileW(sourcePath.c_str()));
}

void CleanOldFiles(const std::filesystem::path& directory, std::filesystem::file_time_type cutoff) {
    std::error_code error;
    auto current = std::filesystem::directory_iterator(directory,error);
    const std::filesystem::directory_iterator end;
    while (!error && current != end) {
        const auto& entry = *current;
        const auto name = entry.path().filename().wstring();
        bool valid = name.starts_with(L"rec_vispick_") && name.ends_with(L".gif");
        if (valid) {
            const auto identity = std::wstring_view(name).substr(12,name.size()-16);
            unsigned separators = 0;
            unsigned digits = 0;
            for (const auto character : identity) {
                if (character == L'_') { valid = valid && digits != 0; ++separators; digits = 0; }
                else if (character >= L'0' && character <= L'9') { ++digits; }
                else { valid = false; }
            }
            valid = valid && separators == 2 && digits != 0;
        }
        const DWORD attributes = valid ? GetFileAttributesW(entry.path().c_str()) : INVALID_FILE_ATTRIBUTES;
        if (valid && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0) {
            const auto written = entry.last_write_time(error);
            if (!error && written < cutoff) { std::filesystem::remove(entry.path(),error); }
            error.clear(); // One locked/unreadable old file must not prevent startup.
        }
        current.increment(error);
    }
}

} // namespace vispick::recording
