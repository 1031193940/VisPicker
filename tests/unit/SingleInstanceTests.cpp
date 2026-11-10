#include <doctest/doctest.h>
#include "platform/SingleInstance.h"
#include "platform/Window.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

std::wstring InstanceIdentity() {
    static unsigned sequence = 0;
    return L"VisPick.Tests." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
}

std::optional<vispick::LaunchOptions> Decode(std::vector<std::byte>& bytes) {
    const COPYDATASTRUCT data{vispick::SingleInstance::CopyDataTag, static_cast<DWORD>(bytes.size()), bytes.data()};
    return vispick::SingleInstance::Decode(data);
}

void ChangeWord(std::vector<std::byte>& bytes, const std::size_t word, const std::uint32_t value) {
    std::memcpy(bytes.data() + word * sizeof(value), &value, sizeof(value));
}

class InstanceReceiver final : public vispick::Window {
public:
    ~InstanceReceiver() override { Destroy(); }
    std::optional<vispick::LaunchOptions> received;
    LRESULT response = vispick::SingleInstance::Accepted;
    int dispatchCount = 0;

protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == WM_COPYDATA) {
            received = vispick::SingleInstance::Decode(*reinterpret_cast<const COPYDATASTRUCT*>(data));
            if (!received) { return vispick::SingleInstance::Rejected; }
            if (response == vispick::SingleInstance::Accepted && !PostMessageW(Handle(), WM_APP + 25, 0, 0)) {
                return vispick::SingleInstance::Rejected;
            }
            return response;
        }
        if (message == WM_APP + 25) {
            ++dispatchCount;
            return 0;
        }
        return Window::OnMessage(message, parameter, data);
    }
};

} // namespace

TEST_CASE("SingleInstance mutex identity is shared until all handles close") {
    const auto identity = InstanceIdentity();
    {
        const vispick::SingleInstance first(identity);
        const vispick::SingleInstance second(identity);
        CHECK(first.IsPrimary());
        CHECK_FALSE(second.IsPrimary());
        CHECK(first.WindowTitle() == second.WindowTitle());
        const vispick::SingleInstance independent(InstanceIdentity());
        CHECK(independent.IsPrimary());
    }
    const vispick::SingleInstance reopened(identity);
    CHECK(reopened.IsPrimary());
}

TEST_CASE("SingleInstance rejects invalid object namespace identities") {
    for (const auto* identity : {L"", L"Global\\VisPick", L"name with spaces", L"用户"}) {
        CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance(identity)), std::invalid_argument);
    }
    CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance(std::wstring(65, L'x'))), std::invalid_argument);
}

TEST_CASE("SingleInstance UTF16 protocol preserves all commands and launch flags") {
    for (unsigned command = 0; command <= static_cast<unsigned>(vispick::AppCommand::Exit); ++command) {
        vispick::LaunchOptions options;
        options.command = static_cast<vispick::AppCommand>(command);
        if (options.command == vispick::AppCommand::PinFile) { options.pinFile = L"C:\\素材\\图 🖼.png"; }
        options.configPath = L"C:\\工具\\配置.ini";
        options.performanceLogging = true;
        auto bytes = vispick::SingleInstance::Encode(options);
        const auto decoded = Decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->command == options.command);
        CHECK(decoded->pinFile == options.pinFile);
        CHECK(decoded->configPath == options.configPath);
        CHECK(decoded->performanceLogging);
    }
    for (const bool help : {false, true}) {
        vispick::LaunchOptions options;
        options.help = help;
        options.version = !help;
        auto bytes = vispick::SingleInstance::Encode(options);
        const auto decoded = Decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->help == help);
        CHECK(decoded->version == !help);
    }
}

TEST_CASE("SingleInstance decoder owns payload after sender storage changes") {
    vispick::LaunchOptions options;
    options.configPath = L"D:\\user-config.ini";
    auto bytes = vispick::SingleInstance::Encode(options);
    const auto decoded = Decode(bytes);
    REQUIRE(decoded);
    std::fill(bytes.begin(), bytes.end(), std::byte{0xFF});
    CHECK(decoded->configPath == options.configPath);
}

TEST_CASE("SingleInstance decoder rejects invalid header sizes fields and tags") {
    auto valid = vispick::SingleInstance::Encode({});
    for (DWORD size = 0; size < valid.size(); ++size) {
        const COPYDATASTRUCT data{vispick::SingleInstance::CopyDataTag, size, valid.data()};
        CHECK_FALSE(vispick::SingleInstance::Decode(data));
    }
    COPYDATASTRUCT data{0, static_cast<DWORD>(valid.size()), valid.data()};
    CHECK_FALSE(vispick::SingleInstance::Decode(data));
    data = {vispick::SingleInstance::CopyDataTag, static_cast<DWORD>(valid.size()), nullptr};
    CHECK_FALSE(vispick::SingleInstance::Decode(data));
    data = {vispick::SingleInstance::CopyDataTag, vispick::SingleInstance::MaximumPayloadBytes + 1, valid.data()};
    CHECK_FALSE(vispick::SingleInstance::Decode(data));
    const std::array<std::pair<std::size_t, std::uint32_t>, 7> corruptions{
        {{0, 0}, {1, 2}, {2, 0xFFFFFFFFU}, {3, 8}, {3, 3}, {4, 0xFFFFFFFFU}, {5, 0xFFFFFFFFU}}};
    for (const auto& [word, value] : corruptions) {
        auto changed = valid;
        ChangeWord(changed, word, value);
        CHECK_FALSE(Decode(changed));
    }
    valid.push_back(std::byte{0});
    CHECK_FALSE(Decode(valid));
}

