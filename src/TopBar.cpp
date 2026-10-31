#include "TopBar.h"

#include "AppIcon.h"
#include "BoardIcons.h"
#include "Chrome.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <cmath>
#include <memory>

namespace {

// The dropdown tabs, in the order of TopBar::Item.
const wchar_t* const kTabs[] = {L"File", L"View", L"Effects", L"Gestures", L"Tips"};
constexpr int kTabCount = 5;

wxColour StatusColour(TopBar::Status status)
{
    switch (status) {
    case TopBar::Status::Running: return chrome::kGood;
    case TopBar::Status::Starting: return chrome::kWarn;
    case TopBar::Status::Failed: return chrome::kBad;
    default: return chrome::kMuted;
    }
}

} // namespace

TopBar::TopBar(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      m_logoIcon(AppIconBundle())
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(-1, FromDIP(44)));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(this);
        Render(dc);
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Refresh(false);
        event.Skip();
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        SetMinSize(wxSize(-1, FromDIP(44)));
        event.Skip();
    });
    Bind(wxEVT_MOTION, &TopBar::OnMouse, this);
    Bind(wxEVT_LEFT_DOWN, &TopBar::OnMouse, this);
    Bind(wxEVT_LEFT_DCLICK, &TopBar::OnMouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &TopBar::OnMouse, this);
}

void TopBar::UpdateTooltip()
{
    // Follows the item under the mouse, and what it says (the camera button's changes).
    const wxString tip = Tooltip(m_hot);
    if (tip == GetToolTipText())
        return;
    if (tip.empty()) UnsetToolTip();
    else SetToolTip(tip);
}

void TopBar::LayoutItems(wxGraphicsContext* gc, int width, int height)
{
    double tw = 0, th = 0;
    const int itemH = FromDIP(30);
    const int y = (height - itemH) / 2;

    // Left: logo, name, then the tabs.
    int x = FromDIP(12);
    const int logo = FromDIP(28);
    m_logo = wxRect(x, (height - logo) / 2, logo, logo);
    x += logo + FromDIP(8);
    m_brandX = x;
    gc->SetFont(GetFont().Bold().Scaled(1.08f), chrome::kText);
    gc->GetTextExtent("FlowBoard", &tw, &th);
    x += int(std::ceil(tw)) + FromDIP(21);

    gc->SetFont(GetFont(), chrome::kText);
    for (int i = 0; i < kTabCount; ++i) {
        gc->GetTextExtent(kTabs[i], &tw, &th);
        const int w = FromDIP(10) + int(std::ceil(tw)) + FromDIP(22);   // label, then the chevron
        m_rects[size_t(i)] = wxRect(x, y, w, itemH);
        x += w + FromDIP(2);
    }

    // Right, from the edge inward: panel and camera toggles, zoom, camera status.
    int right = width - FromDIP(10);
    for (Item item : {Item::Panel, Item::Camera}) {
        right -= itemH;
        m_rects[size_t(item)] = wxRect(right, y, itemH, itemH);
        right -= FromDIP(2);
    }
    right -= FromDIP(8);
    gc->GetTextExtent("000%", &tw, &th);   // widest zoom, so the status pill doesn't jump
    const int zoomW = int(std::ceil(tw)) + FromDIP(16);
    right -= zoomW;
    m_rects[size_t(Item::Zoom)] = wxRect(right, y, zoomW, itemH);
    right -= FromDIP(8);
    gc->GetTextExtent(m_statusText, &tw, &th);
    const int pillH = FromDIP(26);
    const int pillW = FromDIP(12 + 7 + 7 + 12) + int(std::ceil(tw));
    right -= pillW;
    m_rects[size_t(Item::Status)] = wxRect(right, (height - pillH) / 2, pillW, pillH);
}

