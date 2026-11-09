#pragma once
#include <filesystem>

namespace vispick::recording {

enum class TemporaryGifKind { Recording, Clipboard };

class TemporaryGif final {
public:
    explicit TemporaryGif(TemporaryGifKind kind);
    ~TemporaryGif();
    TemporaryGif(const TemporaryGif&) = delete;
    TemporaryGif& operator=(const TemporaryGif&) = delete;
    std::filesystem::path path;
    bool keep = false;
};

// The caller has already obtained overwrite confirmation and closed decoders.
// A new file on the same volume uses a rename. Cross-volume and overwrite
// saves commit a flushed sibling stage so a failed copy preserves both files.
void CommitFile(const std::filesystem::path& temporary, const std::filesystem::path& destination);
void CleanOldFiles(const std::filesystem::path& directory, std::filesystem::file_time_type cutoff,
                   TemporaryGifKind kind = TemporaryGifKind::Recording,
                   const std::filesystem::path& preserved = {});

} // namespace vispick::recording