TEST_CASE("SingleInstance validates command path consistency and UTF16") {
    vispick::LaunchOptions options;
    options.command = vispick::AppCommand::PinFile;
    CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance::Encode(options)), std::invalid_argument);
    options.command = vispick::AppCommand::Screenshot;
    options.pinFile = L"image.png";
    CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance::Encode(options)), std::invalid_argument);
    options.pinFile.clear();
    for (const std::wstring invalid : {std::wstring(1, static_cast<wchar_t>(0xD800)),
                                       std::wstring(1, static_cast<wchar_t>(0xDC00)), std::wstring(L"a\0b", 3)}) {
        options.configPath = invalid;
        CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance::Encode(options)), std::invalid_argument);
    }
    options.configPath = L"xx";
    auto malformed = vispick::SingleInstance::Encode(options);
    const wchar_t highSurrogate = 0xD800;
    std::memcpy(malformed.data() + malformed.size() - sizeof(wchar_t), &highSurrogate, sizeof(wchar_t));
    CHECK_FALSE(Decode(malformed));
}

TEST_CASE("SingleInstance bounds payload before allocation and supports exact byte limit") {
    vispick::LaunchOptions options;
    const auto emptyBytes = vispick::SingleInstance::Encode(options).size();
    options.configPath =
        std::wstring((vispick::SingleInstance::MaximumPayloadBytes - emptyBytes) / sizeof(wchar_t), L'x');
    auto bytes = vispick::SingleInstance::Encode(options);
    CHECK(bytes.size() == vispick::SingleInstance::MaximumPayloadBytes);
    REQUIRE(Decode(bytes));
    options.configPath += L"x";
    CHECK_THROWS_AS(static_cast<void>(vispick::SingleInstance::Encode(options)), std::length_error);
}

TEST_CASE("SingleInstance config comparison handles explicit relative and case variants") {
    CHECK(vispick::SingleInstance::ConfigMatches({}, L"C:\\active.ini"));
    CHECK_FALSE(vispick::SingleInstance::ConfigMatches(L"C:\\requested.ini", {}));
    CHECK(vispick::SingleInstance::ConfigMatches(L"C:\\Config\\..\\USER.ini", L"c:\\user.ini"));
    CHECK(vispick::SingleInstance::ConfigMatches(L".\\config.ini", std::filesystem::current_path() / L"config.ini"));
    CHECK_FALSE(vispick::SingleInstance::ConfigMatches(L"C:\\a.ini", L"C:\\b.ini"));
    CHECK_FALSE(vispick::SingleInstance::ConfigMatches(std::wstring(L"a\0b", 3), L"a"));
}

TEST_CASE("SingleInstance delivers owned commands then receiver dispatches asynchronously") {
    const auto identity = InstanceIdentity();
    const vispick::SingleInstance primary(identity);
    const vispick::SingleInstance secondary(identity);
    InstanceReceiver receiver;
    receiver.Create({vispick::SingleInstance::MainWindowClass, 0, IDC_ARROW}, primary.WindowTitle().c_str(), WS_POPUP,
                    WS_EX_TOOLWINDOW, {0, 0, 1, 1}, vispick::WindowActivation::Passive);
    vispick::LaunchOptions options;
    options.command = vispick::AppCommand::PinFile;
    options.pinFile = L"relative.png";
    options.configPath = L"relative.ini";
    CHECK(secondary.Forward(options) == vispick::ForwardResult::Delivered);
    REQUIRE(receiver.received);
    CHECK(receiver.received->pinFile == std::filesystem::absolute(options.pinFile));
    CHECK(receiver.received->configPath == std::filesystem::absolute(options.configPath));
    CHECK(receiver.dispatchCount == 0);
    vispick::PumpWindowMessages();
    CHECK(receiver.dispatchCount == 1);
    receiver.response = vispick::SingleInstance::ConfigConflict;
    CHECK(secondary.Forward(options) == vispick::ForwardResult::ConfigConflict);
    receiver.response = vispick::SingleInstance::Rejected;
    CHECK(secondary.Forward(options) == vispick::ForwardResult::Rejected);
}

TEST_CASE("SingleInstance missing primary window wait is bounded and validates timeout") {
    const auto identity = InstanceIdentity();
    const vispick::SingleInstance primary(identity);
    const vispick::SingleInstance secondary(identity);
    const auto started = GetTickCount64();
    CHECK(secondary.Forward({}, 10) == vispick::ForwardResult::Unavailable);
    CHECK(GetTickCount64() - started < 2000);
    CHECK_THROWS_AS(static_cast<void>(secondary.Forward({}, 0)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(secondary.Forward({}, 30001)), std::invalid_argument);
    vispick::LaunchOptions invalid;
    invalid.configPath = std::wstring(L"a\0b", 3);
    CHECK_THROWS_AS(static_cast<void>(secondary.Forward(invalid, 10)), std::invalid_argument);
}
