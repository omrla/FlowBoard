#include "Board.h"

#include "BoardIcons.h"
#include "Chrome.h"
#include "HandTracker.h"
#include "PdfWriter.h"

#include <wx/dcbuffer.h>
#include <wx/image.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMouseId = -1;
constexpr double kTwoHandWindow = 0.35;   // s: a second pinch this soon after the first = zoom
constexpr double kToastSeconds = 1.6;
constexpr double kVanishHold = 1.2;       // s: vanishing ink stays this long...
constexpr double kVanishFade = 0.8;       // ...then fades out over this long
constexpr double kHuePerUnit = 0.6;       // rainbow ink: degrees of hue per world unit along a stroke
constexpr double kRainbowRun = 6.0;       // ...changing colour every this many world units
constexpr double kGlowWidth = 3.2;        // neon glow: outer haze, as a multiple of the stroke width
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 8.0;
constexpr size_t kMaxUndo = 200;

const wxColour kPalette[] = {
    wxColour(30, 32, 36),     // ink: follows the theme
    wxColour(229, 57, 53),    // red
    wxColour(251, 140, 0),    // orange
    wxColour(253, 216, 53),   // yellow
    wxColour(67, 160, 71),    // green
    wxColour(30, 136, 229),   // blue
    wxColour(142, 36, 170),   // purple
    wxColour(236, 64, 122),   // pink
};
constexpr int kColorCount = int(std::size(kPalette));
const double kSizes[] = {2.0, 4.0, 8.0, 14.0};
constexpr int kSizeCount = int(std::size(kSizes));

using chrome::kAccent;
const wxColour kPanelBg(28, 30, 36, 232);
const wxColour kIconColour(232, 234, 237);

// Same order as the Tool enum.
const IconId kToolIcons[] = {IconId::Pen, IconId::Highlighter, IconId::Eraser, IconId::Line,
                             IconId::Rect, IconId::Ellipse, IconId::Pan};
constexpr int kToolCount = int(std::size(kToolIcons));
// Same order as Board::Action.
const IconId kActionIcons[] = {IconId::Undo, IconId::Redo, IconId::Clear, IconId::Save};
constexpr int kActionCount = int(std::size(kActionIcons));

double Dist(const wxPoint2DDouble& a, const wxPoint2DDouble& b)
{
    return std::hypot(a.m_x - b.m_x, a.m_y - b.m_y);
}

wxPoint2DDouble Mid(const wxPoint2DDouble& a, const wxPoint2DDouble& b)
{
    return wxPoint2DDouble((a.m_x + b.m_x) / 2, (a.m_y + b.m_y) / 2);
}

double DistToSegment(const wxPoint2DDouble& p, const wxPoint2DDouble& a, const wxPoint2DDouble& b)
{
    const double dx = b.m_x - a.m_x, dy = b.m_y - a.m_y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.m_x - a.m_x) * dx + (p.m_y - a.m_y) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.m_x - (a.m_x + t * dx), p.m_y - (a.m_y + t * dy));
}

bool IsShape(Tool t)
{
    return t == Tool::Line || t == Tool::Rect || t == Tool::Ellipse;
}

wxColour WithAlpha(const wxColour& c, int alpha)
{
    return wxColour(c.Red(), c.Green(), c.Blue(), static_cast<unsigned char>(std::clamp(alpha, 0, 255)));
}

// A vivid colour from the hue circle (degrees), for rainbow ink.
wxColour Rainbow(double hue, int alpha)
{
    const wxImage::RGBValue rgb = wxImage::HSVtoRGB(wxImage::HSVValue(std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 360.0, 0.85, 0.95));
    return wxColour(rgb.red, rgb.green, rgb.blue, static_cast<unsigned char>(std::clamp(alpha, 0, 255)));
}

// Blends a colour toward white, keeping its alpha (the hot core of neon ink).
wxColour Lighter(const wxColour& c, double amount)
{
    auto mix = [&](unsigned char v) { return static_cast<unsigned char>(v + (255 - v) * amount); };
    return wxColour(mix(c.Red()), mix(c.Green()), mix(c.Blue()), c.Alpha());
}

std::vector<wxPoint2DDouble> MirroredX(std::vector<wxPoint2DDouble> pts, double axis)
{
    for (wxPoint2DDouble& q : pts)
        q.m_x = 2 * axis - q.m_x;
    return pts;
}

// Inserts points so neighbours are at most `step` apart (keeps the eraser exact).
std::vector<wxPoint2DDouble> Densify(const std::vector<wxPoint2DDouble>& pts, double step)
{
    if (pts.size() < 2)
        return pts;
    std::vector<wxPoint2DDouble> out{pts.front()};
    for (size_t i = 1; i < pts.size(); ++i) {
        const wxPoint2DDouble& a = pts[i - 1];
        const wxPoint2DDouble& b = pts[i];
        const int n = std::max(1, int(std::ceil(Dist(a, b) / step)));
        for (int k = 1; k <= n; ++k) {
            const double t = double(k) / n;
            out.emplace_back(a.m_x + (b.m_x - a.m_x) * t, a.m_y + (b.m_y - a.m_y) * t);
        }
    }
    return out;
}

// Samples the curve live strokes are drawn with (quadratic Béziers through the
// midpoints of the raw points), so committing a stroke doesn't change its shape.
std::vector<wxPoint2DDouble> SmoothCurve(const std::vector<wxPoint2DDouble>& pts, double step)
{
    if (pts.size() < 3)
        return Densify(pts, step);
    std::vector<wxPoint2DDouble> out{pts.front()};
    wxPoint2DDouble start = pts.front();
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const wxPoint2DDouble ctrl = pts[i];
        const wxPoint2DDouble end = Mid(pts[i], pts[i + 1]);
        const int n = std::max(1, int(std::ceil((Dist(start, ctrl) + Dist(ctrl, end)) / step)));
        for (int k = 1; k <= n; ++k) {
            const double t = double(k) / n, u = 1.0 - t;
            out.emplace_back(u * u * start.m_x + 2 * u * t * ctrl.m_x + t * t * end.m_x,
                             u * u * start.m_y + 2 * u * t * ctrl.m_y + t * t * end.m_y);
        }
        start = end;
    }
    const std::vector<wxPoint2DDouble> tail = Densify({start, pts.back()}, step);
    out.insert(out.end(), tail.begin() + 1, tail.end());
    return out;
}

std::vector<wxPoint2DDouble> ShapeOutline(Tool tool, const wxPoint2DDouble& a, const wxPoint2DDouble& b)
{
    switch (tool) {
    case Tool::Rect:
        return {a, wxPoint2DDouble(b.m_x, a.m_y), b, wxPoint2DDouble(a.m_x, b.m_y), a};
    case Tool::Ellipse: {
        const wxPoint2DDouble c = Mid(a, b);
        const double rx = std::abs(b.m_x - a.m_x) / 2, ry = std::abs(b.m_y - a.m_y) / 2;
        std::vector<wxPoint2DDouble> pts;
        for (int i = 0; i <= 96; ++i) {
            const double t = 2 * kPi * i / 96;
            pts.emplace_back(c.m_x + rx * std::cos(t), c.m_y + ry * std::sin(t));
        }
        return pts;
    }
    default:
        return {a, b};
    }
}

