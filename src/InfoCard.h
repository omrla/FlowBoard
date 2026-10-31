#pragma once

#include <wx/popupwin.h>
#include <wx/wx.h>

#include <functional>
#include <vector>

// A dropdown card of short reference text (the gesture list, tips, camera details):
// titled sections of rows, each a highlighted key ("Index", "Fist") and what it does.
// Closes on any click outside it.
class InfoCard : public wxPopupTransientWindow {
public:
    struct Row {
        wxString key;
        wxString text;
    };
    struct Section {
        wxString title;
        std::vector<Row> rows;
    };

    InfoCard(wxWindow* parent, std::vector<Section> sections, wxString footer = {});

    // Shows the card just below `anchor` (screen coordinates), left-aligned with it, or
    // right-aligned for anchors at the right of the window.
    void ShowBelow(const wxRect& anchor, bool alignRight);

    std::function<void()> onDismiss;

protected:
    void OnDismiss() override;

private:
    void Render(wxMemoryDC& dc);
    // What to draw where; the layout is worked out once with the window's own fonts.
    struct Piece {
        enum Kind { Title, Key, Text, Footer } kind;
        wxString text;
        double x, y, w, h;
    };
    wxSize Layout(wxGraphicsContext* gc);

    std::vector<Section> m_sections;
    wxString m_footer;
    std::vector<Piece> m_pieces;
    bool m_roundedBySystem = false;
};
