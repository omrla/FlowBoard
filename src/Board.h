#pragma once

#include "Gestures.h"

#include <wx/graphics.h>
#include <wx/wx.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

enum class Tool { Pen, Highlighter, Eraser, Line, Rect, Ellipse, Pan };

struct RectD {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Contains(const wxPoint2DDouble& p) const { return p.m_x >= x0 && p.m_x <= x1 && p.m_y >= y0 && p.m_y <= y1; }
    bool Intersects(const RectD& r) const { return x0 <= r.x1 && r.x0 <= x1 && y0 <= r.y1 && r.y0 <= y1; }
    RectD Inflated(double d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
    double Width() const { return x1 - x0; }
    double Height() const { return y1 - y0; }
};

struct Stroke {
    std::vector<wxPoint2DDouble> pts;   // world coordinates
    wxColour color;
    double width = 4.0;                 // world units
    bool highlighter = false;
    bool themeInk = false;              // follows the board theme (black on white, white on dark)
    bool rainbow = false;               // colours cycle along the stroke, starting at `hue`
    bool glow = false;                  // neon glow around a bright core
    double hue = 0.0;                   // degrees
    RectD bounds;                       // includes half the stroke width (and the glow)

    void UpdateBounds();
};
using StrokePtr = std::shared_ptr<const Stroke>;

// Ink effects for new strokes (the top bar's Effects menu).
struct InkEffects {
    bool rainbow = false;   // colours cycle along each stroke
    bool glow = false;      // neon glow
    bool mirror = false;    // every stroke is mirrored across the middle of the view
    bool vanish = false;    // strokes fade away after a moment, like a laser pointer
};

// The drawing surface: an infinite, zoomable board driven by hands and the mouse.
class Board : public wxWindow {
public:
    explicit Board(wxWindow* parent);

    // Feeds the hands seen in the latest camera frame (replaces the previous set).
    void UpdateHands(const std::vector<PointerInput>& hands, double now);
    // Drives animations (toasts, hold progress). Call regularly.
    void Tick(double now);

    void Undo();
    void Redo();
    void ClearBoard();
    void ResetView();
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }

    void SetTool(Tool tool);
    void SetColorIndex(int index);
    void SetSizeIndex(int index);
    void SetDarkTheme(bool dark);
    bool IsDarkTheme() const { return m_dark; }
    void SetShowGrid(bool show);
    bool IsGridShown() const { return m_grid; }
    void SetShowSkeleton(bool show);
    void SetInkEffects(const InkEffects& effects);
    void SetHint(const wxString& hint);
    void ShowToast(const wxString& text);

    bool IsEmpty() const { return m_strokes.empty(); }
    // Changes whenever the drawing does (strokes, erasing, undo, clear), so the window
    // can tell whether there is anything new to save.
    uint64_t Revision() const { return m_revision; }
    // Saves the drawing, cropped to its strokes, as an image (PNG, JPEG...) or a vector PDF.
    bool ExportImage(const wxString& path, wxBitmapType type) const;
    bool ExportPdf(const wxString& path) const;

    double GetZoom() const { return m_zoom; }

    std::function<void()> onQuickSave;
    std::function<void()> onStateChanged;

private:
    enum class Mode { Idle, Draw, Erase, Button, Navigate, Grab, MenuPress };
    enum class ItemKind { Tool, Action, Color, Size };
    enum Action { ActUndo, ActRedo, ActClear, ActSave };

    struct Pointer {
        PointerInput in;
        Mode mode = Mode::Idle;
        Tool tool = Tool::Pen;              // tool the current stroke was started with
        int button = -1;                    // pressed dock item
        std::shared_ptr<Stroke> stroke;     // stroke being drawn
        wxPoint2DDouble lastWorld;          // previous eraser position
        bool erased = false;                // this eraser drag changed the board
        double downTime = 0.0;
        int releaseMark = -1;               // stroke length when the pinch began to open
        double mirrorX = 0.0;               // world x of the mirror axis for this stroke
    };
    struct DockItem {
        ItemKind kind;
        int value;
        RectD rect;
    };
    struct MenuItem {
        ItemKind kind;
        int value;
        double a0, a1, r0, r1;   // wedge angles (radians) and radii
    };
    // One pen pass of a stroke: a plain stroke is a single pass; glow and rainbow ink
    // are layers of wider translucent passes and short runs of changing colour.
    struct InkPass {
        wxColour colour;
        double width;
        std::vector<wxPoint2DDouble> pts;
    };
    struct Vanishing {
        StrokePtr stroke;
        double born;
    };

