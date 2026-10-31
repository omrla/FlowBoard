#include "CameraPreview.h"

#include "BoardIcons.h"
#include "Chrome.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cstring>
#include <memory>

CameraPreview::CameraPreview(wxWindow* parent)
    : wxWindow(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(272, 153)));   // 16:9, the camera's shape
    Bind(wxEVT_PAINT, &CameraPreview::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Refresh(false);
        event.Skip();
    });
}

void CameraPreview::SetFrame(const cv::Mat& rgb)
{
    if (rgb.empty() || rgb.type() != CV_8UC3)
        return;
    wxImage image(rgb.cols, rgb.rows, false);
    for (int y = 0; y < rgb.rows; ++y)
        std::memcpy(image.GetData() + size_t(y) * rgb.cols * 3, rgb.ptr(y), size_t(rgb.cols) * 3);
    m_bitmap = wxBitmap(image);
    Refresh(false);
}

void CameraPreview::ClearFrame()
{
    m_bitmap = wxBitmap();
    Refresh(false);
}

void CameraPreview::SetMessage(const wxString& title, const wxString& detail, bool cameraOff)
{
    if (title == m_title && detail == m_detail && cameraOff == m_cameraOff)
        return;
    m_title = title;
    m_detail = detail;
    m_cameraOff = cameraOff;
    if (!m_bitmap.IsOk())
        Refresh(false);
}

void CameraPreview::SetCaption(const wxString& caption)
{
    if (caption == m_caption)
        return;
    m_caption = caption;
    Refresh(false);
}

void CameraPreview::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize sz = GetClientSize();
    const wxColour background = GetParent()->GetBackgroundColour();
    dc.SetBackground(wxBrush(background));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    const double ui = GetDPIScaleFactor();
    const double radius = 10 * ui;
    double tw = 0, th = 0;
    gc->SetPen(*wxTRANSPARENT_PEN);

    const int bw = m_bitmap.IsOk() ? m_bitmap.GetWidth() : 0;
    const int bh = m_bitmap.IsOk() ? m_bitmap.GetHeight() : 0;
    if (bw <= 0 || bh <= 0) {
        gc->SetBrush(wxBrush(chrome::kRaised));
        gc->DrawRoundedRectangle(0, 0, sz.x, sz.y, radius);
        DrawIcon(gc.get(), m_cameraOff ? IconId::CameraOff : IconId::Camera, sz.x / 2.0, sz.y / 2.0 - 20 * ui,
                 28 * ui, chrome::kMuted);
        gc->SetFont(GetFont().Bold(), chrome::kTextSoft);
        gc->GetTextExtent(m_title, &tw, &th);
        gc->DrawText(m_title, (sz.x - tw) / 2, sz.y / 2.0 + 2 * ui);
        const double detailY = sz.y / 2.0 + 4 * ui + th;
        gc->SetFont(GetFont().Scaled(0.92f), chrome::kMuted);
        gc->GetTextExtent(m_detail, &tw, &th);
        gc->DrawText(m_detail, (sz.x - tw) / 2, detailY);
        return;
    }

    // Fill the box, cropping the image if its shape differs.
    const double scale = std::max(double(sz.x) / bw, double(sz.y) / bh);
    const double w = bw * scale;
    const double h = bh * scale;
    gc->SetInterpolationQuality(wxINTERPOLATION_GOOD);
    gc->Clip(0, 0, sz.x, sz.y);
    gc->DrawBitmap(m_bitmap, (sz.x - w) / 2, (sz.y - h) / 2, w, h);
    gc->ResetClip();

    // Round the corners by painting the panel's colour over them.
    wxGraphicsPath corners = gc->CreatePath();
    corners.AddRectangle(-1, -1, sz.x + 2, sz.y + 2);
    corners.AddRoundedRectangle(0, 0, sz.x, sz.y, radius);
    gc->SetBrush(wxBrush(background));
    gc->FillPath(corners, wxODDEVEN_RULE);

    if (!m_caption.empty()) {
        gc->SetFont(GetFont(), *wxWHITE);
        gc->GetTextExtent(m_caption, &tw, &th);
        const double px = 9 * ui, py = 3 * ui, margin = 8 * ui;
        const double pillH = th + 2 * py;
        gc->SetBrush(wxBrush(wxColour(0, 0, 0, 150)));
        gc->DrawRoundedRectangle(margin, sz.y - margin - pillH, tw + 2 * px, pillH, pillH / 2);
        gc->DrawText(m_caption, margin + px, sz.y - margin - pillH + py);
    }
}
