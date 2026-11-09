#include <doctest/doctest.h>
#include "core/CommandLine.h"
#include <stdexcept>
#include <vector>

namespace {
vispick::CommandLine Parse(std::initializer_list<std::wstring> arguments) {
    return vispick::ParseCommandLine(std::vector<std::wstring>(arguments));
}
}

TEST_CASE("No command displays help without running tests") {
    CHECK(Parse({}).command == vispick::SpikeCommand::Help);
    CHECK(Parse({L"--help"}).command == vispick::SpikeCommand::Help);
}
TEST_CASE("All four prototype commands are distinct") {
    CHECK(Parse({L"--spike-capture"}).command == vispick::SpikeCommand::Capture);
    CHECK(Parse({L"--spike-overlay"}).command == vispick::SpikeCommand::Overlay);
    CHECK(Parse({L"--spike-pin"}).command == vispick::SpikeCommand::Pin);
    CHECK(Parse({L"--spike-gif"}).command == vispick::SpikeCommand::Gif);
    CHECK(Parse({L"--spike-gif-pipeline"}).command == vispick::SpikeCommand::GifPipeline);
    CHECK(Parse({L"--spike-quantizer"}).command == vispick::SpikeCommand::Quantizer);
    CHECK(Parse({L"--all"}).command == vispick::SpikeCommand::All);
    CHECK(Parse({L"--desktop-tests"}).command == vispick::SpikeCommand::Desktop);
}
TEST_CASE("Command options preserve Unicode paths and accept either order") {
    const auto command = Parse({L"--output", L"C:\\测试 报告", L"--spike-gif", L"--seconds", L"10", L"--iterations", L"1000"});
    CHECK(command.options.outputDirectory.native() == L"C:\\测试 报告");
    CHECK(command.options.seconds == 10);
    CHECK(command.options.iterations == 1000);
}
TEST_CASE("Missing and duplicate arguments are rejected") {
    CHECK_THROWS_AS(Parse({L"--spike-gif", L"--seconds"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--spike-gif", L"--output", L"--seconds"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--spike-pin", L"--spike-overlay"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--seconds", L"10"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--spike-gif", L"--seconds", L"10", L"--seconds", L"11"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--spike-overlay", L"--iterations", L"10", L"--iterations", L"11"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--spike-capture", L"--output", L"one", L"--output", L"two"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--unknown"}), std::invalid_argument);
}
TEST_CASE("Numeric values have strict bounds and overflow protection") {
    for (const auto* invalid : {L"0", L"-1", L"61", L"+1", L"1.5", L" 1", L"", L"99999999999999999999999999999999999999"}) {
        CHECK_THROWS_AS(Parse({L"--spike-gif", L"--seconds", invalid}), std::invalid_argument);
    }
    CHECK(Parse({L"--spike-gif", L"--seconds", L"60"}).options.seconds == 60);
    CHECK(Parse({L"--spike-overlay", L"--iterations", L"10000"}).options.iterations == 10000);
    CHECK_THROWS_AS(Parse({L"--spike-overlay", L"--iterations", L"10001"}), std::invalid_argument);
}

