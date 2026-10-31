#pragma once

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/graphics.h>

enum class IconId {
    Pen, Highlighter, Eraser, Line, Rect, Ellipse, Pan,
    Undo, Redo, Clear, Save, Close, Recenter,
    Camera, CameraOff, Sidebar, Chevron,   // window chrome
};

// Draws a line-art icon centred on (cx, cy), fitting a size x size box.
void DrawIcon(wxGraphicsContext* gc, IconId id, double cx, double cy, double size,
              const wxColour& colour);
