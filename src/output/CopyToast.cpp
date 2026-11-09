#include "output/CopyToast.h"
#include "platform/Compat.h"
#include <algorithm>

namespace vispick {
CopyToast::CopyToast(HWND owner) {
    Create({L"VisPick.CopyToast", 0, IDC_ARROW}, L"VisPick", WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        {0, 0, 1, 1}, WindowActivation::Passive, owner);
    DisableWindowAnimations(Handle());
}
CopyToast::~CopyToast() { Destroy(); }
void CopyToast::Show(RECT selection, bool english) {
    text_ = english ? L"Copied" : L"已复制";
    const auto dpi = DpiForWindow(Handle());
    const int width = MulDiv(120, static_cast<int>(dpi), 96);
    const int height = MulDiv(38, static_cast<int>(dpi), 96);
    font_.Reset(CreateFontW(-MulDiv(14, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei"));
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromRect(&selection, MONITOR_DEFAULTTONEAREST), &monitor);
    const int left = std::clamp(static_cast<int>((selection.left + selection.right - width) / 2),
        static_cast<int>(monitor.rcWork.left), static_cast<int>(std::max(monitor.rcWork.left, monitor.rcWork.right - width)));
    const int top = std::clamp(static_cast<int>((selection.top + selection.bottom - height) / 2),
        static_cast<int>(monitor.rcWork.top), static_cast<int>(std::max(monitor.rcWork.top, monitor.rcWork.bottom - height)));
    SetWindowPos(Handle(), HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(Handle(), nullptr, FALSE);
    SetTimer(Handle(), 1, 1000, nullptr);
}
LRESULT CopyToast::OnMessage(UINT message, WPARAM parameter, LPARAM data) {
    if (message == WM_TIMER && parameter == 1) { KillTimer(Handle(), 1); ShowWindow(Handle(), SW_HIDE); font_.Reset(); return 0; }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto context = BeginPaint(Handle(), &paint);
        RECT client{}; GetClientRect(Handle(), &client);
        SetDCBrushColor(context, RGB(34, 37, 44));
        FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetTextColor(context, RGB(255,255,255)); SetBkMode(context, TRANSPARENT);
        const auto previous = SelectObject(context, font_.Get() ? font_.Get() : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(context, text_.c_str(), static_cast<int>(text_.size()), &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(context, previous);
        EndPaint(Handle(), &paint);
        return 0;
    }
    return Window::OnMessage(message, parameter, data);
}
}
