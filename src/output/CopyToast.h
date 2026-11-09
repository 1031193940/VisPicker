#pragma once
#include "platform/Window.h"
#include "platform/Gdi.h"
#include <string>

namespace vispick {
class CopyToast final : public Window {
public:
    explicit CopyToast(HWND owner);
    ~CopyToast() override;
    void Show(RECT selection, bool english);
protected:
    LRESULT OnMessage(UINT message, WPARAM parameter, LPARAM data) override;
private:
    std::wstring text_;
    UniqueGdiObject<HFONT> font_;
};
}
