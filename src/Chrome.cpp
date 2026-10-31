#include "Chrome.h"

#include <wx/app.h>

#ifdef __WXMSW__
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace chrome {

namespace {

#ifdef __WXMSW__
// DWM window attributes from the Windows 11 SDK, spelled out so older SDKs build too.
constexpr DWORD kCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColour = 34;       // DWMWA_BORDER_COLOR
constexpr DWORD kCaptionColour = 35;      // DWMWA_CAPTION_COLOR
constexpr DWORD kCaptionTextColour = 36;  // DWMWA_TEXT_COLOR
constexpr DWORD kRoundCorners = 2;        // DWMWCP_ROUND

bool SetAttribute(wxWindow* window, DWORD attribute, DWORD value)
{
    return SUCCEEDED(DwmSetWindowAttribute(window->GetHWND(), attribute, &value, sizeof value));
}

DWORD ColorRef(const wxColour& c)
{
    return RGB(c.Red(), c.Green(), c.Blue());
}
#endif

} // namespace

void EnableDarkMode()
{
#ifdef __WXMSW__
    wxTheApp->MSWEnableDarkMode(wxApp::DarkMode_Always);
#endif
}

void StyleTitleBar(wxTopLevelWindow* window)
{
#ifdef __WXMSW__
    SetAttribute(window, kCaptionColour, ColorRef(kBackground));
    SetAttribute(window, kCaptionTextColour, ColorRef(kTextSoft));
    SetAttribute(window, kBorderColour, ColorRef(kBorder));
#else
    (void)window;
#endif
}

bool RoundCorners(wxWindow* popup)
{
#ifdef __WXMSW__
    if (!SetAttribute(popup, kCornerPreference, kRoundCorners))
        return false;
    SetAttribute(popup, kBorderColour, ColorRef(kBorder));
    return true;
#else
    (void)popup;
    return false;
#endif
}

} // namespace chrome