wxGraphicsPath WedgePath(wxGraphicsContext* gc, const wxPoint2DDouble& c, double a0, double a1,
                         double r0, double r1)
{
    const int steps = std::max(4, int((a1 - a0) / 0.06));
    wxGraphicsPath path = gc->CreatePath();
    for (int i = 0; i <= steps; ++i) {
        const double a = a0 + (a1 - a0) * i / steps;
        const wxPoint2DDouble p(c.m_x + r1 * std::cos(a), c.m_y + r1 * std::sin(a));
        if (i == 0) path.MoveToPoint(p);
        else path.AddLineToPoint(p);
    }
    for (int i = steps; i >= 0; --i) {
        const double a = a0 + (a1 - a0) * i / steps;
        path.AddLineToPoint(c.m_x + r0 * std::cos(a), c.m_y + r0 * std::sin(a));
    }
    path.CloseSubpath();
    return path;
}

} // namespace

void Stroke::UpdateBounds()
{
    if (pts.empty()) {
        bounds = RectD();
        return;
    }
    bounds = {pts[0].m_x, pts[0].m_y, pts[0].m_x, pts[0].m_y};
    for (const wxPoint2DDouble& p : pts) {
        bounds.x0 = std::min(bounds.x0, p.m_x);
        bounds.y0 = std::min(bounds.y0, p.m_y);
        bounds.x1 = std::max(bounds.x1, p.m_x);
        bounds.y1 = std::max(bounds.y1, p.m_y);
    }
    bounds = bounds.Inflated(width * (glow ? kGlowWidth : 1.0) / 2 + 1);
}

// ---------------------------------------------------------------------------
// Construction and public API

Board::Board(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxWANTS_CHARS | wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_ui = GetDPIScaleFactor();
    SetCursor(wxCursor(wxCURSOR_CROSS));

    Bind(wxEVT_PAINT, &Board::OnPaint, this);
    Bind(wxEVT_SIZE, &Board::OnSize, this);
    for (const auto& type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_LEFT_DCLICK, wxEVT_MIDDLE_DOWN,
                             wxEVT_MIDDLE_UP, wxEVT_MIDDLE_DCLICK, wxEVT_MOTION, wxEVT_LEAVE_WINDOW})
        Bind(type, &Board::OnMouse, this);
    Bind(wxEVT_RIGHT_DOWN, &Board::OnRightDown, this);
    Bind(wxEVT_MOUSEWHEEL, &Board::OnWheel, this);
    Bind(wxEVT_CHAR, &Board::OnChar, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        const auto it = m_pointers.find(kMouseId);
        if (it != m_pointers.end()) {
            EndPointer(it->second);
            m_pointers.erase(it);
        }
        UpdateNavigation();
        Refresh(false);
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        m_ui = GetDPIScaleFactor();
        LayoutDock();
        m_cacheDirty = true;
        Refresh(false);
        event.Skip();
    });
}

void Board::UpdateHands(const std::vector<PointerInput>& hands, double now)
{
    m_now = now;
    std::set<int> seen;
    for (const PointerInput& in : hands) {
        seen.insert(in.id);
        ProcessPointer(m_pointers[in.id], in, now);
    }
    for (auto it = m_pointers.begin(); it != m_pointers.end();) {
        if (it->first != kMouseId && !seen.count(it->first)) {
            EndPointer(it->second);
            it = m_pointers.erase(it);
        } else {
            ++it;
        }
    }
    UpdateNavigation();
    Refresh(false);
}

void Board::Tick(double now)
{
    m_now = now;
    // Vanishing ink: forget strokes that have faded out, keep repainting the rest.
    const size_t vanishing = m_vanishing.size();
    m_vanishing.erase(std::remove_if(m_vanishing.begin(), m_vanishing.end(),
                                     [&](const Vanishing& v) { return now - v.born > kVanishHold + kVanishFade; }),
                      m_vanishing.end());
    const bool animating = now - m_toastTime < kToastSeconds + 0.1 || !m_vanishing.empty() ||
                           m_vanishing.size() != vanishing;
    if (animating)
        Refresh(false);
}

void Board::Undo()
{
    if (m_undo.empty())
        return;
    m_redo.push_back(std::move(m_strokes));
    m_strokes = std::move(m_undo.back());
    m_undo.pop_back();
    StrokesChanged();
}

void Board::Redo()
{
    if (m_redo.empty())
        return;
    m_undo.push_back(std::move(m_strokes));
    m_strokes = std::move(m_redo.back());
    m_redo.pop_back();
    StrokesChanged();
}

void Board::ClearBoard()
{
    if (m_strokes.empty())
        return;
    PushUndo(m_strokes);
    m_strokes.clear();
    StrokesChanged();
}

void Board::ResetView()
{
    m_zoom = 1.0;
    m_origin = wxPoint2DDouble(0, 0);
    m_navIds.clear();
    m_cacheDirty = true;
    Notify();
    Refresh(false);
}

void Board::SetTool(Tool tool)
{
    m_tool = tool;
    Notify();
    Refresh(false);
}

void Board::SetColorIndex(int index)
{
    m_color = std::clamp(index, 0, kColorCount - 1);
    Notify();
    Refresh(false);
}

void Board::SetSizeIndex(int index)
{
    m_size = std::clamp(index, 0, kSizeCount - 1);
    Notify();
    Refresh(false);
}

void Board::SetDarkTheme(bool dark)
{
    m_dark = dark;
    m_cacheDirty = true;
    Refresh(false);
}

void Board::SetShowGrid(bool show)
{
    m_grid = show;
    m_cacheDirty = true;
    Refresh(false);
}

void Board::SetShowSkeleton(bool show)
{
    m_skeleton = show;
    Refresh(false);
}

void Board::SetInkEffects(const InkEffects& effects)
{
    m_fx = effects;
    Refresh(false);
}

void Board::SetHint(const wxString& hint)
{
    if (hint == m_hint)
        return;
    m_hint = hint;
    Refresh(false);
}

void Board::ShowToast(const wxString& text)
{
    m_toast = text;
    m_toastTime = m_now = HandTracker::Now();
    Refresh(false);
}

RectD Board::ExportArea() const
{
    if (m_strokes.empty())
        return VisibleWorld();
    RectD area = m_strokes.front()->bounds;
    for (const StrokePtr& s : m_strokes) {
        area.x0 = std::min(area.x0, s->bounds.x0);
        area.y0 = std::min(area.y0, s->bounds.y0);
        area.x1 = std::max(area.x1, s->bounds.x1);
        area.y1 = std::max(area.y1, s->bounds.y1);
    }
    return area.Inflated(40);
}

bool Board::ExportImage(const wxString& path, wxBitmapType type) const
{
    const RectD area = ExportArea();
    const double scale = std::min(2.0, 8000.0 / std::max(1.0, std::max(area.Width(), area.Height())));
    const int w = std::max(1, int(std::ceil(area.Width() * scale)));
    const int h = std::max(1, int(std::ceil(area.Height() * scale)));
    wxBitmap bitmap(w, h, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(Background()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->Scale(scale, scale);
            gc->Translate(-area.x0, -area.y0);
            for (const StrokePtr& s : m_strokes)
                DrawStroke(gc.get(), *s);
        }
    }
    wxImage image = bitmap.ConvertToImage();
    if (type == wxBITMAP_TYPE_JPEG)
        image.SetOption(wxIMAGE_OPTION_QUALITY, 92);
    return image.SaveFile(path, type);
}

