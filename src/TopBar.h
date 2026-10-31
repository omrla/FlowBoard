#pragma once

#include <wx/bmpbndl.h>
#include <wx/wx.h>

#include <array>
#include <functional>

// The slim bar across the top of the window, in the style of modern drawing apps: the
// menus as dropdown tabs on the left; the camera's status, the zoom and quick toggles on
// the right.
class TopBar : public wxWindow {
public:
    enum class Item { None = -1, File, View, Effects, Gestures, Tips, Status, Zoom, Camera, Panel, Count };
    enum class Status { Off, Starting, Running, Failed };

    explicit TopBar(wxWindow* parent);

    void SetStatus(Status status, const wxString& text) { Set(m_status, status); Set(m_statusText, text); }
    void SetZoom(int percent) { Set(m_zoom, percent); }
    void SetCameraOn(bool on) { Set(m_cameraOn, on); }
    void SetPanelShown(bool shown) { Set(m_panelShown, shown); }
    void SetEffectsOn(bool on) { Set(m_effectsOn, on); }     // a dot on the Effects tab
    void SetOpenItem(Item item) { Set(m_open, item); }       // the tab whose dropdown is showing
    // Where an item is, in client coordinates (valid once the bar has been painted).
    wxRect ItemRect(Item item) const { return m_rects[size_t(item)]; }

    std::function<void(Item)> onClick;

private:
    void Render(wxMemoryDC& dc);   // paints the bar, laying out its items as it goes
    void LayoutItems(wxGraphicsContext* gc, int width, int height);
    Item HitTest(const wxPoint& pt) const;
    wxString Tooltip(Item item) const;
    void OnMouse(wxMouseEvent& event);
    void UpdateTooltip();

    // Changes something the bar shows, repainting only if it really changed.
    template <class T>
    void Set(T& field, const T& value)
    {
        if (field == value)
            return;
        field = value;
        Refresh(false);
        UpdateTooltip();
    }

    std::array<wxRect, size_t(Item::Count)> m_rects;
    wxBitmapBundle m_logoIcon;   // the app icon
    wxRect m_logo;
    int m_brandX = 0;
    Item m_hot = Item::None;
    Item m_open = Item::None;
    Status m_status = Status::Off;
    wxString m_statusText = "Camera off";
    int m_zoom = 100;
    bool m_cameraOn = false;
    bool m_panelShown = true;
    bool m_effectsOn = false;
};
