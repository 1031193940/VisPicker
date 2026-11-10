#include "core/CommandLine.h"
#include "core/Report.h"
#include "platform/Compat.h"
#include "spikes/TestEnvironment.h"
#include "platform/Console.h"
#include "spikes/PatternProcess.h"
#include "spikes/Spikes.h"

#include <stdexcept>
#include <vector>

int wmain() {
    vispick::InitializeDpiAwareness();
    const vispick::ConsoleOutput output;
    try {
        const auto arguments = vispick::ProcessArguments();
        if (!arguments.empty() && arguments.front() == L"--pattern-service") {
            // This private child is unattended and may have no console handles.
            // Never turn a service failure into a blocking MessageBox: the parent
            // observes shared failure state / readiness or the process exit code.
            try {
                return vispick::RunPatternService(std::span<const std::wstring>(arguments).subspan(1));
            } catch (const std::exception& error) {
                OutputDebugStringA("VisPick pattern service failed: ");
                OutputDebugStringA(error.what());
                OutputDebugStringA("\n");
                return 2;
            } catch (...) {
                OutputDebugStringW(L"VisPick pattern service failed with an unknown exception.\n");
                return 2;
            }
        }
        auto commandLine = vispick::ParseCommandLine(arguments);
        if (commandLine.command == vispick::SpikeCommand::Help) {
            output.Show(vispick::CommandLineHelp());
            return 0;
        }
        if (commandLine.command == vispick::SpikeCommand::Desktop) {
            throw std::invalid_argument("Use VisPickTest.exe for --desktop-tests.");
        }
        if (commandLine.options.outputDirectory.empty()) {
            commandLine.options.outputDirectory = vispick::DefaultReportDirectory();
        }
        std::filesystem::create_directories(commandLine.options.outputDirectory);
        const auto environment = vispick::DetectTestEnvironment();
        std::vector<vispick::SpikeResult> results;
        const auto run = [&](vispick::SpikeCommand command, const char* name, auto function) {
            if (commandLine.command != command && commandLine.command != vispick::SpikeCommand::All) { return; }
            output.Show(L"Running " + vispick::Utf8ToWide(name) + L"...\n");
            try {
                results.push_back(function(commandLine.options));
            } catch (const std::exception& error) {
                results.push_back({name, false, {}, {vispick::WideToUtf8(vispick::ExceptionMessage(error))}});
            }
            vispick::WriteReports(commandLine.options.outputDirectory, results, environment);
        };
        run(vispick::SpikeCommand::Capture, "capture", vispick::RunCaptureSpike);
        run(vispick::SpikeCommand::Overlay, "overlay", vispick::RunOverlaySpike);
        run(vispick::SpikeCommand::Pin, "pin", vispick::RunPinSpike);
        run(vispick::SpikeCommand::Gif, "gif-correctness", vispick::RunGifSpike);
        run(vispick::SpikeCommand::GifPipeline, "gif-pipeline", vispick::RunGifPipelineSpike);
        run(vispick::SpikeCommand::Quantizer, "quantizer", vispick::RunQuantizerBenchmark);
        const bool passed = vispick::AcceptancePassed(results, environment);
        output.Show(!environment.physicalAcceptanceEligible ? L"NOT ELIGIBLE: 非验收有效环境，数据仅供诊断。\n"
                                                            : (passed ? L"PASS\n" : L"FAIL: see results.json\n"));
        output.Show(L"Reports: " + commandLine.options.outputDirectory.native() + L"\n");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        output.Show(L"VisPickSpikes: " + vispick::ExceptionMessage(error) + L"\n", true);
        return 2;
    }
}
