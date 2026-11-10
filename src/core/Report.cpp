#include "core/Report.h"
#include "platform/Compat.h"
#include "platform/Console.h"

#include <windows.h>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace vispick {

std::string JsonEscape(std::string_view text) {
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(text.size());
    for (const char rawCharacter : text) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20) {
                escaped += "\\u00";
                escaped += hexadecimal[character >> 4];
                escaped += hexadecimal[character & 0x0f];
            } else {
                escaped += rawCharacter;
            }
        }
    }
    return escaped;
}

std::filesystem::path DefaultReportDirectory() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::wostringstream name;
    name << std::setfill(L'0') << std::setw(4) << time.wYear << std::setw(2) << time.wMonth << std::setw(2) << time.wDay
         << L'-' << std::setw(2) << time.wHour << std::setw(2) << time.wMinute << std::setw(2) << time.wSecond << L'-'
         << GetCurrentProcessId();
    return std::filesystem::temp_directory_path() / L"VisPick" / L"spikes" / name.str();
}

std::string FormatMeasurement(double value) {
    if (!std::isfinite(value)) { return "n/a"; }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    if (std::trunc(value) == value) {
        output << std::fixed << std::setprecision(0);
    } else {
        output << std::setprecision(12);
    }
    output << (value == 0.0 ? 0.0 : value);
    return output.str();
}

bool AcceptancePassed(std::span<const SpikeResult> results, const TestEnvironment& environment) {
    return environment.physicalAcceptanceEligible && !results.empty() &&
           std::all_of(results.begin(), results.end(), [](const SpikeResult& result) { return result.passed; });
}

void WriteReports(const std::filesystem::path& directory, std::span<const SpikeResult> results) {
    WriteReports(directory, results, DetectTestEnvironment());
}

