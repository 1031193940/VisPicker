#include "DesktopTests.h"
#include "core/CommandLine.h"
#include "core/Report.h"
#include "platform/Compat.h"
#include "platform/Console.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

int wmain(int argumentCount, wchar_t* arguments[]) {
    vispick::InitializeDpiAwareness();
    const vispick::ConsoleOutput output;
    try {
        std::vector<std::wstring> tokens;
        for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex) { tokens.emplace_back(arguments[argumentIndex]); }
        if (tokens.empty()) { tokens.emplace_back(L"--desktop-tests"); }
        auto commandLine = vispick::ParseCommandLine(tokens);
        if (commandLine.command == vispick::SpikeCommand::Help) {
            output.Show(L"VisPickTest.exe --desktop-tests | --all\n" + vispick::CommandLineHelp());
            return 0;
        }
        if (commandLine.options.outputDirectory.empty()) { commandLine.options.outputDirectory = vispick::DefaultReportDirectory(); }
        std::filesystem::create_directories(commandLine.options.outputDirectory);
        std::vector<vispick::SpikeResult> results;
        const auto environment = vispick::DetectTestEnvironment();
        const auto run = [&](vispick::SpikeCommand command, const char* name, auto function) {
            if (commandLine.command != command && commandLine.command != vispick::SpikeCommand::All) { return; }
            std::cout << "Running " << name << "..." << std::endl;
            try { results.push_back(function(commandLine.options)); }
            catch (const std::exception& error) { results.push_back({name, false, {}, {vispick::WideToUtf8(vispick::ExceptionMessage(error))}}); }
            vispick::WriteReports(commandLine.options.outputDirectory, results, environment);
            std::cout << (!environment.physicalAcceptanceEligible ? "NOT ELIGIBLE " : (results.back().passed ? "PASS " : "FAIL ")) << name << std::endl;
        };
        run(vispick::SpikeCommand::Desktop, "desktop_pixels", vispick::RunDesktopTests);
        run(vispick::SpikeCommand::Capture, "capture", vispick::RunCaptureSpike);
        run(vispick::SpikeCommand::Overlay, "overlay", vispick::RunOverlaySpike);
        run(vispick::SpikeCommand::Pin, "pin", vispick::RunPinSpike);
        run(vispick::SpikeCommand::Gif, "gif", vispick::RunGifSpike);
        run(vispick::SpikeCommand::GifPipeline, "gif-pipeline", vispick::RunGifPipelineSpike);
        run(vispick::SpikeCommand::Quantizer, "quantizer", vispick::RunQuantizerBenchmark);
        output.Show(L"Reports: " + commandLine.options.outputDirectory.native() + L"\n");
        return vispick::AcceptancePassed(results, environment) ? 0 : 1;
    } catch (const std::exception& error) {
        output.Show(vispick::ExceptionMessage(error) + L"\n", true);
        return 2;
    }
}

