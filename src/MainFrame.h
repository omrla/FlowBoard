#pragma once

#include "Board.h"
#include "Gestures.h"
#include "HandTracker.h"
#include "TopBar.h"

#include <wx/wx.h>

#include <filesystem>

class CameraPreview;
class InfoCard;

class MainFrame : public wxFrame {
public:
    MainFrame();

private:
    void BindCommands();
    wxWindow* BuildSidePanel();
    wxSlider* AddSlider(wxWindow* parent, wxSizer* sizer, const wxString& label,
                        const wxString& tip, int value, int min, int max);

    // Top bar
    void OnTopBarClick(TopBar::Item item);
    void PopupTabMenu(TopBar::Item item, wxMenu& menu);
    void ShowFileMenu();
    void ShowViewMenu();
    void ShowEffectsMenu();
    // The dropdown card a top-bar tab opens (Gestures, Tips, Status), not yet shown.
    InfoCard* CreateCard(TopBar::Item item);
    void ShowCard(TopBar::Item item);
    void TogglePanel();

    void StartCamera();
    void StopCamera();
    void ToggleCamera();
    void ApplyTrackingSettings();
    void ApplyInkEffects();
    void ToggleEffect(bool InkEffects::*effect, const wxString& name);
    void UpdateTrackingStatus(double now);
    bool ExportBoard(const wxString& path, int format);
    bool SaveAs();      // false if cancelled or the save failed
    void QuickSave();
    bool ConfirmClose();
    void ChooseSaveFolder();
    void LoadSettings();
    void SaveSettings();

    void OnTimer(wxTimerEvent& event);
    void OnClose(wxCloseEvent& event);

    TopBar* m_topBar = nullptr;
    Board* m_board = nullptr;
    wxWindow* m_side = nullptr;
    CameraPreview* m_preview = nullptr;
    wxChoice* m_cameraIndex = nullptr;
    wxChoice* m_handCount = nullptr;
    wxSlider* m_smoothing = nullptr;
    wxSlider* m_reach = nullptr;
    wxSlider* m_pinch = nullptr;

    InkEffects m_fx;
    bool m_showHands = true;

    // The open dropdown card, and when it closed: the click that closes a card by
    // landing on its own tab must not open it again.
    TopBar::Item m_cardItem = TopBar::Item::None;
    double m_cardClosed = -100.0;

    wxTimer m_timer;
    HandTracker m_tracker;
    GestureEngine m_gestures;
    std::filesystem::path m_modelDir;
    uint64_t m_lastSeq = 0;
    TrackerState m_lastState = TrackerState::Stopped;
    double m_lastHandSeen = -100.0;
    double m_lastStatusUpdate = 0.0;
    double m_cameraStarted = 0.0;
    float m_fps = 0.f, m_inferenceMs = 0.f;
    wxString m_gestureText;
    wxString m_edgeHint;          // "move your hand" advice while it's at the camera's edge
    double m_edgeSince = 0.0;
    wxString m_saveDir;    // where Quick save and the board's save button write
    int m_saveFormat = 0;  // index into the save formats (PNG, JPEG, PDF)
    uint64_t m_savedRevision = 0;   // the board's revision when it was last saved
};