void WriteReports(const std::filesystem::path& directory, std::span<const SpikeResult> results,
                  const TestEnvironment& environment) {
    std::filesystem::create_directories(directory);
    std::ofstream json(directory / L"results.json", std::ios::binary | std::ios::trunc);
    std::ofstream markdown(directory / L"results.md", std::ios::binary | std::ios::trunc);
    json.exceptions(std::ios::badbit | std::ios::failbit);
    markdown.exceptions(std::ios::badbit | std::ios::failbit);
    json.imbue(std::locale::classic());
    markdown.imbue(std::locale::classic());
    const auto version = GetWindowsVersion();
    const auto desktop = VirtualDesktopBounds();
    json << std::setprecision(12) << "{\n  \"schema_version\": 2,\n  \"pointer_bits\": " << sizeof(void*) * 8
         << ",\n  \"windows\": \"" << version.major << '.' << version.minor << '.' << version.build
         << "\",\n  \"desktop\": [" << desktop.left << ',' << desktop.top << ',' << desktop.right << ','
         << desktop.bottom << "],\n  \"remote_session\": " << (environment.remoteSession ? "true" : "false")
         << ",\n  \"remote_session_metric\": " << (environment.remoteSessionMetric ? "true" : "false")
         << ",\n  \"session_detection_known\": " << (environment.sessionDetectionKnown ? "true" : "false")
         << ",\n  \"acceptance_eligible\": " << (environment.physicalAcceptanceEligible ? "true" : "false")
         << ",\n  \"invalid_environment_reasons\": [";
    for (std::size_t reasonIndex = 0; reasonIndex < environment.invalidReasons.size(); ++reasonIndex) {
        if (reasonIndex != 0) { json << ','; }
        json << '"' << JsonEscape(environment.invalidReasons[reasonIndex]) << '"';
    }
    json << "],\n  \"monitors\": [";
    const auto& monitors = environment.monitors;
    for (std::size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
        const auto& monitor = monitors[monitorIndex];
        if (monitorIndex != 0) { json << ','; }
        json << "{\"bounds\":[" << monitor.bounds.left << ',' << monitor.bounds.top << ',' << monitor.bounds.right
             << ',' << monitor.bounds.bottom << "],\"dpi\":[" << monitor.dpiX << ',' << monitor.dpiY
             << "],\"primary\":" << (monitor.primary ? "true" : "false") << ",\"device_name\":\""
             << JsonEscape(WideToUtf8(monitor.deviceName)) << "\",\"adapter_name\":\""
             << JsonEscape(WideToUtf8(monitor.adapterName)) << "\",\"refresh_hz\":" << monitor.refreshHz
             << ",\"physical_display_verified\":" << (monitor.physicalDisplayVerified ? "true" : "false") << '}';
    }
    json << "],\n  \"results\": [\n";
    markdown << "# VisPick P0 measured results\n\nWindows " << version.major << '.' << version.minor << '.'
             << version.build << " / " << sizeof(void*) * 8 << " bit. Virtual desktop: (" << desktop.left << ", "
             << desktop.top << ")–(" << desktop.right << ", " << desktop.bottom << ").\n\n"
             << "Timing gates apply to Release; instrumented builds are for correctness checks.\n\n";
    markdown << "Remote session: " << (environment.remoteSession ? "yes" : "no")
             << "; session detection: " << (environment.sessionDetectionKnown ? "known" : "unknown") << ".\n\n";
    if (!environment.physicalAcceptanceEligible) {
        markdown << "> 🔴 **非验收有效环境 / NOT ELIGIBLE** — Diagnostic measurements only. No acceptance pass or "
                    "capture-backend decision is permitted.\n\n";
        for (const auto& reason : environment.invalidReasons) { markdown << "- " << reason << '\n'; }
        markdown << '\n';
    }
    markdown << "| Display | Adapter | Refresh | Physical verified |\n|---|---|---:|---|\n";
    for (const auto& monitor : monitors) {
        markdown << "| " << WideToUtf8(monitor.deviceName) << " | " << WideToUtf8(monitor.adapterName) << " | "
                 << monitor.refreshHz << " Hz | " << (monitor.physicalDisplayVerified ? "yes" : "no") << " |\n";
    }
    markdown << '\n';
    for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
        const auto& result = results[resultIndex];
        if (resultIndex != 0) { json << ",\n"; }
        const bool accepted = result.passed && environment.physicalAcceptanceEligible;
        json << "    {\"name\":\"" << JsonEscape(result.name) << "\",\"passed\":" << (accepted ? "true" : "false")
             << ",\"checks_passed\":" << (result.passed ? "true" : "false") << ",\"acceptance_status\":\""
             << (!environment.physicalAcceptanceEligible ? "not_eligible" : (accepted ? "pass" : "fail"))
             << "\",\"measurements\":[";
        markdown << "## " << result.name << " — "
                 << (!environment.physicalAcceptanceEligible ? "NOT ELIGIBLE" : (accepted ? "PASS" : "FAIL"))
                 << "\n\nMeasured checks: " << (result.passed ? "passed" : "failed") << ".\n\n"
                 << "| Measurement | Value | Unit |\n|---|---:|---|\n";
        for (std::size_t metricIndex = 0; metricIndex < result.measurements.size(); ++metricIndex) {
            const auto& measurement = result.measurements[metricIndex];
            if (metricIndex != 0) { json << ','; }
            json << "{\"name\":\"" << JsonEscape(measurement.name) << "\",\"value\":";
            if (std::isfinite(measurement.value)) {
                json << FormatMeasurement(measurement.value);
            } else {
                json << "null";
            }
            json << ",\"unit\":\"" << JsonEscape(measurement.unit) << "\"}";
            markdown << "| " << measurement.name << " | " << FormatMeasurement(measurement.value) << " | "
                     << measurement.unit << " |\n";
        }
        json << "],\"notes\":[";
        markdown << '\n';
        for (std::size_t noteIndex = 0; noteIndex < result.notes.size(); ++noteIndex) {
            if (noteIndex != 0) { json << ','; }
            json << '"' << JsonEscape(result.notes[noteIndex]) << '"';
            markdown << "- " << result.notes[noteIndex] << '\n';
        }
        json << "]}";
        markdown << '\n';
    }
    json << "\n  ]\n}\n";
    json.close();
    markdown.close();
}

} // namespace vispick
