#include <doctest/doctest.h>
#include "platform/Tray.h"

#include <shellapi.h>
#include <stdexcept>

TEST_CASE("Tray dispatch uses configured left click command without shell registration") {
    vispick::Tray tray(GetDesktopWindow(), WM_APP + 9, LoadIconW(nullptr, IDI_APPLICATION));
    for (const auto command : {vispick::AppCommand::Screenshot, vispick::AppCommand::Gif,
         vispick::AppCommand::Pin, vispick::AppCommand::None}) {
        tray.SetLeftClickCommand(command);
        CHECK(tray.HandleCallback(1, WM_LBUTTONUP, false) == command);
        CHECK(tray.HandleCallback(1, NIN_SELECT, false) == command);
        CHECK(tray.HandleCallback(1, NIN_KEYSELECT, false) == command);
    }
    CHECK(tray.HandleCallback(2, WM_LBUTTONUP, false) == vispick::AppCommand::None);
    CHECK(tray.HandleCallback(1, WM_MOUSEMOVE, false) == vispick::AppCommand::None);
    tray.SetLanguage(true);
    tray.SetLanguage(false);
    CHECK_FALSE(tray.Notify(L"title", L"message"));
}

TEST_CASE("Tray menu command mapping rejects unknown IDs and file command without path") {
    CHECK(vispick::Tray::CommandFromMenuId(0) == vispick::AppCommand::None);
    CHECK(vispick::Tray::CommandFromMenuId(1000) == vispick::AppCommand::None);
    CHECK(vispick::Tray::CommandFromMenuId(0xFFFFFFFFU) == vispick::AppCommand::None);
    for (UINT command = 1; command <= static_cast<UINT>(vispick::AppCommand::Exit); ++command) {
        const auto expected = command == static_cast<UINT>(vispick::AppCommand::PinFile)
            ? vispick::AppCommand::None : static_cast<vispick::AppCommand>(command);
        CHECK(vispick::Tray::CommandFromMenuId(1000 + command) == expected);
    }
}

TEST_CASE("Tray validates owner icon and callback and registers stable recovery message") {
    const HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
    CHECK_THROWS_AS(vispick::Tray(nullptr, WM_APP + 1, icon), std::invalid_argument);
    CHECK_THROWS_AS(vispick::Tray(GetDesktopWindow(), WM_APP + 1, nullptr), std::invalid_argument);
    CHECK_THROWS_AS(vispick::Tray(GetDesktopWindow(), WM_USER, icon), std::invalid_argument);
    const UINT message = vispick::Tray::TaskbarCreatedMessage();
    CHECK(message >= 0xC000);
    CHECK(vispick::Tray::TaskbarCreatedMessage() == message);
}
