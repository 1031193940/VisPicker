#include <doctest/doctest.h>
#include "core/AppCommand.h"

#include <stdexcept>
#include <vector>

namespace {
vispick::LaunchOptions Parse(std::initializer_list<const wchar_t*> values) {
    std::vector<std::wstring> arguments;
    for (const auto* value : values) { arguments.emplace_back(value); }
    return vispick::ParseLaunchOptions(arguments);
}
} // namespace

TEST_CASE("No arguments selects resident startup without an action") {
    const auto options = Parse({});
    CHECK(options.command == vispick::AppCommand::None);
    CHECK_FALSE(options.help);
    CHECK_FALSE(options.performanceLogging);
    CHECK(options.configPath.empty());
}

TEST_CASE("Product actions parse independently of developer spike commands") {
    CHECK(Parse({L"--screenshot"}).command == vispick::AppCommand::Screenshot);
    CHECK(Parse({L"--gif"}).command == vispick::AppCommand::Gif);
    CHECK(Parse({L"--pin"}).command == vispick::AppCommand::Pin);
    CHECK(Parse({L"--pin-clipboard"}).command == vispick::AppCommand::PinClipboard);
    CHECK(Parse({L"--toggle-pins"}).command == vispick::AppCommand::TogglePins);
    CHECK(Parse({L"--reload"}).command == vispick::AppCommand::Reload);
    CHECK(Parse({L"--exit"}).command == vispick::AppCommand::Exit);
    CHECK_THROWS_AS(Parse({L"--spike-gif"}), std::invalid_argument);
}

TEST_CASE("Product arguments preserve Unicode and spaces in paths") {
    const auto options =
        Parse({L"--config", L"D:\\配置 空间\\设置.ini", L"--pin-file", L"D:\\参考图\\图片 🖼.png", L"--perf"});
    CHECK(options.command == vispick::AppCommand::PinFile);
    CHECK(options.configPath.native() == L"D:\\配置 空间\\设置.ini");
    CHECK(options.pinFile.native() == L"D:\\参考图\\图片 🖼.png");
    CHECK(options.performanceLogging);
}

TEST_CASE("Multiple product actions and repeated options are rejected") {
    CHECK_THROWS_AS(Parse({L"--screenshot", L"--gif"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--exit", L"--exit"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--config", L"one.ini", L"--config", L"two.ini"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--perf", L"--perf"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"unused.png"}), std::invalid_argument);
}

TEST_CASE("Missing file operands never consume another option") {
    CHECK_THROWS_AS(Parse({L"--config"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--config", L""}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--config", L"--exit"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--pin-file"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--pin-file", L"--reload"}), std::invalid_argument);
}

TEST_CASE("Help and version are standalone side effect free requests") {
    CHECK(Parse({L"--help"}).help);
    CHECK(Parse({L"-h"}).help);
    CHECK(Parse({L"--version"}).version);
    CHECK_THROWS_AS(Parse({L"--help", L"--config", L"settings.ini"}), std::invalid_argument);
    CHECK_THROWS_AS(Parse({L"--version", L"--exit"}), std::invalid_argument);
}
