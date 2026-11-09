#include <doctest/doctest.h>
#include "core/Log.h"
#include "core/CrashDump.h"
#include "platform/Console.h"

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace {

class DiagnosticTestDirectory final {
public:
    DiagnosticTestDirectory() {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() / (L"VisPick-diagnostic-tests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(sequence.fetch_add(1)));
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error("Test directory already exists"); }
    }
    ~DiagnosticTestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    DiagnosticTestDirectory(const DiagnosticTestDirectory&) = delete;
    DiagnosticTestDirectory& operator=(const DiagnosticTestDirectory&) = delete;
    std::filesystem::path path;
};

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

LONG WINAPI PriorExceptionFilter(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }

} // namespace

TEST_CASE("Disabled logging creates no files or directories and accepts arbitrary messages") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path / L"never-created"};
    vispick::Log log;
    CHECK_FALSE(log.Status().enabled);
    log.Write(vispick::LogLevel::Error, L"disabled");
    log.WriteUtf8(vispick::LogLevel::Error, std::string("\xFF", 1));
    CHECK_FALSE(log.Configure(false, candidates).enabled);
    CHECK_FALSE(std::filesystem::exists(candidates[0]));
    CHECK(std::filesystem::is_empty(directory.path));
}

TEST_CASE("Log records preserve UTF8 timestamp levels and escaped record boundaries") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    REQUIRE(status.enabled);
    CHECK_FALSE(status.usingFallback);
    CHECK(status.path == directory.path / L"VisPick.log");
    log.Write(vispick::LogLevel::Warning, L"中文路径：D:\\参考图\\🎨.png");
    log.WriteUtf8(vispick::LogLevel::Error, std::string("first\r\nsecond\0third", 19));
    REQUIRE_FALSE(log.Configure(false).enabled);
    const auto bytes = ReadBytes(status.path);
    CHECK_NOTHROW(static_cast<void>(vispick::Utf8ToWide(bytes)));
    CHECK(bytes.find("[WARN] 中文路径：D:\\参考图\\🎨.png\r\n") != std::string::npos);
    CHECK(bytes.find("[ERROR] first\\r\\nsecond\\0third\r\n") != std::string::npos);
    const std::regex record(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[(INFO|WARN|ERROR)\] .*\r$)");
    std::istringstream lines(bytes);
    std::string line;
    unsigned count = 0;
    while (std::getline(lines, line)) { CHECK(std::regex_match(line, record)); ++count; }
    CHECK(count == 3);
}

TEST_CASE("Log disable and reenable append without writing disabled messages") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    REQUIRE(status.enabled);
    log.WriteUtf8(vispick::LogLevel::Info, "before-disable");
    REQUIRE_FALSE(log.Configure(false).enabled);
    const auto size = std::filesystem::file_size(status.path);
    log.WriteUtf8(vispick::LogLevel::Info, "must-not-appear");
    CHECK(std::filesystem::file_size(status.path) == size);
    REQUIRE(log.Configure(true, candidates).enabled);
    log.WriteUtf8(vispick::LogLevel::Info, "after-enable");
    REQUIRE_FALSE(log.Configure(false).enabled);
    const auto bytes = ReadBytes(status.path);
    CHECK(bytes.find("before-disable") != std::string::npos);
    CHECK(bytes.find("after-enable") != std::string::npos);
    CHECK(bytes.find("must-not-appear") == std::string::npos);
}

TEST_CASE("Log reports ordered directory fallback and actual path") {
    DiagnosticTestDirectory directory;
    const auto blocked = directory.path / L"ordinary-file";
    { std::ofstream file(blocked); file << "do not replace"; }
    const std::array candidates{blocked, directory.path / L"fallback"};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    REQUIRE(status.enabled);
    CHECK(status.usingFallback);
    CHECK(status.error == ERROR_SUCCESS);
    CHECK(status.path == candidates[1] / L"VisPick.log");
    CHECK(ReadBytes(blocked) == "do not replace");
    CHECK(std::filesystem::exists(status.path));
}

