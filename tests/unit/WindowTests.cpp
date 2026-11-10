#include <doctest/doctest.h>
#include "platform/Window.h"
#include "spikes/PatternProcess.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

class TestWindow final : public vispick::Window {
public:
    ~TestWindow() override { Destroy(); }
    UINT throwOnMessage = 0;
    int destructionMessages = 0;

protected:
    LRESULT OnMessage(const UINT message, const WPARAM parameter, const LPARAM data) override {
        if (message == throwOnMessage) { throw std::runtime_error("expected window-test failure"); }
        if (message == WM_NCDESTROY) { ++destructionMessages; }
        return Window::OnMessage(message, parameter, data);
    }
};

void CreateHidden(TestWindow& window, const wchar_t* className,
                  const vispick::WindowActivation activation = vispick::WindowActivation::Passive) {
    window.Create({className, CS_DBLCLKS, IDC_ARROW}, L"hidden test", WS_POPUP, WS_EX_TOOLWINDOW, {0, 0, 32, 32},
                  activation);
}

UINT observedMessage = 0;
void ObserveException(const vispick::WindowException& exception) noexcept {
    observedMessage = exception.message;
}

class QuitMessageCleanup {
public:
    ~QuitMessageCleanup() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
};

} // namespace

TEST_CASE("Window types register independent class names and activation policies") {
    TestWindow first;
    TestWindow second;
    CreateHidden(first, L"VisPick.Tests.FirstClass");
    CreateHidden(second, L"VisPick.Tests.SecondClass", vispick::WindowActivation::Interactive);
    wchar_t firstClass[128]{};
    wchar_t secondClass[128]{};
    REQUIRE(GetClassNameW(first.Handle(), firstClass, 128) != 0);
    REQUIRE(GetClassNameW(second.Handle(), secondClass, 128) != 0);
    CHECK(std::wstring(firstClass) != std::wstring(secondClass));
    CHECK((GetWindowLongPtrW(first.Handle(), GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);
    CHECK((GetWindowLongPtrW(second.Handle(), GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0);
}

TEST_CASE("Conflicting class options are rejected without creating a second window") {
    TestWindow first;
    CreateHidden(first, L"VisPick.Tests.ConflictingClass");
    TestWindow second;
    CHECK_THROWS_AS(second.Create({L"VisPick.Tests.ConflictingClass", 0, IDC_CROSS}, L"test", WS_POPUP, 0,
                                  {0, 0, 10, 10}, vispick::WindowActivation::Passive),
                    std::invalid_argument);
    CHECK(second.Handle() == nullptr);
}

TEST_CASE("WM_NCCREATE and WM_CREATE exceptions reject creation and detach state") {
    for (const UINT message : {static_cast<UINT>(WM_NCCREATE), static_cast<UINT>(WM_CREATE)}) {
        TestWindow window;
        window.throwOnMessage = message;
        const auto previousCount = vispick::Window::ExceptionCount();
        CHECK_THROWS_AS(CreateHidden(window, L"VisPick.Tests.RejectedCreation"), std::runtime_error);
        CHECK(window.Handle() == nullptr);
        CHECK(vispick::Window::ExceptionCount() == previousCount + 1);
    }
}

TEST_CASE("Message exceptions are observable and never unwind through SendMessage") {
    TestWindow window;
    CreateHidden(window, L"VisPick.Tests.ExceptionBoundary");
    window.throwOnMessage = WM_APP + 1;
    observedMessage = 0;
    vispick::Window::SetExceptionCallback(ObserveException);
    const auto previousCount = vispick::Window::ExceptionCount();
    CHECK_NOTHROW(SendMessageW(window.Handle(), window.throwOnMessage, 0, 0));
    vispick::Window::SetExceptionCallback(nullptr);
    CHECK(observedMessage == WM_APP + 1);
    CHECK(vispick::Window::ExceptionCount() == previousCount + 1);
    window.Destroy();
    CHECK(window.Handle() == nullptr);
    CHECK(window.destructionMessages == 1);
    window.Destroy();
    CHECK(window.destructionMessages == 1);
}

TEST_CASE("Message timer preserves WM_QUIT and cancels future and expired deadlines") {
    const QuitMessageCleanup cleanup;
    vispick::MessageTimer timer;
    PostQuitMessage(37);
    CHECK_FALSE(timer.WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    MSG pending{};
    REQUIRE(PeekMessageW(&pending, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) != FALSE);
    CHECK(pending.wParam == 37);
    // A missed deadline must not bypass the same pending cancellation signal.
    CHECK_FALSE(timer.WaitUntil(std::chrono::steady_clock::now() - std::chrono::seconds(1)));
    REQUIRE(PeekMessageW(&pending, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE);
    CHECK(pending.wParam == 37);
}
