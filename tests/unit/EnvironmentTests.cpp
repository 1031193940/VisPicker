#include <doctest/doctest.h>

#include "platform/Compat.h"

namespace {

vispick::TestEnvironment PhysicalEnvironment() {
    vispick::TestEnvironment environment;
    environment.sessionDetectionKnown = true;
    vispick::MonitorInfo monitor;
    monitor.deviceName = L"\\\\.\\DISPLAY1";
    monitor.adapterName = L"NVIDIA Test Adapter";
    monitor.refreshHz = 60;
    monitor.adapterFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
    monitor.physicalDisplayVerified = true;
    environment.monitors.push_back(monitor);
    return environment;
}

} // namespace

TEST_CASE("Verified local physical displays are acceptance eligible") {
    auto environment = PhysicalEnvironment();
    vispick::EvaluateTestEnvironment(environment);
    CHECK(environment.physicalAcceptanceEligible);
    CHECK(environment.invalidReasons.empty());
}

TEST_CASE("RemoteFX adapter evidence overrides a false SM_REMOTESESSION metric") {
    auto environment = PhysicalEnvironment();
    environment.remoteSessionMetric = false;
    environment.monitors.front().adapterName = L"Microsoft Remote Display Adapter";
    vispick::EvaluateTestEnvironment(environment);
    CHECK(environment.remoteSession);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
    CHECK_FALSE(environment.invalidReasons.empty());
}

TEST_CASE("Remote display flags invalidate a locally named session") {
    auto environment = PhysicalEnvironment();
    environment.monitors.front().adapterFlags |= 0x04000000;
    vispick::EvaluateTestEnvironment(environment);
    CHECK(environment.remoteSession);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
}

TEST_CASE("WTS or GlassSessionId remote evidence is sufficient to invalidate") {
    auto environment = PhysicalEnvironment();
    environment.remoteSession = true;
    vispick::EvaluateTestEnvironment(environment);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
}

TEST_CASE("Unknown session identity or physical connector cannot pass") {
    SUBCASE("Unknown session identity") {
        auto environment = PhysicalEnvironment();
        environment.sessionDetectionKnown = false;
        vispick::EvaluateTestEnvironment(environment);
        CHECK_FALSE(environment.physicalAcceptanceEligible);
    }
    SUBCASE("Unknown connector") {
        auto environment = PhysicalEnvironment();
        environment.monitors.front().physicalDisplayVerified = false;
        vispick::EvaluateTestEnvironment(environment);
        CHECK_FALSE(environment.physicalAcceptanceEligible);
    }
    SUBCASE("Unknown adapter") {
        auto environment = PhysicalEnvironment();
        environment.monitors.front().adapterName.clear();
        vispick::EvaluateTestEnvironment(environment);
        CHECK_FALSE(environment.physicalAcceptanceEligible);
    }
}

TEST_CASE("Every active monitor must meet the refresh rate gate") {
    auto environment = PhysicalEnvironment();
    auto secondMonitor = environment.monitors.front();
    secondMonitor.refreshHz = 32;
    environment.monitors.push_back(secondMonitor);
    vispick::EvaluateTestEnvironment(environment);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
    environment.monitors.back().refreshHz = 0;
    vispick::EvaluateTestEnvironment(environment);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
    environment.monitors.back().refreshHz = 50;
    vispick::EvaluateTestEnvironment(environment);
    CHECK(environment.physicalAcceptanceEligible);
}

TEST_CASE("A virtual display cannot pass even when its session is local") {
    auto environment = PhysicalEnvironment();
    environment.monitors.front().adapterName = L"Virtual Display Adapter";
    vispick::EvaluateTestEnvironment(environment);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
    environment.monitors.clear();
    vispick::EvaluateTestEnvironment(environment);
    CHECK_FALSE(environment.physicalAcceptanceEligible);
}