bool Board::ExportPdf(const wxString& path) const
{
    // Vector output: every stroke becomes a PDF path, so it stays sharp at any zoom.
    const RectD area = ExportArea();
    const double k = 0.75;   // board pixels (96 dpi) -> PDF points (72 dpi)
    const double w = area.Width() * k;
    const double h = area.Height() * k;
    PdfWriter pdf(w, h);
    pdf.FillRect(0, 0, w, h, Background());
    // Board coordinates from here on: offset to the drawing, y pointing down.
    pdf.Transform(k, 0, 0, -k, -area.x0 * k, h + area.y0 * k);
    for (const StrokePtr& s : m_strokes) {
        for (const InkPass& pass : InkPasses(*s)) {
            if (pass.pts.empty())
                continue;
            pdf.BeginStroke(pass.colour, pass.width);
            pdf.MoveTo(pass.pts[0].m_x, pass.pts[0].m_y);
            for (size_t i = 1; i < pass.pts.size(); ++i)
                pdf.LineTo(pass.pts[i].m_x, pass.pts[i].m_y);
            if (pass.pts.size() == 1)
                pdf.LineTo(pass.pts[0].m_x, pass.pts[0].m_y);   // a dot
            pdf.EndStroke();
        }
    }
    return pdf.Save(path);
}

// ---------------------------------------------------------------------------
// Interaction

void Board::ProcessPointer(Pointer& p, const PointerInput& in, double now)
{
    const PointerInput prev = p.in;
    p.in = in;

    // Pinch with the palm facing you recenters the view (Quest's system gesture).
    if (in.system && !prev.system) {
        ResetView();
        ShowToast("View recentered");
    }
    // Thumb + ring finger tap opens the quick menu at the cursor, or closes it.
    if (in.ringTap) {
        if (m_menuOpen) m_menuOpen = false;
        else OpenMenu(in.pos);
    }

    // Thumb + pinky tap clears the board (one undo step brings it back).
    if (in.pinkyTap && !IsEmpty()) {
        ClearBoard();
        ShowToast(L"Board cleared  ·  tap thumb + middle finger to undo");
    }

    if (in.middleTap && CanUndo()) {
        Undo();
        ShowToast("Undo");
    }

    // A fist (or the middle mouse button) grabs the board.
    if (in.grab && p.mode == Mode::Idle) p.mode = Mode::Grab;
    else if (!in.grab && p.mode == Mode::Grab) p.mode = Mode::Idle;

    if (in.down && !prev.down) Press(p, in.isMouse ? in.pos : in.pressPos, now);
    else if (in.down && prev.down) Drag(p);
    else if (!in.down && prev.down) Release(p, in.pos);
}

void Board::Press(Pointer& p, const wxPoint2DDouble& pos, double now)
{
    if (p.mode != Mode::Idle)
        return;
    p.downTime = now;

    if (m_menuOpen) {
        const int item = MenuHitTest(pos);
        if (item >= 0)
            ActivateItem(m_menuItems[item].kind, m_menuItems[item].value);
        m_menuOpen = false;
        p.mode = Mode::MenuPress;
        return;
    }
    const int button = DockHitTest(pos);
    if (button >= 0) {
        p.mode = Mode::Button;
        p.button = button;
        return;
    }
    if (m_rail.Contains(pos) || m_bar.Contains(pos)) {
        p.mode = Mode::MenuPress;   // pinched the panel background: swallow it
        return;
    }
    if (m_tool == Tool::Pan) {
        p.mode = Mode::Navigate;
        return;
    }
    if (!p.in.isMouse) {
        // Two hands pinching together means "zoom", not two strokes: cancel the
        // stroke the first hand just started and navigate with both.
        for (auto& [id, other] : m_pointers) {
            if (&other == &p || other.in.isMouse)
                continue;
            const bool fresh = (other.mode == Mode::Draw || other.mode == Mode::Erase) &&
                               now - other.downTime < kTwoHandWindow;
            if (!fresh && other.mode != Mode::Navigate)
                continue;
            if (other.mode == Mode::Erase && other.erased && !m_undo.empty()) {
                m_strokes = std::move(m_undo.back());
                m_undo.pop_back();
                StrokesChanged();
            }
            other.stroke.reset();
            other.mode = Mode::Navigate;
            p.mode = Mode::Navigate;
            return;
        }
    }
    BeginStroke(p, pos);
}

void Board::Drag(Pointer& p)
{
    const wxPoint2DDouble w = ToWorld(p.in.pos);
    if (p.mode == Mode::Draw && p.stroke && !IsShape(p.tool)) {
        // Keep inking while the pinch looks like it's opening: mid-stroke that is
        // usually motion blur, and skipping points would cut fast strokes. Remember
        // where it began; if the release is confirmed, the stroke is cut back there so
        // the release wobble leaves no tail.
        if (!p.in.releasing) p.releaseMark = -1;
        else if (p.releaseMark < 0) p.releaseMark = int(p.stroke->pts.size());
        if (Dist(p.stroke->pts.back(), w) * m_zoom >= 1.0)
            p.stroke->pts.push_back(w);
        return;
    }
    if (p.in.releasing)
        return;   // the fingers are opening: ignore the release wobble
    if (p.mode == Mode::Draw && p.stroke) p.stroke->pts[1] = w;   // shape end point
    else if (p.mode == Mode::Erase) EraseAlong(p, w);
}

void Board::Release(Pointer& p, const wxPoint2DDouble& pos)
{
    if (p.mode == Mode::Draw) {
        if (p.stroke && p.releaseMark > 0 && p.releaseMark < int(p.stroke->pts.size()) && !IsShape(p.tool))
            p.stroke->pts.resize(p.releaseMark);
        CommitStroke(p);
    } else if (p.mode == Mode::Button && p.button >= 0 && p.button < int(m_dock.size())) {
        // Hands drift a little while releasing a pinch, so be lenient.
        const double slop = p.in.isMouse ? 0.0 : 24.0 * m_ui;
        const DockItem item = m_dock[p.button];
        if (item.rect.Inflated(slop).Contains(pos))
            ActivateItem(item.kind, item.value);
    }
    p.mode = p.in.grab ? Mode::Grab : Mode::Idle;
    p.stroke.reset();
    p.button = -1;
    p.releaseMark = -1;
}

void Board::EndPointer(Pointer& p)
{
    if (p.mode == Mode::Draw)
        CommitStroke(p);
    p.mode = Mode::Idle;
    p.stroke.reset();
}

void Board::UpdateNavigation()
{
    std::vector<const Pointer*> nav;
    for (const auto& [id, p] : m_pointers)
        if ((p.mode == Mode::Navigate || p.mode == Mode::Grab) && nav.size() < 2)
            nav.push_back(&p);
    if (nav.empty()) {
        m_navIds.clear();
        return;
    }
    auto navPos = [](const Pointer* p) { return p->mode == Mode::Grab ? p->in.grabPos : p->in.pos; };
    std::vector<int> ids;
    for (const Pointer* p : nav)
        ids.push_back(p->in.id);
    const wxPoint2DDouble focus = nav.size() == 1 ? navPos(nav[0]) : Mid(navPos(nav[0]), navPos(nav[1]));
    const double spread = nav.size() == 2 ? Dist(navPos(nav[0]), navPos(nav[1])) : 0.0;

    if (ids != m_navIds) {
        // The set of grabbing hands changed: anchor the board under them again.
        m_navIds = ids;
        m_navAnchor = ToWorld(focus);
        m_navSpread = std::max(spread, 1.0);
        m_navZoom = m_zoom;
        return;
    }
    double zoom = m_zoom;
    if (nav.size() == 2)
        zoom = std::clamp(m_navZoom * spread / m_navSpread, kMinZoom, kMaxZoom);
    // Keep the grabbed world point under the hand(s).
    const wxPoint2DDouble origin(m_navAnchor.m_x - focus.m_x / zoom, m_navAnchor.m_y - focus.m_y / zoom);
    if (zoom != m_zoom || origin.m_x != m_origin.m_x || origin.m_y != m_origin.m_y) {
        m_zoom = zoom;
        m_origin = origin;
        m_cacheDirty = true;
        Notify();
    }
}

