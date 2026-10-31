#include "InfoCard.h"

#include "Chrome.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

const wxColour kChip(61, 139, 255, 40);
const wxColour kChipText(160, 196, 255);

// Splits `text` into lines no wider than `width` (in the context's current font).
std::vector<wxString> Wrap(wxGraphicsContext* gc, const wxString& text, double width)
{
    std::vector<wxString> lines;
    wxString line;
    for (const wxString& word : wxSplit(text, ' ', '\0')) {
        const wxString candidate = line.empty() ? word : line + " " + word;
        double w = 0, h = 0;
        gc->GetTextExtent(candidate, &w, &h);
        if (w > width && !line.empty()) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

InfoCard::InfoCard(wxWindow* parent, std::vector<Section> sections, wxString footer)
    : wxPopupTransientWindow(parent, wxBORDER_NONE), m_sections(std::move(sections)), m_footer(std::move(footer))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(chrome::kRaised);
    {
        wxClientDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        SetClientSize(Layout(gc.get()));
    }
    m_roundedBySystem = chrome::RoundCorners(this);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(this);
        Render(dc);
    });
}

wxSize InfoCard::Layout(wxGraphicsContext* gc)
{
    const double ui = GetDPIScaleFactor();
    const double pad = 16 * ui, chipPad = 8 * ui, gap = 12 * ui, maxText = 280 * ui;
    const wxFont titleFont = GetFont().Bold().Scaled(0.85f);
    const wxFont keyFont = GetFont().Bold();
    const wxFont footerFont = GetFont().Scaled(0.92f);
    double tw = 0, th = 0;
    m_pieces.clear();

    // Keys in one column, as wide as the widest; texts beside them, wrapping when long.
    double keyW = 0, textW = 0;
    gc->SetFont(keyFont, chrome::kText);
    for (const Section& s : m_sections)
        for (const Row& row : s.rows) {
            gc->GetTextExtent(row.key, &tw, &th);
            keyW = std::max(keyW, tw + 2 * chipPad);
        }
    gc->SetFont(GetFont(), chrome::kText);
    for (const Section& s : m_sections)
        for (const Row& row : s.rows) {
            gc->GetTextExtent(row.text, &tw, &th);
            textW = std::max(textW, std::min(tw, maxText));
        }
    double lineH = 0;
    gc->GetTextExtent("Ag", &tw, &lineH);
    const double contentW = std::max(keyW + gap + textW, 220 * ui);

    double y = pad;
    for (size_t i = 0; i < m_sections.size(); ++i) {
        const Section& s = m_sections[i];
        if (i > 0)
            y += 10 * ui;
        if (!s.title.empty()) {
            gc->SetFont(titleFont, chrome::kMuted);
            gc->GetTextExtent(s.title.Upper(), &tw, &th);
            m_pieces.push_back({Piece::Title, s.title.Upper(), pad, y, tw, th});
            y += th + 8 * ui;
        }
        for (const Row& row : s.rows) {
            const double chipH = lineH + 6 * ui;
            gc->SetFont(keyFont, chrome::kText);
            gc->GetTextExtent(row.key, &tw, &th);
            if (!row.key.empty())
                m_pieces.push_back({Piece::Key, row.key, pad, y, tw + 2 * chipPad, chipH});
            gc->SetFont(GetFont(), chrome::kText);
            const std::vector<wxString> lines = Wrap(gc, row.text, textW);
            for (size_t l = 0; l < lines.size(); ++l)
                m_pieces.push_back({Piece::Text, lines[l], pad + keyW + gap, y + 3 * ui + l * lineH, textW, lineH});
            y += std::max(chipH, lines.size() * lineH + 6 * ui) + 6 * ui;
        }
    }
    if (!m_footer.empty()) {
        y += 6 * ui;
        gc->SetFont(footerFont, chrome::kMuted);
        double footH = 0;
        gc->GetTextExtent("Ag", &tw, &footH);
        for (const wxString& line : Wrap(gc, m_footer, contentW)) {
            m_pieces.push_back({Piece::Footer, line, pad, y, contentW, footH});
            y += footH;
        }
        y += 6 * ui;
    }
    return wxSize(int(std::ceil(contentW + 2 * pad)), int(std::ceil(y - 6 * ui + pad)));
}

void InfoCard::Render(wxMemoryDC& dc)
{
    const wxSize size = GetClientSize();
    dc.SetBackground(wxBrush(chrome::kRaised));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    const double ui = GetDPIScaleFactor();
    if (!m_roundedBySystem) {
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(chrome::kBorder, 1.0)));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0);
    }
    for (const Piece& p : m_pieces) {
        switch (p.kind) {
        case Piece::Title:
            gc->SetFont(GetFont().Bold().Scaled(0.85f), chrome::kMuted);
            gc->DrawText(p.text, p.x, p.y);
            break;
        case Piece::Key:
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(kChip));
            gc->DrawRoundedRectangle(p.x, p.y, p.w, p.h, 5 * ui);
            gc->SetFont(GetFont().Bold(), kChipText);
            gc->DrawText(p.text, p.x + 8 * ui, p.y + 3 * ui);
            break;
        case Piece::Text:
            gc->SetFont(GetFont(), chrome::kText);
            gc->DrawText(p.text, p.x, p.y);
            break;
        case Piece::Footer:
            gc->SetFont(GetFont().Scaled(0.92f), chrome::kMuted);
            gc->DrawText(p.text, p.x, p.y);
            break;
        }
    }
}

void InfoCard::ShowBelow(const wxRect& anchor, bool alignRight)
{
    const wxSize size = GetSize();
    const int x = alignRight ? anchor.GetRight() + 1 - size.x : anchor.x;
    Position(wxPoint(x, anchor.GetBottom() + 1 + FromDIP(6)), wxSize(0, 0));
    Popup();
}

void InfoCard::OnDismiss()
{
    if (onDismiss)
        onDismiss();
}
