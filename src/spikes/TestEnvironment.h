#pragma once

#include "platform/Compat.h"

namespace vispick {

// Hardware evidence belongs to diagnostic tools, not runtime monitor enumeration.
struct TestMonitorInfo : MonitorInfo {
    std::wstring adapterName;
    DWORD refreshHz = 0;
    DWORD adapterFlags = 0;
    bool physicalDisplayVerified = false;
};

struct TestEnvironment {
    bool remoteSession = false;
    bool remoteSessionMetric = false;
    bool sessionDetectionKnown = false;
    bool physicalAcceptanceEligible = false;
    std::vector<TestMonitorInfo> monitors;
    std::vector<std::string> invalidReasons;
};

[[nodiscard]] TestEnvironment DetectTestEnvironment();
// Pure policy used by tests as well as the OS detector. Unknown is ineligible.
void EvaluateTestEnvironment(TestEnvironment& environment);

} // namespace vispick