void Board::BeginStroke(Pointer& p, const wxPoint2DDouble& screen)
{
    const wxPoint2DDouble w = ToWorld(screen);
    p.tool = m_tool;
    if (m_tool == Tool::Eraser) {
        p.mode = Mode::Erase;
        p.erased = false;
        p.lastWorld = w;
        EraseAlong(p, w);
        return;
    }
    auto s = std::make_shared<Stroke>();
    s->highlighter = m_tool == Tool::Highlighter;
    s->themeInk = m_color == 0;
    s->color = kPalette[m_color];
    s->width = (s->highlighter ? std::max(14.0 * m_ui, BrushPx() * 3.5) : BrushPx()) / m_zoom;
    s->rainbow = m_fx.rainbow;
    s->glow = m_fx.glow;
    s->hue = m_nextHue;
    m_nextHue = std::fmod(m_nextHue + 67.0, 360.0);
    s->pts.push_back(w);
    if (IsShape(m_tool))
        s->pts.push_back(w);   // [start, end]
    p.stroke = s;
    p.mode = Mode::Draw;
    p.releaseMark = -1;
    p.mirrorX = ToWorld(wxPoint2DDouble(GetClientSize().x / 2.0, 0)).m_x;   // the middle of the view
}

void Board::CommitStroke(Pointer& p)
{
    std::shared_ptr<Stroke> s = std::move(p.stroke);
    if (!s || s->pts.empty())
        return;
    const double step = 2.0 / m_zoom;   // a point every ~2 screen pixels
    if (IsShape(p.tool)) {
        if (Dist(s->pts[0], s->pts[1]) * m_zoom < 3.0)
            return;   // a tap, not a shape
        s->pts = Densify(ShapeOutline(p.tool, s->pts[0], s->pts[1]), step);
    } else {
        s->pts = SmoothCurve(s->pts, step);
    }
    s->UpdateBounds();
    std::vector<StrokePtr> made{s};
    if (m_fx.mirror) {
        auto twin = std::make_shared<Stroke>(*s);
        twin->pts = MirroredX(s->pts, p.mirrorX);
        twin->UpdateBounds();
        made.push_back(twin);
    }
    if (m_fx.vanish) {
        // Laser-pointer ink: shown for a moment, never part of the drawing.
        for (const StrokePtr& m : made)
            m_vanishing.push_back({m, m_now});
        Refresh(false);
        return;
    }
    PushUndo(m_strokes);
    m_strokes.insert(m_strokes.end(), made.begin(), made.end());
    ++m_revision;

    // Paint just the new stroke(s) onto the cached board instead of re-rendering it all.
    if (!m_cacheDirty && m_cache.IsOk()) {
        wxMemoryDC dc(m_cache);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            ApplyView(gc.get());
            for (const StrokePtr& m : made)
                DrawStroke(gc.get(), *m);
        }
    }
    Notify();
    Refresh(false);
}

void Board::EraseAlong(Pointer& p, const wxPoint2DDouble& world)
{
    std::vector<StrokePtr> before = m_strokes;
    if (EraseSegment(p.lastWorld, world)) {
        if (!p.erased) {
            PushUndo(std::move(before));   // one undo step per eraser drag
            p.erased = true;
        }
        StrokesChanged();
    }
    p.lastWorld = world;
}

bool Board::EraseSegment(const wxPoint2DDouble& a, const wxPoint2DDouble& b)
{
    const double radius = EraserRadiusPx() / m_zoom;
    const RectD sweep = RectD{std::min(a.m_x, b.m_x), std::min(a.m_y, b.m_y),
                              std::max(a.m_x, b.m_x), std::max(a.m_y, b.m_y)}.Inflated(radius);
    bool changed = false;
    std::vector<StrokePtr> result;
    result.reserve(m_strokes.size());
    for (const StrokePtr& s : m_strokes) {
        if (!s->bounds.Intersects(sweep)) {
            result.push_back(s);
            continue;
        }
        // Split the stroke around the points the eraser touched. Each piece keeps the
        // stroke's look; a rainbow piece starts at the hue it had at that point.
        const double reach = radius + s->width / 2;
        struct Piece {
            std::vector<wxPoint2DDouble> pts;
            double along = 0.0;   // distance along the stroke where the piece starts
        };
        std::vector<Piece> pieces(1);
        bool hit = false;
        double along = 0.0;
        for (size_t i = 0; i < s->pts.size(); ++i) {
            if (i > 0)
                along += Dist(s->pts[i - 1], s->pts[i]);
            if (DistToSegment(s->pts[i], a, b) <= reach) {
                hit = true;
                if (!pieces.back().pts.empty())
                    pieces.emplace_back();
            } else {
                if (pieces.back().pts.empty())
                    pieces.back().along = along;
                pieces.back().pts.push_back(s->pts[i]);
            }
        }
        if (!hit) {
            result.push_back(s);
            continue;
        }
        changed = true;
        for (Piece& piece : pieces) {
            if (piece.pts.empty() || (piece.pts.size() < 2 && s->pts.size() > 1))
                continue;   // drop crumbs
            auto ns = std::make_shared<Stroke>(*s);
            ns->pts = std::move(piece.pts);
            ns->hue = s->hue + piece.along * kHuePerUnit;
            ns->UpdateBounds();
            result.push_back(ns);
        }
    }
    if (changed)
        m_strokes = std::move(result);
    return changed;
}

