#include "platform/SingleInstance.h"

#include <sddl.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace vispick {
namespace {

struct HandleDeleter {
    void operator()(void* handle) const noexcept { if (handle) { CloseHandle(handle); } }
};
struct LocalDeleter {
    void operator()(wchar_t* text) const noexcept { LocalFree(text); }
};

std::wstring CurrentUserSid() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) { throw std::runtime_error("OpenProcessToken failed"); }
    const std::unique_ptr<void, HandleDeleter> token(rawToken);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes == 0 || bytes > 65536) { throw std::runtime_error("Cannot size current user token"); }
    std::vector<std::byte> storage(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes)) { throw std::runtime_error("GetTokenInformation failed"); }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    wchar_t* rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid)) { throw std::runtime_error("ConvertSidToStringSidW failed"); }
    const std::unique_ptr<wchar_t, LocalDeleter> sid(rawSid);
    return sid.get();
}

struct WireHeader {
    std::uint32_t magic = static_cast<std::uint32_t>(SingleInstance::CopyDataTag);
    std::uint32_t version = 1;
    std::uint32_t command = 0;
    std::uint32_t flags = 0;
    std::uint32_t pinCodeUnits = 0;
    std::uint32_t configCodeUnits = 0;
};
static_assert(sizeof(WireHeader) == 24);
static_assert(sizeof(wchar_t) == 2);

bool ValidUtf16(const std::wstring_view text) noexcept {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character == L'\0') { return false; }
        if (character >= 0xD800 && character <= 0xDBFF) {
            if (++index == text.size() || text[index] < 0xDC00 || text[index] > 0xDFFF) { return false; }
        } else if (character >= 0xDC00 && character <= 0xDFFF) { return false; }
    }
    return true;
}

bool ValidOptions(const LaunchOptions& options) {
    if (static_cast<unsigned>(options.command) > static_cast<unsigned>(AppCommand::Exit)) { return false; }
    if (options.command == AppCommand::PinFile ? options.pinFile.empty() : !options.pinFile.empty()) { return false; }
    if (options.help && options.version) { return false; }
    return ValidUtf16(options.pinFile.native()) && ValidUtf16(options.configPath.native());
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    return path.empty() ? path : std::filesystem::absolute(path).lexically_normal();
}

} // namespace

SingleInstance::SingleInstance(std::wstring identity) {
    if (identity.empty() || identity.size() > 64 || !std::all_of(identity.begin(), identity.end(), [](const wchar_t character) {
        return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z') ||
               (character >= L'0' && character <= L'9') || character == L'.' || character == L'_' || character == L'-';
    })) { throw std::invalid_argument("Single-instance identity must contain 1..64 ASCII letters, digits, dots, underscores or hyphens"); }
    const std::wstring userIdentity = identity + L"." + CurrentUserSid();
    const std::wstring mutexName = L"Local\\" + userIdentity;
    windowTitle_ = L"VisPick.Main." + userIdentity;
    mutex_ = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex_) { throw std::runtime_error("Create single-instance mutex failed"); }
    primary_ = GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() { if (mutex_) { CloseHandle(mutex_); } }

std::vector<std::byte> SingleInstance::Encode(const LaunchOptions& options) {
    if (!ValidOptions(options)) { throw std::invalid_argument("Invalid launch options for single-instance forwarding"); }
    const auto& pin = options.pinFile.native();
    const auto& config = options.configPath.native();
    constexpr std::size_t maximumCodeUnits = (MaximumPayloadBytes - sizeof(WireHeader)) / sizeof(wchar_t);
    if (pin.size() > maximumCodeUnits || config.size() > maximumCodeUnits - pin.size()) {
        throw std::length_error("Single-instance payload exceeds 64 KiB");
    }
    WireHeader header;
    header.command = static_cast<std::uint32_t>(options.command);
    header.flags = (options.help ? 1U : 0U) | (options.version ? 2U : 0U) | (options.performanceLogging ? 4U : 0U);
    header.pinCodeUnits = static_cast<std::uint32_t>(pin.size());
    header.configCodeUnits = static_cast<std::uint32_t>(config.size());
    std::vector<std::byte> payload(sizeof(header) + (pin.size() + config.size()) * sizeof(wchar_t));
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), pin.data(), pin.size() * sizeof(wchar_t));
    std::memcpy(payload.data() + sizeof(header) + pin.size() * sizeof(wchar_t), config.data(), config.size() * sizeof(wchar_t));
    return payload;
}