TEST_CASE("Log all-path failure is explicit and does not become enabled") {
    DiagnosticTestDirectory directory;
    const auto blocked = directory.path / L"ordinary-file";
    { std::ofstream file(blocked); file << "keep"; }
    const std::array candidates{blocked};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    CHECK_FALSE(status.enabled);
    CHECK(status.error != ERROR_SUCCESS);
    CHECK(status.path.empty());
    log.WriteUtf8(vispick::LogLevel::Error, "must-not-append-to-file");
    CHECK(ReadBytes(blocked) == "keep");
}

TEST_CASE("Log rejects malformed Unicode and remains usable") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    REQUIRE(status.enabled);
    log.WriteUtf8(vispick::LogLevel::Warning, std::string("\xC0\xAF", 2));
    CHECK(log.Status().error == ERROR_NO_UNICODE_TRANSLATION);
    log.Write(vispick::LogLevel::Error, std::wstring(1, static_cast<wchar_t>(0xD800)));
    CHECK(log.Status().error == ERROR_NO_UNICODE_TRANSLATION);
    log.WriteUtf8(vispick::LogLevel::Info, "valid-after-invalid");
    REQUIRE_FALSE(log.Configure(false).enabled);
    const auto bytes = ReadBytes(status.path);
    CHECK_NOTHROW(static_cast<void>(vispick::Utf8ToWide(bytes)));
    CHECK(bytes.find("valid-after-invalid") != std::string::npos);
}

TEST_CASE("UI and worker logging produce complete unique records without interleaving") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path};
    vispick::Log log;
    const auto status = log.Configure(true, candidates);
    REQUIRE(status.enabled);
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&log, worker] {
            for (unsigned record = 0; record < 150; ++record) {
                log.WriteUtf8(vispick::LogLevel::Info, "record=" + std::to_string(worker) + ":" + std::to_string(record) + ":中文");
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    CHECK(log.Status().error == ERROR_SUCCESS);
    REQUIRE_FALSE(log.Configure(false).enabled);
    const auto bytes = ReadBytes(status.path);
    CHECK_NOTHROW(static_cast<void>(vispick::Utf8ToWide(bytes)));
    std::istringstream lines(bytes);
    std::set<std::string> messages;
    std::string line;
    while (std::getline(lines, line)) {
        const auto start = line.find("record=");
        if (start != std::string::npos) { messages.insert(line.substr(start)); }
    }
    CHECK(messages.size() == 900);
    for (unsigned worker = 0; worker < 6; ++worker) {
        for (unsigned record = 0; record < 150; ++record) {
            CHECK(messages.contains("record=" + std::to_string(worker) + ":" + std::to_string(record) + ":中文\r"));
        }
    }
}

TEST_CASE("Crash handler installation creates no file and does not load dbghelp") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path / L"no-crash-no-directory"};
    const auto before = GetModuleHandleW(L"dbghelp.dll");
    {
        vispick::CrashDump handler;
        REQUIRE(handler.Install(candidates).installed);
        CHECK(GetModuleHandleW(L"dbghelp.dll") == before);
        CHECK_FALSE(std::filesystem::exists(candidates[0]));
        CHECK(handler.Install(candidates).installed);
    }
    CHECK_FALSE(std::filesystem::exists(candidates[0]));
}

TEST_CASE("Crash handler has one owner and restores the prior exception filter") {
    DiagnosticTestDirectory directory;
    const std::array candidates{directory.path};
    const auto original = SetUnhandledExceptionFilter(PriorExceptionFilter);
    {
        vispick::CrashDump handler;
        CHECK(handler.Install(candidates).installed);
        vispick::CrashDump competing;
        CHECK_FALSE(competing.Install(candidates).installed);
        CHECK(competing.Status().error == ERROR_ALREADY_EXISTS);
        handler.Uninstall();
        CHECK_FALSE(handler.Status().installed);
        CHECK(competing.Install(candidates).installed);
    }
    const auto restored = SetUnhandledExceptionFilter(original);
    CHECK(restored == PriorExceptionFilter);
}

TEST_CASE("Crash handler rejects paths that cannot fit its fixed exception buffers") {
    const std::array candidates{std::filesystem::path(std::wstring(1100, L'x'))};
    vispick::CrashDump handler;
    const auto status = handler.Install(candidates);
    CHECK_FALSE(status.installed);
    CHECK(status.error == ERROR_FILENAME_EXCED_RANGE);
}