void Board::PushUndo(std::vector<StrokePtr> snapshot)
{
    m_undo.push_back(std::move(snapshot));
    if (m_undo.size() > kMaxUndo)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void Board::StrokesChanged()
{
    ++m_revision;
    m_cacheDirty = true;
    Notify();
    Refresh(false);
}

void Board::ActivateItem(ItemKind kind, int value)
{
    switch (kind) {
    case ItemKind::Tool:
        SetTool(Tool(value));
        break;
    case ItemKind::Color:
        SetColorIndex(value);
        if (m_tool == Tool::Eraser || m_tool == Tool::Pan)
            SetTool(Tool::Pen);   // picking a colour means you want to draw
        break;
    case ItemKind::Size:
        SetSizeIndex(value);
        break;
    case ItemKind::Action:
        switch (value) {
        case ActUndo:
            if (CanUndo()) { Undo(); ShowToast("Undo"); }
            break;
        case ActRedo:
            if (CanRedo()) { Redo(); ShowToast("Redo"); }
            break;
        case ActClear:
            if (!IsEmpty()) { ClearBoard(); ShowToast("Board cleared (undo brings it back)"); }
            break;
        case ActSave:
            if (onQuickSave) onQuickSave();
            break;
        }
        break;
    }
}

void Board::Notify()
{
    if (onStateChanged)
        onStateChanged();
}

// ---------------------------------------------------------------------------
// Dock and radial menu

void Board::LayoutDock()
{
    m_dock.clear();
    m_separators.clear();
    const wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0)
        return;

    // Left rail: tools, then actions. Shrinks to fit short windows.
    const int railItems = kToolCount + kActionCount;
    double button = 54 * m_ui, gap = 6 * m_ui, pad = 10 * m_ui, sep = 18 * m_ui;
    const double railHeight = 2 * pad + railItems * button + (railItems - 2) * gap + sep;
    const double fit = std::clamp((sz.y - 24 * m_ui) / railHeight, 0.4, 1.0);
    button *= fit;
    gap *= fit;
    pad *= fit;
    sep *= fit;
    const double x = 16 * m_ui;
    double y = (sz.y - railHeight * fit) / 2;
    m_rail = {x, y, x + button + 2 * pad, y + railHeight * fit};
    y += pad;
    for (int i = 0; i < kToolCount; ++i) {
        m_dock.push_back({ItemKind::Tool, i, {x + pad, y, x + pad + button, y + button}});
        y += button + (i + 1 < kToolCount ? gap : 0);
    }
    m_separators.push_back({x + pad + button * 0.2, y + sep / 2, x + pad + button * 0.8, y + sep / 2});
    y += sep;
    for (int i = 0; i < kActionCount; ++i) {
        m_dock.push_back({ItemKind::Action, i, {x + pad, y, x + pad + button, y + button}});
        y += button + gap;
    }

    // Bottom bar: colours, then brush sizes.
    const int barItems = kColorCount + kSizeCount;
    double cell = 46 * m_ui, bgap = 4 * m_ui, bpad = 8 * m_ui, bsep = 18 * m_ui;
    const double barWidth = 2 * bpad + barItems * cell + (barItems - 2) * bgap + bsep;
    const double left = m_rail.x1 + 16 * m_ui;
    const double bfit = std::clamp((sz.x - left - 16 * m_ui) / barWidth, 0.4, 1.0);
    cell *= bfit;
    bgap *= bfit;
    bpad *= bfit;
    bsep *= bfit;
    double bx = std::max(left, (sz.x - barWidth * bfit) / 2);
    const double by = sz.y - 16 * m_ui - (cell + 2 * bpad);
    m_bar = {bx, by, bx + barWidth * bfit, by + cell + 2 * bpad};
    bx += bpad;
    const double cy = by + bpad;
    for (int i = 0; i < kColorCount; ++i) {
        m_dock.push_back({ItemKind::Color, i, {bx, cy, bx + cell, cy + cell}});
        bx += cell + (i + 1 < kColorCount ? bgap : 0);
    }
    m_separators.push_back({bx + bsep / 2, cy + cell * 0.2, bx + bsep / 2, cy + cell * 0.8});
    bx += bsep;
    for (int i = 0; i < kSizeCount; ++i) {
        m_dock.push_back({ItemKind::Size, i, {bx, cy, bx + cell, cy + cell}});
        bx += cell + bgap;
    }
}

int Board::DockHitTest(const wxPoint2DDouble& pt) const
{
    for (size_t i = 0; i < m_dock.size(); ++i)
        if (m_dock[i].rect.Contains(pt))
            return int(i);
    return -1;
}

void Board::OpenMenu(const wxPoint2DDouble& at)
{
    const double r0 = 44 * m_ui, r1 = 108 * m_ui, r2 = 166 * m_ui;
    const wxSize sz = GetClientSize();
    const double margin = r2 + 10 * m_ui;
    m_menuCenter = wxPoint2DDouble(std::clamp(at.m_x, margin, std::max(margin, sz.x - margin)),
                                   std::clamp(at.m_y, margin, std::max(margin, sz.y - margin)));
    m_menuItems.clear();
    const double toolSpan = 2 * kPi / kToolCount;
    for (int i = 0; i < kToolCount; ++i) {
        const double a = -kPi / 2 + i * toolSpan;
        m_menuItems.push_back({ItemKind::Tool, i, a - toolSpan / 2, a + toolSpan / 2, r0, r1});
    }
    const double colorSpan = 2 * kPi / kColorCount;
    for (int i = 0; i < kColorCount; ++i) {
        const double a = -kPi / 2 + i * colorSpan;
        m_menuItems.push_back({ItemKind::Color, i, a - colorSpan / 2, a + colorSpan / 2,
                               r1 + 6 * m_ui, r2});
    }
    m_menuOpen = true;
    Refresh(false);
}

int Board::MenuHitTest(const wxPoint2DDouble& pt) const
{
    if (!m_menuOpen)
        return -1;
    const double dx = pt.m_x - m_menuCenter.m_x, dy = pt.m_y - m_menuCenter.m_y;
    const double d = std::hypot(dx, dy);
    const double angle = std::atan2(dy, dx);
    for (size_t i = 0; i < m_menuItems.size(); ++i) {
        const MenuItem& m = m_menuItems[i];
        const double outer = m.r1 + (m.kind == ItemKind::Color ? 16 * m_ui : 0.0);
        if (d < m.r0 || d > outer)
            continue;
        double rel = std::fmod(angle - m.a0, 2 * kPi);
        if (rel < 0)
            rel += 2 * kPi;
        if (rel <= m.a1 - m.a0)
            return int(i);
    }
    return -1;
}

bool Board::IsOverUi(const wxPoint2DDouble& pt) const
{
    if (m_menuOpen)
        return !m_menuItems.empty() && Dist(pt, m_menuCenter) <= m_menuItems.back().r1;
    return m_rail.Contains(pt) || m_bar.Contains(pt);
}

// ---------------------------------------------------------------------------
// View helpers

wxPoint2DDouble Board::ToWorld(const wxPoint2DDouble& screen) const
{
    return wxPoint2DDouble(m_origin.m_x + screen.m_x / m_zoom, m_origin.m_y + screen.m_y / m_zoom);
}

RectD Board::VisibleWorld() const
{
    const wxSize sz = GetClientSize();
    return {m_origin.m_x, m_origin.m_y, m_origin.m_x + sz.x / m_zoom, m_origin.m_y + sz.y / m_zoom};
}

void Board::ApplyView(wxGraphicsContext* gc) const
{
    gc->Scale(m_zoom, m_zoom);
    gc->Translate(-m_origin.m_x, -m_origin.m_y);
}

double Board::BrushPx() const
{
    return kSizes[m_size] * m_ui;
}

double Board::EraserRadiusPx() const
{
    return std::max(10.0 * m_ui, BrushPx() * 3.0);
}

wxColour Board::Background() const
{
    return m_dark ? wxColour(30, 33, 38) : wxColour(250, 250, 248);
}

wxColour Board::PanelColour() const
{
    // A step lighter than the dark board so the toolbars don't melt into it.
    return m_dark ? wxColour(46, 50, 58, 240) : kPanelBg;
}

wxColour Board::InkColour() const
{
    return m_dark ? wxColour(240, 240, 240) : kPalette[0];
}

wxColour Board::SwatchColour(int index) const
{
    return index == 0 ? InkColour() : kPalette[index];
}

wxColour Board::StrokeColour(const Stroke& s) const
{
    const wxColour c = s.themeInk ? InkColour() : s.color;
    return s.highlighter ? WithAlpha(c, m_dark ? 110 : 90) : c;
}

// ---------------------------------------------------------------------------
// Rendering

