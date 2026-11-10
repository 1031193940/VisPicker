#pragma once

#include <Windows.h>
#include <objbase.h>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace vispick {

// Extended Win32 paths also work on Win7, independently of the long-path policy.
[[nodiscard]] inline std::filesystem::path ExtendedFilePath(const std::filesystem::path& path) {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos || path.native().starts_with(L"\\\\.\\")) {
        throw std::invalid_argument("Invalid image file path");
    }
    if (path.native().starts_with(L"\\\\?\\")) { return path; }
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    absolute.make_preferred();
    const auto& name = absolute.native();
    return name.starts_with(L"\\\\") ? std::filesystem::path(L"\\\\?\\UNC\\" + name.substr(2))
                                     : std::filesystem::path(L"\\\\?\\" + name);
}

[[nodiscard]] inline std::filesystem::path UniqueSiblingPath(const std::filesystem::path& destination,
                                                             const wchar_t* extension) {
    GUID identity{};
    if (FAILED(CoCreateGuid(&identity))) { throw std::runtime_error("Create a unique output filename failed"); }
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name = L".vispick-";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&identity);
    for (std::size_t byte = 0; byte < sizeof(identity); ++byte) {
        name += digits[bytes[byte] >> 4];
        name += digits[bytes[byte] & 15];
    }
    name += extension;
    return destination.parent_path() / name;
}

// Staging is a distinct, flushed sibling file owned by the caller. Only the
// destination is modified. A failed rollback leaves the original backup intact.
inline void CommitStagedFile(const std::filesystem::path& staging, const std::filesystem::path& destination) {
    const auto source = ExtendedFilePath(staging);
    const auto target = ExtendedFilePath(destination);
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            throw std::system_error(static_cast<int>(error), std::system_category(), "Inspect output destination");
        }
        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Commit new output file");
        }
        return;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("Cannot replace a directory or redirected output file");
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) { throw std::runtime_error("Cannot replace a read-only output file"); }

    constexpr DWORD retainedAttributes =
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if ((attributes & retainedAttributes) &&
        !SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_ARCHIVE | (attributes & retainedAttributes))) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Preserve output file attributes");
    }

    // ReplaceFile merges DACLs and named streams. Its documented partial-failure
    // states may rename the old destination, so always supply a backup path.
    const auto backup = UniqueSiblingPath(target, L".bak");
    if (ReplaceFileW(target.c_str(), source.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
        DeleteFileW(backup.c_str());
        return;
    }
    const DWORD error = GetLastError();
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto name = backup.u8string();
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Replace output; original retained at " +
                                    std::string(reinterpret_cast<const char*>(name.data()), name.size()));
    }
    throw std::system_error(static_cast<int>(error), std::system_category(), "Replace output file");
}

} // namespace vispick
