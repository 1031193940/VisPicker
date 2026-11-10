#include <doctest/doctest.h>
#include "core/Report.h"
#include <fstream>
#include <iterator>
#include <limits>

TEST_CASE("JSON report escaping preserves UTF-8 and handles all controls") {
    CHECK(vispick::JsonEscape("line\n\t\"\\\r") == "line\\n\\t\\\"\\\\\\r");
    CHECK(vispick::JsonEscape("中文报告") == "中文报告");
    CHECK(vispick::JsonEscape(std::string("\0\x01\x1f", 3)) == "\\u0000\\u0001\\u001f");
    CHECK(vispick::JsonEscape("").empty());
}

TEST_CASE("Markdown numbers retain integer precision without scientific notation") {
    CHECK(vispick::FormatMeasurement(11366400) == "11366400");
    CHECK(vispick::FormatMeasurement(1717873) == "1717873");
    CHECK(vispick::FormatMeasurement(9007199254740991.0) == "9007199254740991");
    CHECK(vispick::FormatMeasurement(-123456789) == "-123456789");
    CHECK(vispick::FormatMeasurement(0.0123456789012) == "0.0123456789012");
    CHECK(vispick::FormatMeasurement(std::numeric_limits<double>::infinity()) == "n/a");
}

TEST_CASE("A successful diagnostic in an invalid environment is never an acceptance pass") {
    struct ReportFiles {
        std::filesystem::path directory = std::filesystem::temp_directory_path() /
            (L"VisPick-report-unit-" + std::to_wstring(GetCurrentProcessId()));
        ~ReportFiles() {
            std::error_code ignored;
            std::filesystem::remove(directory / L"results.json", ignored);
            std::filesystem::remove(directory / L"results.md", ignored);
            std::filesystem::remove(directory, ignored);
        }
    } files;
    vispick::TestEnvironment environment;
    environment.remoteSession = true;
    environment.sessionDetectionKnown = true;
    environment.invalidReasons = {"Remote session"};
    vispick::TestMonitorInfo monitor;
    monitor.deviceName = L"DISPLAY1";
    monitor.adapterName = L"Remote 显示适配器";
    monitor.refreshHz = 32;
    environment.monitors.push_back(monitor);
    const std::vector<vispick::SpikeResult> results{{"example", true,
        {{"pixels", 11366400, "pixels"}, {"bytes", 1717873, "bytes"}}, {}}};
    vispick::WriteReports(files.directory, results, environment);
    std::ifstream jsonFile(files.directory / L"results.json", std::ios::binary);
    std::ifstream markdownFile(files.directory / L"results.md", std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(jsonFile)), {});
    const std::string markdown((std::istreambuf_iterator<char>(markdownFile)), {});
    CHECK(json.find("\"passed\":false") != std::string::npos);
    CHECK(json.find("\"checks_passed\":true") != std::string::npos);
    CHECK(json.find("\"acceptance_status\":\"not_eligible\"") != std::string::npos);
    CHECK(json.find("\"refresh_hz\":32") != std::string::npos);
    CHECK(json.find("Remote 显示适配器") != std::string::npos);
    CHECK(markdown.find("NOT ELIGIBLE") != std::string::npos);
    CHECK(markdown.find("| pixels | 11366400 | pixels |") != std::string::npos);
    CHECK(markdown.find("| bytes | 1717873 | bytes |") != std::string::npos);
    CHECK_FALSE(vispick::AcceptancePassed(results, environment));
    environment.physicalAcceptanceEligible = true;
    CHECK(vispick::AcceptancePassed(results, environment));
    CHECK_FALSE(vispick::AcceptancePassed({}, environment));
}