void Board::OnSize(wxSizeEvent& event)
{
    LayoutDock();
    m_cacheDirty = true;
    Refresh(false);
    event.Skip();
}

void Board::RebuildCache()
{
    const wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0)
        return;
    if (!m_cache.IsOk() || m_cache.GetSize() != sz)
        m_cache.Create(sz, 24);
    wxMemoryDC dc(m_cache);
    dc.SetBackground(wxBrush(Background()));
    dc.Clear();
    if (m_grid)
        DrawGrid(dc);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        ApplyView(gc.get());
        const RectD visible = VisibleWorld();
        for (const StrokePtr& s : m_strokes)
            if (s->bounds.Intersects(visible))
                DrawStroke(gc.get(), *s);
    }
    m_cacheDirty = false;
}

void Board::DrawGrid(wxDC& dc) const
{
    // Dot grid on multiples of 32 world units, thinned/densified to stay readable.
    double step = 32.0 * m_zoom;
    while (step < 18.0) step *= 2;
    while (step > 72.0) step /= 2;
    const wxSize sz = GetClientSize();
    double ox = std::fmod(-m_origin.m_x * m_zoom, step);
    double oy = std::fmod(-m_origin.m_y * m_zoom, step);
    if (ox < 0) ox += step;
    if (oy < 0) oy += step;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(m_dark ? wxColour(60, 65, 74) : wxColour(208, 211, 217)));
    const int d = std::max(2, int(std::lround(2 * m_ui)));
    for (double y = oy; y < sz.y; y += step)
        for (double x = ox; x < sz.x; x += step)
            dc.DrawRectangle(int(x) - d / 2, int(y) - d / 2, d, d);
}

std::vector<Board::InkPass> Board::InkPasses(const Stroke& s, double opacity) const
{
    const wxColour base = StrokeColour(s);
    // The stroke as runs of one colour: the whole stroke, or for rainbow ink short runs
    // stepping round the hue circle with the distance travelled.
    std::vector<std::pair<wxColour, std::vector<wxPoint2DDouble>>> runs;
    if (!s.rainbow) {
        runs.push_back({base, s.pts});
    } else if (s.pts.size() < 2) {
        runs.push_back({Rainbow(s.hue, base.Alpha()), s.pts});
    } else {
        double along = 0.0, runStart = 0.0;
        std::vector<wxPoint2DDouble> run{s.pts[0]};
        for (size_t i = 1; i < s.pts.size(); ++i) {
            along += Dist(s.pts[i - 1], s.pts[i]);
            run.push_back(s.pts[i]);
            if (along - runStart >= kRainbowRun || i + 1 == s.pts.size()) {
                runs.push_back({Rainbow(s.hue + (runStart + along) / 2 * kHuePerUnit, base.Alpha()), run});
                run = {s.pts[i]};
                runStart = along;
            }
        }
    }
    std::vector<InkPass> passes;
    auto layer = [&](double width, double alpha, double whiten) {
        for (const auto& [colour, pts] : runs)
            passes.push_back({WithAlpha(Lighter(colour, whiten), int(colour.Alpha() * alpha * opacity)), s.width * width, pts});
    };
    if (s.glow) {   // neon: a wide faint haze, a brighter halo, the ink, then a hot core
        layer(kGlowWidth, 0.14, 0.0);
        layer(1.9, 0.32, 0.0);
    }
    layer(1.0, 1.0, 0.0);
    if (s.glow)
        layer(0.4, 0.9, 0.65);
    return passes;
}

void Board::DrawStroke(wxGraphicsContext* gc, const Stroke& s, double opacity) const
{
    if (s.pts.empty())
        return;
    for (const InkPass& pass : InkPasses(s, opacity)) {
        if (pass.pts.size() == 1) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(pass.colour));
            gc->DrawEllipse(pass.pts[0].m_x - pass.width / 2, pass.pts[0].m_y - pass.width / 2, pass.width, pass.width);
            continue;
        }
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(pass.colour, pass.width).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
        wxGraphicsPath path = gc->CreatePath();
        path.MoveToPoint(pass.pts[0]);
        for (size_t i = 1; i < pass.pts.size(); ++i)
            path.AddLineToPoint(pass.pts[i]);
        gc->StrokePath(path);
    }
}

void Board::DrawLiveStroke(wxGraphicsContext* gc, const Pointer& p) const
{
    // Drawn exactly as it will be committed: the shape outline, or the smoothed curve.
    Stroke live = *p.stroke;
    live.pts = IsShape(p.tool) ? ShapeOutline(p.tool, live.pts[0], live.pts[1]) : SmoothCurve(live.pts, 2.0 / m_zoom);
    DrawStroke(gc, live);
    if (m_fx.mirror) {
        live.pts = MirroredX(live.pts, p.mirrorX);
        DrawStroke(gc, live);
    }
}

void Board::DrawPanel(wxGraphicsContext* gc, const RectD& r, double radius) const
{
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(wxColour(0, 0, 0, 16)));
    for (int i = 3; i >= 1; --i) {   // soft shadow
        const double g = i * 2.0 * m_ui;
        gc->DrawRoundedRectangle(r.x0 - g, r.y0 - g + 3 * m_ui, r.Width() + 2 * g, r.Height() + 2 * g, radius + g);
    }
    gc->SetBrush(wxBrush(PanelColour()));
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, m_dark ? 40 : 22), 1.0)));
    gc->DrawRoundedRectangle(r.x0, r.y0, r.Width(), r.Height(), radius);
}

void Board::DrawDock(wxGraphicsContext* gc) const
{
    DrawPanel(gc, m_rail, 20 * m_ui);
    DrawPanel(gc, m_bar, m_bar.Height() / 2);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, 40), 1.5 * m_ui)));
    for (const RectD& s : m_separators)
        gc->StrokeLine(s.x0, s.y0, s.x1, s.y1);

    std::set<int> hot, pressed;
    if (!m_menuOpen) {
        for (const auto& [id, p] : m_pointers) {
            const int h = DockHitTest(p.in.pos);
            if (h >= 0) hot.insert(h);
            if (p.mode == Mode::Button) pressed.insert(p.button);
        }
    }

    for (size_t i = 0; i < m_dock.size(); ++i) {
        const DockItem& item = m_dock[i];
        const RectD& r = item.rect;
        const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2, w = r.Width();
        const bool isHot = hot.count(int(i)) > 0;
        const double shrink = pressed.count(int(i)) ? 0.9 : 1.0;

        switch (item.kind) {
        case ItemKind::Tool:
        case ItemKind::Action: {
            const bool selected = item.kind == ItemKind::Tool && item.value == int(m_tool);
            bool enabled = true;
            if (item.kind == ItemKind::Action) {
                if (item.value == ActUndo) enabled = CanUndo();
                else if (item.value == ActRedo) enabled = CanRedo();
                else if (item.value == ActClear) enabled = !IsEmpty();
            }
            if (selected || isHot) {
                const double bw = w * shrink;
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(selected ? kAccent : wxColour(255, 255, 255, 34)));
                gc->DrawRoundedRectangle(cx - bw / 2, cy - bw / 2, bw, bw, 14 * m_ui);
            }
            const IconId icon = item.kind == ItemKind::Tool ? kToolIcons[item.value] : kActionIcons[item.value];
            const wxColour colour = !enabled ? wxColour(255, 255, 255, 70) : selected ? *wxWHITE : kIconColour;
            DrawIcon(gc, icon, cx, cy, w * 0.52 * shrink, colour);
            break;
        }
        case ItemKind::Color: {
            const double rad = w * (isHot ? 0.36 : 0.31) * shrink;
            if (item.value == m_color) {
                const double ring = rad + 5 * m_ui;
                gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(*wxWHITE, 2.5 * m_ui)));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawEllipse(cx - ring, cy - ring, 2 * ring, 2 * ring);
            }
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, 90), 1.0 * m_ui)));
            gc->SetBrush(wxBrush(SwatchColour(item.value)));
            gc->DrawEllipse(cx - rad, cy - rad, 2 * rad, 2 * rad);
            break;
        }
        case ItemKind::Size: {
            const bool selected = item.value == m_size;
            if (selected || isHot) {
                const double bw = w * 0.86 * shrink;
                gc->SetPen(selected ? gc->CreatePen(wxGraphicsPenInfo(kAccent, 2.0 * m_ui)) : wxGraphicsPen());
                gc->SetBrush(wxBrush(wxColour(255, 255, 255, selected ? 30 : 24)));
                gc->DrawRoundedRectangle(cx - bw / 2, cy - bw / 2, bw, bw, bw / 2);
            }
            const double d = std::min(std::max(4.0, kSizes[item.value] * 1.3) * m_ui, w * 0.55) * shrink;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(*wxWHITE));
            gc->DrawEllipse(cx - d / 2, cy - d / 2, d, d);
            break;
        }
        }
    }
}

