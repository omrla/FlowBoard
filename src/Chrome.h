#pragma once

#include <wx/colour.h>
#include <wx/toplevel.h>
#include <wx/window.h>

// The window chrome (top bar, camera panel, dropdown cards): dark, like the board's own
// floating toolbars, so the drawing is the brightest thing on screen.
namespace chrome {

const wxColour kBackground(24, 26, 31);
const wxColour kRaised(34, 37, 43);      // cards, the status pill
const wxColour kHover(44, 48, 56);       // hovered and open items
const wxColour kBorder(48, 52, 60);
const wxColour kText(232, 234, 237);
const wxColour kTextSoft(200, 204, 211);
const wxColour kMuted(140, 146, 158);
const wxColour kAccent(61, 139, 255);
const wxColour kGood(52, 199, 116);
const wxColour kWarn(240, 180, 60);
const wxColour kBad(240, 86, 86);

// Dark mode for every native control, menu and title bar. Call once, before the first
// window is created.
void EnableDarkMode();
// Gives a top-level window's title bar the chrome's colour, so it and the top bar read
// as one header (Windows 11; no effect elsewhere).
void StyleTitleBar(wxTopLevelWindow* window);
// Rounds a popup's corners and gives it the chrome's border (Windows 11). False where
// the system can't, in which case the popup should draw its own border.
bool RoundCorners(wxWindow* popup);

} // namespace chrome