void TopBar::Render(wxMemoryDC& dc)
{
    const wxSize size = GetClientSize();
    dc.SetBackground(wxBrush(chrome::kBackground));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    LayoutItems(gc.get(), size.x, size.y);

    const double ui = GetDPIScaleFactor();
    const double radius = 6 * ui;
    double tw = 0, th = 0;
    auto active = [this](Item item) { return m_hot == item || m_open == item; };
    auto fill = [&](const wxRect& r, const wxColour& colour, double corner) {
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(colour));
        gc->DrawRoundedRectangle(r.x, r.y, r.width, r.height, corner);
    };

    // Logo: the app icon, rendered from its SVG at the exact size.
    if (m_logoIcon.IsOk())
        gc->DrawBitmap(m_logoIcon.GetBitmap(m_logo.GetSize()), m_logo.x, m_logo.y, m_logo.width, m_logo.height);

    gc->SetFont(GetFont().Bold().Scaled(1.08f), chrome::kText);
    gc->GetTextExtent("FlowBoard", &tw, &th);
    gc->DrawText("FlowBoard", m_brandX, (size.y - th) / 2);
    const double divider = m_rects[0].x - 10 * ui;
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(chrome::kBorder, 1.0)));
    gc->StrokeLine(divider, size.y / 2.0 - 9 * ui, divider, size.y / 2.0 + 9 * ui);

    // Tabs
    for (int i = 0; i < kTabCount; ++i) {
        const Item item = Item(i);
        const wxRect& r = m_rects[size_t(i)];
        if (active(item))
            fill(r, chrome::kHover, radius);
        gc->SetFont(GetFont(), active(item) ? chrome::kText : chrome::kTextSoft);
        gc->GetTextExtent(kTabs[i], &tw, &th);
        const double tx = r.x + 10 * ui, ty = r.y + (r.height - th) / 2;
        gc->DrawText(kTabs[i], tx, ty);
        DrawIcon(gc.get(), IconId::Chevron, r.GetRight() - 12 * ui, r.y + r.height / 2.0 + ui, 9 * ui,
                 chrome::kMuted);
        if (item == Item::Effects && m_effectsOn) {   // some ink effect is on
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(chrome::kAccent));
            gc->DrawEllipse(tx + tw + 3 * ui, ty - 2 * ui, 6 * ui, 6 * ui);
        }
    }

    // Camera status
    const wxRect& s = m_rects[size_t(Item::Status)];
    fill(s, active(Item::Status) ? chrome::kHover : chrome::kRaised, s.height / 2.0);
    const double dot = 7 * ui;
    gc->SetBrush(wxBrush(StatusColour(m_status)));
    gc->DrawEllipse(s.x + 12 * ui, s.y + (s.height - dot) / 2, dot, dot);
    gc->SetFont(GetFont(), m_status == Status::Off ? chrome::kMuted : chrome::kTextSoft);
    gc->GetTextExtent(m_statusText, &tw, &th);
    gc->DrawText(m_statusText, s.x + 26 * ui, s.y + (s.height - th) / 2);

    // Zoom
    const wxRect& z = m_rects[size_t(Item::Zoom)];
    if (active(Item::Zoom))
        fill(z, chrome::kHover, radius);
    const wxString zoom = wxString::Format("%d%%", m_zoom);
    gc->SetFont(GetFont(), active(Item::Zoom) ? chrome::kText : chrome::kTextSoft);
    gc->GetTextExtent(zoom, &tw, &th);
    gc->DrawText(zoom, z.x + (z.width - tw) / 2, z.y + (z.height - th) / 2);

    // Camera and panel toggles
    auto toggle = [&](Item item, IconId icon, bool on) {
        const wxRect& r = m_rects[size_t(item)];
        if (active(item))
            fill(r, chrome::kHover, radius);
        DrawIcon(gc.get(), icon, r.x + r.width / 2.0, r.y + r.height / 2.0, 18 * ui,
                 on ? chrome::kText : chrome::kMuted);
    };
    toggle(Item::Camera, m_cameraOn ? IconId::Camera : IconId::CameraOff, m_cameraOn);
    toggle(Item::Panel, IconId::Sidebar, m_panelShown);

    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(chrome::kBorder, 1.0)));
    gc->StrokeLine(0, size.y - 0.5, size.x, size.y - 0.5);
}

TopBar::Item TopBar::HitTest(const wxPoint& pt) const
{
    for (size_t i = 0; i < m_rects.size(); ++i)
        if (m_rects[i].Contains(pt))
            return Item(i);
    return Item::None;
}

wxString TopBar::Tooltip(Item item) const
{
    switch (item) {
    case Item::Status: return "Camera status: click for details";
    case Item::Zoom: return "Zoom: click to reset the view (Ctrl+0)";
    case Item::Camera: return m_cameraOn ? "Turn the camera off (F5)" : "Turn the camera on (F5)";
    case Item::Panel: return m_panelShown ? "Hide the camera panel (F2)" : "Show the camera panel (F2)";
    default: return {};   // the tabs say what they are
    }
}

void TopBar::OnMouse(wxMouseEvent& event)
{
    const Item hot = event.Leaving() ? Item::None : HitTest(event.GetPosition());
    if (hot != m_hot) {
        m_hot = hot;
        UpdateTooltip();
        Refresh(false);
    }
    if ((event.LeftDown() || event.LeftDClick()) && hot != Item::None && onClick)
        onClick(hot);
    event.Skip();
}