void Board::DrawMenu(wxGraphicsContext* gc) const
{
    if (m_menuItems.empty())
        return;
    const wxPoint2DDouble c = m_menuCenter;
    std::set<int> hot;
    for (const auto& [id, p] : m_pointers) {
        const int h = MenuHitTest(p.in.pos);
        if (h >= 0) hot.insert(h);
    }

    const double outer = m_menuItems.back().r1;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(wxColour(0, 0, 0, 45)));
    const double shadow = outer + 12 * m_ui;
    gc->DrawEllipse(c.m_x - shadow, c.m_y - shadow + 4 * m_ui, 2 * shadow, 2 * shadow);
    gc->SetBrush(wxBrush(PanelColour()));
    const double disc = outer + 8 * m_ui;
    gc->DrawEllipse(c.m_x - disc, c.m_y - disc, 2 * disc, 2 * disc);

    const double gap = 0.025;
    for (size_t i = 0; i < m_menuItems.size(); ++i) {
        const MenuItem& m = m_menuItems[i];
        const bool isHot = hot.count(int(i)) > 0;
        const double mid = (m.a0 + m.a1) / 2;
        if (m.kind == ItemKind::Tool) {
            const bool selected = m.value == int(m_tool);
            gc->SetBrush(wxBrush(selected ? kAccent : wxColour(255, 255, 255, isHot ? 50 : 14)));
            gc->FillPath(WedgePath(gc, c, m.a0 + gap, m.a1 - gap, m.r0 + 2 * m_ui, m.r1));
            const double r = (m.r0 + m.r1) / 2;
            DrawIcon(gc, kToolIcons[m.value], c.m_x + r * std::cos(mid), c.m_y + r * std::sin(mid),
                     30 * m_ui, *wxWHITE);
        } else {
            const double r1 = m.r1 + (isHot ? 10 * m_ui : 0.0);
            gc->SetBrush(wxBrush(SwatchColour(m.value)));
            gc->FillPath(WedgePath(gc, c, m.a0 + gap, m.a1 - gap, m.r0, r1));
            if (m.value == m_color) {
                const double r = (m.r0 + r1) / 2, d = 6 * m_ui;
                gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(0, 0, 0, 120), 1.5 * m_ui)));
                gc->SetBrush(wxBrush(*wxWHITE));
                gc->DrawEllipse(c.m_x + r * std::cos(mid) - d, c.m_y + r * std::sin(mid) - d, 2 * d, 2 * d);
                gc->SetPen(*wxTRANSPARENT_PEN);
            }
        }
    }
    const double inner = m_menuItems.front().r0 - 4 * m_ui;
    gc->SetBrush(wxBrush(wxColour(255, 255, 255, 20)));
    gc->DrawEllipse(c.m_x - inner, c.m_y - inner, 2 * inner, 2 * inner);
    DrawIcon(gc, IconId::Close, c.m_x, c.m_y, 26 * m_ui, kIconColour);
}

void Board::DrawSkeleton(wxGraphicsContext* gc, const PointerInput& in) const
{
    // A translucent "ghost hand", like the hands Quest draws in passthrough.
    const auto& s = in.skeleton;
    const wxColour base = m_dark ? wxColour(150, 190, 255) : wxColour(50, 100, 210);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(WithAlpha(base, 30), 11 * m_ui).Cap(wxCAP_ROUND)));
    for (const auto& b : kHandBones)
        gc->StrokeLine(s[b[0]].m_x, s[b[0]].m_y, s[b[1]].m_x, s[b[1]].m_y);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(WithAlpha(base, 95), 2.5 * m_ui).Cap(wxCAP_ROUND)));
    for (const auto& b : kHandBones)
        gc->StrokeLine(s[b[0]].m_x, s[b[0]].m_y, s[b[1]].m_x, s[b[1]].m_y);
    gc->SetPen(*wxTRANSPARENT_PEN);
    for (int i = 0; i < LANDMARK_COUNT; ++i) {
        const bool pinchTip = i == THUMB_TIP || i == INDEX_TIP;
        const double r = (pinchTip ? 4.5 : 3.0) * m_ui;
        gc->SetBrush(wxBrush(pinchTip ? WithAlpha(kAccent, int(90 + 140 * in.strength)) : WithAlpha(base, 140)));
        gc->DrawEllipse(s[i].m_x - r, s[i].m_y - r, 2 * r, 2 * r);
    }
}