std::optional<LaunchOptions> SingleInstance::Decode(const COPYDATASTRUCT& data) {
    if (data.dwData != CopyDataTag || !data.lpData || data.cbData < sizeof(WireHeader) || data.cbData > MaximumPayloadBytes) { return std::nullopt; }
    WireHeader header;
    std::memcpy(&header, data.lpData, sizeof(header));
    if (header.magic != CopyDataTag || header.version != 1 || (header.flags & ~7U) != 0 ||
        header.command > static_cast<std::uint32_t>(AppCommand::Exit)) { return std::nullopt; }
    const std::uint64_t codeUnits = static_cast<std::uint64_t>(header.pinCodeUnits) + header.configCodeUnits;
    if (sizeof(WireHeader) + codeUnits * sizeof(wchar_t) != data.cbData) { return std::nullopt; }
    const auto* bytes = static_cast<const std::byte*>(data.lpData) + sizeof(WireHeader);
    std::wstring pin(header.pinCodeUnits, L'\0');
    std::wstring config(header.configCodeUnits, L'\0');
    std::memcpy(pin.data(), bytes, pin.size() * sizeof(wchar_t));
    std::memcpy(config.data(), bytes + pin.size() * sizeof(wchar_t), config.size() * sizeof(wchar_t));
    LaunchOptions options;
    options.command = static_cast<AppCommand>(header.command);
    options.help = (header.flags & 1U) != 0;
    options.version = (header.flags & 2U) != 0;
    options.performanceLogging = (header.flags & 4U) != 0;
    options.pinFile = std::move(pin);
    options.configPath = std::move(config);
    if (!ValidOptions(options)) { return std::nullopt; }
    return options;
}

bool SingleInstance::ConfigMatches(const std::filesystem::path& requested, const std::filesystem::path& active) {
    if (!ValidUtf16(requested.native()) || !ValidUtf16(active.native())) { return false; }
    if (requested.empty()) { return true; }
    if (active.empty()) { return false; }
    const auto requestedPath = AbsolutePath(requested).native();
    const auto activePath = AbsolutePath(active).native();
    if (requestedPath.size() > INT_MAX || activePath.size() > INT_MAX) { return false; }
    return CompareStringOrdinal(requestedPath.data(), static_cast<int>(requestedPath.size()),
        activePath.data(), static_cast<int>(activePath.size()), TRUE) == CSTR_EQUAL;
}

ForwardResult SingleInstance::Forward(const LaunchOptions& launch, const DWORD timeoutMilliseconds) const {
    if (primary_) { return ForwardResult::Unavailable; }
    if (timeoutMilliseconds == 0 || timeoutMilliseconds > 30000) { throw std::invalid_argument("Forward timeout must be 1..30000 milliseconds"); }
    if (!ValidOptions(launch)) { throw std::invalid_argument("Invalid launch options for single-instance forwarding"); }
    LaunchOptions options = launch;
    // Resolve in the sender's working directory before crossing process boundaries.
    options.pinFile = AbsolutePath(options.pinFile);
    options.configPath = AbsolutePath(options.configPath);
    auto payload = Encode(options);
    COPYDATASTRUCT data{CopyDataTag, static_cast<DWORD>(payload.size()), payload.data()};
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    HWND target = nullptr;
    while (!(target = FindWindowW(MainWindowClass, windowTitle_.c_str()))) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) { return ForwardResult::Unavailable; }
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(20, deadline - now)));
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) { return ForwardResult::TimedOut; }
    if (options.command == AppCommand::Screenshot || options.command == AppCommand::Gif || options.command == AppCommand::Pin) {
        DWORD process = 0;
        if (GetWindowThreadProcessId(target, &process) != 0) {
            // A foreground-eligible launcher can transfer that permission to
            // the existing tray process. Windows may decline the transfer.
            AllowSetForegroundWindow(process);
        }
    }
    DWORD_PTR response = 0;
    SetLastError(ERROR_SUCCESS);
    if (!SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, static_cast<UINT>(deadline - now), &response)) {
        return GetLastError() == ERROR_TIMEOUT ? ForwardResult::TimedOut : ForwardResult::Unavailable;
    }
    if (response == Accepted) { return ForwardResult::Delivered; }
    if (response == ConfigConflict) { return ForwardResult::ConfigConflict; }
    return ForwardResult::Rejected;
}

} // namespace vispick
