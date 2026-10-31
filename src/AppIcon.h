#pragma once

#include <wx/bmpbndl.h>
#include <wx/iconbndl.h>

// FlowBoard's icon, drawn at any size from res/FlowBoard.svg, which the build embeds in
// the .exe (resource FLOWBOARD_SVG).
wxBitmapBundle AppIconBundle();
// The icon at the sizes Windows asks a window for (title bar, taskbar, Alt+Tab).
wxIconBundle AppIcons();
// Writes the icon as a multi-size .ico, the .exe's own icon in Explorer. Run as
// `FlowBoard.exe --make-icon res\FlowBoard.ico` after changing the SVG.
bool WriteAppIconFile(const wxString& path);