    // Interaction
    void ProcessPointer(Pointer& p, const PointerInput& in, double now);
    void Press(Pointer& p, const wxPoint2DDouble& pos, double now);
    void Drag(Pointer& p);
    void Release(Pointer& p, const wxPoint2DDouble& pos);
    void EndPointer(Pointer& p);
    void UpdateNavigation();
    void BeginStroke(Pointer& p, const wxPoint2DDouble& screen);
    void CommitStroke(Pointer& p);
    void EraseAlong(Pointer& p, const wxPoint2DDouble& world);
    bool EraseSegment(const wxPoint2DDouble& a, const wxPoint2DDouble& b);
    void PushUndo(std::vector<StrokePtr> snapshot);
    void StrokesChanged();
    void ActivateItem(ItemKind kind, int value);
    void Notify();

    // Dock and radial menu
    void LayoutDock();
    int DockHitTest(const wxPoint2DDouble& pt) const;
    void OpenMenu(const wxPoint2DDouble& at);
    int MenuHitTest(const wxPoint2DDouble& pt) const;
    bool IsOverUi(const wxPoint2DDouble& pt) const;

    // View
    wxPoint2DDouble ToWorld(const wxPoint2DDouble& screen) const;
    RectD VisibleWorld() const;
    RectD ExportArea() const;
    void ApplyView(wxGraphicsContext* gc) const;
    double BrushPx() const;
    double EraserRadiusPx() const;
    wxColour Background() const;
    wxColour PanelColour() const;
    wxColour InkColour() const;
    wxColour SwatchColour(int index) const;
    wxColour StrokeColour(const Stroke& s) const;

    // Rendering
    void OnPaint(wxPaintEvent& event);
    void Render(wxMemoryDC& dc);   // the whole board, its UI and the cursors
    void OnSize(wxSizeEvent& event);
    void RebuildCache();
    void DrawGrid(wxDC& dc) const;
    std::vector<InkPass> InkPasses(const Stroke& s, double opacity = 1.0) const;
    void DrawStroke(wxGraphicsContext* gc, const Stroke& s, double opacity = 1.0) const;
    void DrawLiveStroke(wxGraphicsContext* gc, const Pointer& p) const;
    void DrawPanel(wxGraphicsContext* gc, const RectD& r, double radius) const;
    void DrawDock(wxGraphicsContext* gc) const;
    void DrawMenu(wxGraphicsContext* gc) const;
    void DrawSkeleton(wxGraphicsContext* gc, const PointerInput& in) const;
    void DrawPointer(wxGraphicsContext* gc, const Pointer& p) const;
    void DrawPill(wxGraphicsContext* gc, const wxString& text, double cx, double y, double alpha) const;

    // Mouse / keyboard
    void OnMouse(wxMouseEvent& event);
    void OnRightDown(wxMouseEvent& event);
    void OnWheel(wxMouseEvent& event);
    void OnChar(wxKeyEvent& event);

    std::vector<StrokePtr> m_strokes;
    std::vector<std::vector<StrokePtr>> m_undo, m_redo;
    uint64_t m_revision = 0;

    Tool m_tool = Tool::Pen;
    int m_color = 0;
    int m_size = 1;
    bool m_dark = false;
    bool m_grid = true;
    bool m_skeleton = true;
    InkEffects m_fx;
    double m_nextHue = 200.0;              // rainbow strokes start at successive hues
    std::vector<Vanishing> m_vanishing;    // vanishing-ink strokes still fading out

    wxPoint2DDouble m_origin{0, 0};   // world point at the window's top-left
    double m_zoom = 1.0;
    std::vector<int> m_navIds;
    wxPoint2DDouble m_navAnchor;
    double m_navSpread = 1.0, m_navZoom = 1.0;

    std::map<int, Pointer> m_pointers;
    std::vector<DockItem> m_dock;
    RectD m_rail, m_bar;
    std::vector<RectD> m_separators;
    bool m_menuOpen = false;
    wxPoint2DDouble m_menuCenter;
    std::vector<MenuItem> m_menuItems;

    wxBitmap m_cache;
    bool m_cacheDirty = true;
    wxString m_hint, m_toast;
    double m_toastTime = -100.0;
    double m_now = 0.0;
    double m_ui = 1.0;   // DPI scale
};