void Board::DrawPointer(wxGraphicsContext* gc, const Pointer& p) const
{
    const PointerInput& in = p.in;
    const double x = in.pos.m_x, y = in.pos.m_y;
    const bool overUi = IsOverUi(in.pos);

    if (m_tool == Tool::Eraser && !overUi && (p.mode == Mode::Idle || p.mode == Mode::Erase)) {
        const double r = EraserRadiusPx();
        gc->SetBrush(wxBrush(m_dark ? wxColour(255, 255, 255, 25) : wxColour(0, 0, 0, 16)));
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(m_dark ? wxColour(255, 255, 255, 150) : wxColour(60, 60, 60, 160), 1.5 * m_ui)));
        gc->DrawEllipse(x - r, y - r, 2 * r, 2 * r);
    }
    if (in.isMouse)
        return;   // the mouse keeps the system cursor

    if (p.mode == Mode::Grab || p.mode == Mode::Navigate) {
        const wxPoint2DDouble g = p.mode == Mode::Grab ? in.grabPos : in.pos;
        const double r = 20 * m_ui;
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, 120), 1.5 * m_ui)));
        gc->SetBrush(wxBrush(PanelColour()));
        gc->DrawEllipse(g.m_x - r, g.m_y - r, 2 * r, 2 * r);
        DrawIcon(gc, IconId::Pan, g.m_x, g.m_y, 26 * m_ui, *wxWHITE);
        return;
    }

    if (in.palmFacing && !in.down) {
        // Palm turned toward you: show the recenter badge (pinch to recenter the view).
        const double r = 18 * m_ui;
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(in.system ? kAccent : wxColour(255, 255, 255, 120), 1.5 * m_ui)));
        gc->SetBrush(wxBrush(in.system ? kAccent : PanelColour()));
        gc->DrawEllipse(x - r, y - r, 2 * r, 2 * r);
        DrawIcon(gc, IconId::Recenter, x, y, 24 * m_ui, *wxWHITE);
        return;
    }

    // Quest-style cursor: a ring that tightens as the pinch closes.
    const double ring = (7.0 + 13.0 * (1.0 - in.strength)) * m_ui;
    const wxColour ringColour = overUi ? kAccent : *wxWHITE;
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(0, 0, 0, 90), 4.5 * m_ui)));
    gc->DrawEllipse(x - ring, y - ring, 2 * ring, 2 * ring);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(ringColour, 2.2 * m_ui)));
    if (in.down)
        gc->SetBrush(wxBrush(WithAlpha(ringColour, 200)));
    gc->DrawEllipse(x - ring, y - ring, 2 * ring, 2 * ring);

    const bool inking = !overUi && m_tool != Tool::Eraser && m_tool != Tool::Pan;
    const double dot = inking ? std::clamp(BrushPx() / 2, 2.5 * m_ui, ring - 2 * m_ui) : 2.5 * m_ui;
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, 230), 1.2 * m_ui)));
    gc->SetBrush(wxBrush(inking ? SwatchColour(m_color) : ringColour));
    gc->DrawEllipse(x - dot, y - dot, 2 * dot, 2 * dot);
}

void Board::DrawPill(wxGraphicsContext* gc, const wxString& text, double cx, double y, double alpha) const
{
    gc->SetFont(GetFont().Larger(), wxColour(255, 255, 255, int(255 * alpha)));
    double tw = 0, th = 0;
    gc->GetTextExtent(text, &tw, &th);
    const double px = 16 * m_ui, py = 7 * m_ui, h = th + 2 * py;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(wxColour(28, 30, 36, int(225 * alpha))));
    gc->DrawRoundedRectangle(cx - tw / 2 - px, y, tw + 2 * px, h, h / 2);
    gc->DrawText(text, cx - tw / 2, y + py);
}

void Board::OnPaint(wxPaintEvent&)
{
    wxBufferedPaintDC dc(this);
    Render(dc);
}

void Board::Render(wxMemoryDC& dc)
{
    const wxSize sz = GetClientSize();
    if (m_cacheDirty || !m_cache.IsOk() || m_cache.GetSize() != sz)
        RebuildCache();
    if (m_cache.IsOk())
        dc.DrawBitmap(m_cache, 0, 0);

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    if (m_fx.mirror) {   // the mirror line down the middle of the view
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(m_dark ? wxColour(255, 255, 255, 46) : wxColour(0, 0, 0, 34),
                                                   1.5 * m_ui).Style(wxPENSTYLE_SHORT_DASH)));
        gc->StrokeLine(sz.x / 2.0, 0, sz.x / 2.0, sz.y);
    }
    gc->PushState();
    ApplyView(gc.get());
    for (const Vanishing& v : m_vanishing)
        DrawStroke(gc.get(), *v.stroke, std::clamp(1.0 - (m_now - v.born - kVanishHold) / kVanishFade, 0.0, 1.0));
    for (const auto& [id, p] : m_pointers)
        if (p.mode == Mode::Draw && p.stroke)
            DrawLiveStroke(gc.get(), p);
    gc->PopState();

    DrawDock(gc.get());
    if (m_menuOpen)
        DrawMenu(gc.get());
    if (m_skeleton)
        for (const auto& [id, p] : m_pointers)
            if (p.in.hasSkeleton)
                DrawSkeleton(gc.get(), p.in);
    for (const auto& [id, p] : m_pointers)
        DrawPointer(gc.get(), p);

    const double cx = sz.x / 2.0;
    double y = 14 * m_ui;
    if (!m_hint.empty()) {
        DrawPill(gc.get(), m_hint, cx, y, 1.0);
        y += 44 * m_ui;
    }
    const double age = m_now - m_toastTime;
    if (age >= 0 && age < kToastSeconds)
        DrawPill(gc.get(), m_toast, cx, y, std::clamp((kToastSeconds - age) / 0.4, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// Mouse and keyboard

void Board::OnMouse(wxMouseEvent& event)
{
    if (event.ButtonDown() || event.ButtonDClick()) {
        SetFocus();
        if (!HasCapture())
            CaptureMouse();
    }
    const bool left = event.LeftIsDown();
    const bool middle = event.MiddleIsDown();
    if (event.ButtonUp() && !left && !middle && HasCapture())
        ReleaseMouse();

    if (event.Leaving() && !left && !middle) {
        const auto it = m_pointers.find(kMouseId);
        if (it != m_pointers.end() && it->second.mode == Mode::Idle)
            m_pointers.erase(it);
        Refresh(false);
        return;
    }

    PointerInput in;
    in.id = kMouseId;
    in.isMouse = true;
    in.pos = in.pressPos = in.grabPos = wxPoint2DDouble(event.GetX(), event.GetY());
    in.down = left;
    in.grab = middle;
    m_now = HandTracker::Now();
    ProcessPointer(m_pointers[kMouseId], in, m_now);
    UpdateNavigation();
    Refresh(false);
}

void Board::OnRightDown(wxMouseEvent& event)
{
    SetFocus();
    if (m_menuOpen) m_menuOpen = false;
    else OpenMenu(wxPoint2DDouble(event.GetX(), event.GetY()));
    Refresh(false);
}

void Board::OnWheel(wxMouseEvent& event)
{
    if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL)
        return;
    const wxPoint2DDouble p(event.GetX(), event.GetY());
    const wxPoint2DDouble w = ToWorld(p);
    const double steps = double(event.GetWheelRotation()) / std::max(1, event.GetWheelDelta());
    m_zoom = std::clamp(m_zoom * std::pow(1.15, steps), kMinZoom, kMaxZoom);
    m_origin = wxPoint2DDouble(w.m_x - p.m_x / m_zoom, w.m_y - p.m_y / m_zoom);
    m_navIds.clear();   // re-anchor any hand navigation in progress
    m_cacheDirty = true;
    Notify();
    Refresh(false);
}

void Board::OnChar(wxKeyEvent& event)
{
    if (event.ControlDown() || event.AltDown()) {
        event.Skip();
        return;
    }
    const int key = event.GetUnicodeKey() != WXK_NONE ? int(event.GetUnicodeKey()) : event.GetKeyCode();
    switch (key) {
    case 'p': case 'P': SetTool(Tool::Pen); break;
    case 'h': case 'H': SetTool(Tool::Highlighter); break;
    case 'e': case 'E': SetTool(Tool::Eraser); break;
    case 'l': case 'L': SetTool(Tool::Line); break;
    case 'r': case 'R': SetTool(Tool::Rect); break;
    case 'o': case 'O': SetTool(Tool::Ellipse); break;
    case ' ': SetTool(Tool::Pan); break;
    case '[': SetSizeIndex(m_size - 1); break;
    case ']': SetSizeIndex(m_size + 1); break;
    case WXK_ESCAPE:
        m_menuOpen = false;
        Refresh(false);
        break;
    default:
        if (key >= '1' && key <= '8') SetColorIndex(key - '1');
        else event.Skip();
    }
}
