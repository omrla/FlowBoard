#include "BoardIcons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

constexpr double kPi = 3.14159265358979323846;

wxGraphicsPath Polyline(wxGraphicsContext* gc, std::initializer_list<wxPoint2DDouble> pts, bool close)
{
    wxGraphicsPath path = gc->CreatePath();
    bool first = true;
    for (const wxPoint2DDouble& p : pts) {
        if (first) path.MoveToPoint(p);
        else path.AddLineToPoint(p);
        first = false;
    }
    if (close)
        path.CloseSubpath();
    return path;
}

} // namespace

void DrawIcon(wxGraphicsContext* gc, IconId id, double cx, double cy, double size,
              const wxColour& colour)
{
    const double s = size;
    auto P = [s](double x, double y) { return wxPoint2DDouble(x * s, y * s); };

    gc->PushState();
    gc->Translate(cx, cy);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(colour, std::max(1.5, s * 0.075))
                                 .Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    gc->SetBrush(wxBrush(colour));

    switch (id) {
    case IconId::Pen:
        gc->Rotate(-kPi / 4);
        gc->StrokePath(Polyline(gc, {P(-0.16, -0.10), P(0.40, -0.10), P(0.40, 0.10), P(-0.16, 0.10),
                                     P(-0.42, 0.0)}, true));
        gc->StrokeLine(-0.16 * s, -0.10 * s, -0.16 * s, 0.10 * s);
        gc->StrokeLine(0.24 * s, -0.10 * s, 0.24 * s, 0.10 * s);
        break;

    case IconId::Highlighter:
        gc->Rotate(-kPi / 4);
        gc->StrokePath(Polyline(gc, {P(-0.08, -0.14), P(0.40, -0.14), P(0.40, 0.14), P(-0.08, 0.14)}, true));
        gc->FillPath(Polyline(gc, {P(-0.08, -0.10), P(-0.36, -0.02), P(-0.36, 0.10), P(-0.08, 0.10)}, true));
        break;

    case IconId::Eraser: {
        gc->Rotate(-kPi / 4);
        wxGraphicsPath body = gc->CreatePath();
        body.AddRoundedRectangle(-0.38 * s, -0.17 * s, 0.76 * s, 0.34 * s, 0.06 * s);
        gc->StrokePath(body);
        wxGraphicsPath tip = gc->CreatePath();
        tip.AddRoundedRectangle(-0.38 * s, -0.17 * s, 0.30 * s, 0.34 * s, 0.06 * s);
        gc->FillPath(tip);
        break;
    }

    case IconId::Line:
        gc->StrokeLine(-0.28 * s, 0.28 * s, 0.28 * s, -0.28 * s);
        gc->DrawEllipse(-0.36 * s, 0.20 * s, 0.16 * s, 0.16 * s);
        gc->DrawEllipse(0.20 * s, -0.36 * s, 0.16 * s, 0.16 * s);
        break;

    case IconId::Rect: {
        wxGraphicsPath path = gc->CreatePath();
        path.AddRoundedRectangle(-0.34 * s, -0.26 * s, 0.68 * s, 0.52 * s, 0.06 * s);
        gc->StrokePath(path);
        break;
    }

    case IconId::Ellipse: {
        wxGraphicsPath path = gc->CreatePath();
        path.AddEllipse(-0.36 * s, -0.27 * s, 0.72 * s, 0.54 * s);
        gc->StrokePath(path);
        break;
    }

    case IconId::Pan: {
        gc->StrokeLine(-0.36 * s, 0, 0.36 * s, 0);
        gc->StrokeLine(0, -0.36 * s, 0, 0.36 * s);
        const double dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs) {
            const double tx = d[0] * 0.36, ty = d[1] * 0.36;
            const double bx = tx - d[0] * 0.12, by = ty - d[1] * 0.12;
            gc->StrokePath(Polyline(gc, {P(bx - d[1] * 0.11, by + d[0] * 0.11), P(tx, ty),
                                         P(bx + d[1] * 0.11, by - d[0] * 0.11)}, false));
        }
        break;
    }

    case IconId::Undo:
    case IconId::Redo: {
        if (id == IconId::Redo)
            gc->Scale(-1, 1);
        // Arrow pointing left with a tail that hooks down and back.
        wxGraphicsPath path = gc->CreatePath();
        path.MoveToPoint(P(-0.32, -0.12));
        path.AddLineToPoint(P(0.06, -0.12));
        const double r = 0.19;
        for (int i = 1; i <= 16; ++i) {
            const double a = -kPi / 2 + kPi * i / 16.0;
            path.AddLineToPoint(P(0.06 + r * std::cos(a), 0.07 + r * std::sin(a)));
        }
        path.AddLineToPoint(P(-0.12, 0.26));
        gc->StrokePath(path);
        gc->StrokePath(Polyline(gc, {P(-0.19, -0.25), P(-0.32, -0.12), P(-0.19, 0.01)}, false));
        break;
    }

    case IconId::Clear:
        gc->StrokeLine(-0.32 * s, -0.24 * s, 0.32 * s, -0.24 * s);
        gc->StrokePath(Polyline(gc, {P(-0.09, -0.24), P(-0.09, -0.34), P(0.09, -0.34), P(0.09, -0.24)}, false));
        gc->StrokePath(Polyline(gc, {P(-0.23, -0.24), P(-0.18, 0.34), P(0.18, 0.34), P(0.23, -0.24)}, false));
        gc->StrokeLine(-0.06 * s, -0.10 * s, -0.06 * s, 0.20 * s);
        gc->StrokeLine(0.06 * s, -0.10 * s, 0.06 * s, 0.20 * s);
        break;

    case IconId::Save:
        gc->StrokeLine(0, -0.36 * s, 0, 0.10 * s);
        gc->StrokePath(Polyline(gc, {P(-0.15, -0.05), P(0, 0.10), P(0.15, -0.05)}, false));
        gc->StrokePath(Polyline(gc, {P(-0.32, 0.06), P(-0.32, 0.30), P(0.32, 0.30), P(0.32, 0.06)}, false));
        break;

    case IconId::Close:
        gc->StrokeLine(-0.24 * s, -0.24 * s, 0.24 * s, 0.24 * s);
        gc->StrokeLine(-0.24 * s, 0.24 * s, 0.24 * s, -0.24 * s);
        break;

    case IconId::Recenter: {   // a target: ring, centre dot and four ticks
        wxGraphicsPath ring = gc->CreatePath();
        ring.AddEllipse(-0.22 * s, -0.22 * s, 0.44 * s, 0.44 * s);
        gc->StrokePath(ring);
        gc->StrokeLine(0, -0.38 * s, 0, -0.26 * s);
        gc->StrokeLine(0, 0.26 * s, 0, 0.38 * s);
        gc->StrokeLine(-0.38 * s, 0, -0.26 * s, 0);
        gc->StrokeLine(0.26 * s, 0, 0.38 * s, 0);
        gc->DrawEllipse(-0.05 * s, -0.05 * s, 0.1 * s, 0.1 * s);
        break;
    }

    case IconId::Camera:
    case IconId::CameraOff: {   // a video camera: body and lens cone
        wxGraphicsPath body = gc->CreatePath();
        body.AddRoundedRectangle(-0.40 * s, -0.22 * s, 0.54 * s, 0.44 * s, 0.08 * s);
        gc->StrokePath(body);
        gc->StrokePath(Polyline(gc, {P(0.14, -0.06), P(0.40, -0.20), P(0.40, 0.20), P(0.14, 0.06)}, true));
        if (id == IconId::CameraOff)
            gc->StrokeLine(-0.36 * s, -0.36 * s, 0.36 * s, 0.36 * s);
        break;
    }

    case IconId::Sidebar: {   // a window with a panel down its right side
        wxGraphicsPath frame = gc->CreatePath();
        frame.AddRoundedRectangle(-0.38 * s, -0.30 * s, 0.76 * s, 0.60 * s, 0.08 * s);
        gc->StrokePath(frame);
        gc->StrokeLine(0.12 * s, -0.30 * s, 0.12 * s, 0.30 * s);
        break;
    }

    case IconId::Chevron:   // pointing down
        gc->StrokePath(Polyline(gc, {P(-0.26, -0.12), P(0, 0.14), P(0.26, -0.12)}, false));
        break;
    }
    gc->PopState();
}
