#include "MainFrame.h"

#include "AppIcon.h"
#include "CameraPreview.h"
#include "Chrome.h"
#include "InfoCard.h"

#include <wx/aboutdlg.h>
#include <wx/config.h>
#include <wx/datetime.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

enum {
    ID_ToggleCamera = wxID_HIGHEST + 1,
    ID_ResetView,
    ID_TogglePanel,
    ID_DarkBoard,
    ID_ShowGrid,
    ID_ShowHands,
    ID_FullScreen,
    ID_Gestures,
    ID_QuickSave,
    ID_ChooseSaveFolder,
    ID_OpenSaveFolder,
    ID_Rainbow,
    ID_Glow,
    ID_Mirror,
    ID_Vanish,
    ID_Format,   // + the format's index; keep last
};

// Save formats, in the order of the Save As filters and the Quick save format menu.
struct SaveFormat {
    const char* label;
    const char* ext;
};
const SaveFormat kSaveFormats[] = {
    {"PNG image", "png"},
    {"JPEG image", "jpg"},
    {"PDF document", "pdf"},
};
enum { FormatPng, FormatJpeg, FormatPdf };
constexpr int FormatCount = int(std::size(kSaveFormats));

int FormatFromExtension(const wxString& ext)
{
    const wxString e = ext.Lower();
    if (e == "png") return FormatPng;
    if (e == "jpg" || e == "jpeg") return FormatJpeg;
    if (e == "pdf") return FormatPdf;
    return -1;
}

wxString DefaultSaveDir()
{
    wxFileName dir = wxFileName::DirName(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Pictures));
    dir.AppendDir("FlowBoard");
    return dir.GetPath();
}

std::filesystem::path FindModelDir()
{
    const wxString exeDir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    const wxString candidates[] = {
        exeDir + "/models",
        wxGetCwd() + "/models",
        exeDir + "/../models",
        exeDir + "/../../models",
    };
    for (const wxString& dir : candidates)
        if (wxFileExists(dir + "/" + kPalmModelFile))
            return std::filesystem::path(dir.ToStdWstring());
    return std::filesystem::path(candidates[0].ToStdWstring());
}

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "FlowBoard"), m_timer(this)
{
    m_modelDir = FindModelDir();
    SetIcons(AppIcons());
    SetBackgroundColour(chrome::kBackground);
    chrome::StyleTitleBar(this);

    m_topBar = new TopBar(this);
    m_board = new Board(this);
    m_side = BuildSidePanel();
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(m_board, 1, wxEXPAND);
    body->Add(m_side, 0, wxEXPAND);
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_topBar, 0, wxEXPAND);
    root->Add(body, 1, wxEXPAND);
    SetSizer(root);

    SetMinSize(FromDIP(wxSize(960, 640)));
    SetSize(FromDIP(wxSize(1440, 900)));
    Centre();

    BindCommands();
    LoadSettings();
    ApplyTrackingSettings();
    ApplyInkEffects();
    // Open dropdowns after the click has been handled, not from inside it.
    m_topBar->onClick = [this](TopBar::Item item) { CallAfter([this, item] { OnTopBarClick(item); }); };
    m_board->onStateChanged = [this] { m_topBar->SetZoom(int(std::lround(m_board->GetZoom() * 100))); };
    m_board->onQuickSave = [this] { QuickSave(); };

    Bind(wxEVT_TIMER, &MainFrame::OnTimer, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    m_timer.Start(15);
    CallAfter([this] { StartCamera(); });
}

void MainFrame::BindCommands()
{
    // Keyboard shortcuts; there is no menu bar to carry them.
    wxAcceleratorEntry keys[] = {
        {wxACCEL_CTRL, 'N', wxID_NEW},
        {wxACCEL_CTRL, 'S', wxID_SAVEAS},
        {wxACCEL_CTRL | wxACCEL_SHIFT, 'S', ID_QuickSave},
        {wxACCEL_CTRL, 'Z', wxID_UNDO},
        {wxACCEL_CTRL, 'Y', wxID_REDO},
        {wxACCEL_CTRL, 'D', ID_DarkBoard},
        {wxACCEL_CTRL, 'G', ID_ShowGrid},
        {wxACCEL_CTRL, '0', ID_ResetView},
        {wxACCEL_NORMAL, WXK_F1, ID_Gestures},
        {wxACCEL_NORMAL, WXK_F2, ID_TogglePanel},
        {wxACCEL_NORMAL, WXK_F5, ID_ToggleCamera},
        {wxACCEL_NORMAL, WXK_F11, ID_FullScreen},
    };
    SetAcceleratorTable(wxAcceleratorTable(int(WXSIZEOF(keys)), keys));

    // The same commands come from the shortcuts and the dropdown menus, so toggles flip
    // the current state rather than reading a menu item's check mark.
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->ClearBoard(); }, wxID_NEW);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SaveAs(); }, wxID_SAVEAS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { QuickSave(); }, ID_QuickSave);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ChooseSaveFolder(); }, ID_ChooseSaveFolder);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxFileName::Mkdir(m_saveDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        wxLaunchDefaultApplication(m_saveDir);
    }, ID_OpenSaveFolder);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { m_saveFormat = e.GetId() - ID_Format; },
         ID_Format, ID_Format + FormatCount - 1);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->Undo(); }, wxID_UNDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->Redo(); }, wxID_REDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { TogglePanel(); }, ID_TogglePanel);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->SetDarkTheme(!m_board->IsDarkTheme()); }, ID_DarkBoard);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->SetShowGrid(!m_board->IsGridShown()); }, ID_ShowGrid);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        m_showHands = !m_showHands;
        m_board->SetShowSkeleton(m_showHands);
    }, ID_ShowHands);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_board->ResetView(); }, ID_ResetView);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        ShowFullScreen(!IsFullScreen(), wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION);
    }, ID_FullScreen);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleCamera(); }, ID_ToggleCamera);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowCard(TopBar::Item::Gestures); }, ID_Gestures);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleEffect(&InkEffects::rainbow, "Rainbow ink"); }, ID_Rainbow);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleEffect(&InkEffects::glow, "Neon glow"); }, ID_Glow);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleEffect(&InkEffects::mirror, "Mirror drawing"); }, ID_Mirror);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleEffect(&InkEffects::vanish, "Vanishing ink"); }, ID_Vanish);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxAboutDialogInfo info;
        info.SetName("FlowBoard");
        info.SetVersion("1.0");
        info.SetIcon(AppIcons().GetIcon(FromDIP(wxSize(64, 64))));
        // The about box doesn't wrap the description, so it carries its own line breaks.
        info.SetDescription("A whiteboard you draw on in the air. A webcam tracks your\n"
                            "hands, and Quest-style pinch gestures control the cursor.\n\n"
                            "Hand tracking runs MediaPipe's models on the graphics card\n"
                            "through ONNX Runtime + DirectML. Built with wxWidgets,\n"
                            "OpenCV and ONNX Runtime.");
        info.SetCopyright("Made by @omrla");
        info.SetWebSite("https://github.com/omrla", "github.com/omrla");
        wxAboutBox(info, this);
    }, wxID_ABOUT);
}

wxSlider* MainFrame::AddSlider(wxWindow* parent, wxSizer* sizer, const wxString& label,
                               const wxString& tip, int value, int min, int max)
{
    auto* text = new wxStaticText(parent, wxID_ANY, label);
    text->SetForegroundColour(chrome::kTextSoft);
    auto* slider = new wxSlider(parent, wxID_ANY, value, min, max);
    text->SetToolTip(tip);
    slider->SetToolTip(tip);
    sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(slider, 1, wxEXPAND);
    return slider;
}

wxWindow* MainFrame::BuildSidePanel()
{
    // The camera panel: the preview and the few settings worth having at hand. Everything
    // else lives in the top bar's dropdowns.
    auto* panel = new wxPanel(this);
    panel->SetBackgroundColour(chrome::kBackground);
    const int pad = FromDIP(16);

    auto heading = [panel](const wxString& text) {
        auto* label = new wxStaticText(panel, wxID_ANY, text.Upper());
        label->SetFont(label->GetFont().Bold().Scaled(0.85f));
        label->SetForegroundColour(chrome::kMuted);
        return label;
    };
    auto label = [panel](const wxString& text) {
        auto* label = new wxStaticText(panel, wxID_ANY, text);
        label->SetForegroundColour(chrome::kTextSoft);
        return label;
    };

    auto* content = new wxBoxSizer(wxVERTICAL);
    content->AddSpacer(pad);
    content->Add(heading("Camera"), 0, wxLEFT | wxRIGHT, pad);
    content->AddSpacer(FromDIP(10));
    m_preview = new CameraPreview(panel);
    content->Add(m_preview, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    auto* source = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
    source->AddGrowableCol(1);
    m_cameraIndex = new wxChoice(panel, wxID_ANY);
    for (int i = 0; i < 10; ++i)
        m_cameraIndex->Append(wxString::Format("Camera %d", i + 1));
    m_cameraIndex->SetSelection(0);
    m_cameraIndex->SetToolTip("Which camera to use, if you have more than one");
    m_handCount = new wxChoice(panel, wxID_ANY);
    m_handCount->Append("1 hand");
    m_handCount->Append("2 hands");
    m_handCount->SetSelection(1);
    m_handCount->SetToolTip("Two hands are needed to zoom");
    source->Add(label("Source"), 0, wxALIGN_CENTER_VERTICAL);
    source->Add(m_cameraIndex, 1, wxEXPAND);
    source->Add(label("Track"), 0, wxALIGN_CENTER_VERTICAL);
    source->Add(m_handCount, 1, wxEXPAND);
    content->AddSpacer(FromDIP(14));
    content->Add(source, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    content->AddSpacer(FromDIP(24));
    content->Add(heading("Tracking"), 0, wxLEFT | wxRIGHT, pad);
    content->AddSpacer(FromDIP(8));
    auto* tuning = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    tuning->AddGrowableCol(1);
    m_smoothing = AddSlider(panel, tuning, "Smoothing",
                            "Higher = steadier cursor, lower = snappier cursor", 50, 0, 100);
    m_reach = AddSlider(panel, tuning, "Reach",
                        "How much of the camera view maps onto the whole board. "
                        "Lower = less arm movement needed.", 70, 35, 100);
    m_pinch = AddSlider(panel, tuning, "Pinch",
                        "Pinch sensitivity: higher = a pinch registers with the fingers further apart",
                        50, 0, 100);
    content->Add(tuning, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    content->AddSpacer(pad);

    // One label column for both sections, so the dropdowns and sliders line up.
    int labelWidth = 0;
    for (wxFlexGridSizer* grid : {source, tuning})
        for (size_t i = 0; i < grid->GetItemCount(); i += 2)
            labelWidth = std::max(labelWidth, grid->GetItem(i)->GetMinSize().x);
    for (wxFlexGridSizer* grid : {source, tuning})
        for (size_t i = 0; i < grid->GetItemCount(); i += 2)
            grid->GetItem(i)->SetMinSize(labelWidth, -1);

    // A hairline between the board and the panel.
    auto* divider = new wxWindow(panel, wxID_ANY);
    divider->SetBackgroundColour(chrome::kBorder);
    divider->SetMinSize(wxSize(1, 1));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(divider, 0, wxEXPAND);
    row->Add(content, 1, wxEXPAND);
    panel->SetSizer(row);
    panel->SetMinSize(wxSize(std::max(FromDIP(305), row->GetMinSize().x), -1));

    m_cameraIndex->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        if (m_tracker.IsActive())
            StartCamera();
    });
    m_handCount->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ApplyTrackingSettings(); });
    for (wxSlider* slider : {m_smoothing, m_reach, m_pinch})
        slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { ApplyTrackingSettings(); });
    return panel;
}

// --- Top bar ------------------------------------------------------------------

void MainFrame::OnTopBarClick(TopBar::Item item)
{
    using Item = TopBar::Item;
    switch (item) {
    case Item::File: ShowFileMenu(); break;
    case Item::View: ShowViewMenu(); break;
    case Item::Effects: ShowEffectsMenu(); break;
    case Item::Gestures:
    case Item::Tips:
    case Item::Status: ShowCard(item); break;
    case Item::Zoom: m_board->ResetView(); break;
    case Item::Camera: ToggleCamera(); break;
    case Item::Panel: TogglePanel(); break;
    default: break;
    }
}

void MainFrame::PopupTabMenu(TopBar::Item item, wxMenu& menu)
{
    const wxRect tab = m_topBar->ItemRect(item);
    m_topBar->SetOpenItem(item);
    m_topBar->Update();   // show the tab as open before the menu's modal loop starts
    PopupMenu(&menu, ScreenToClient(m_topBar->ClientToScreen(wxPoint(tab.x, tab.GetBottom() + FromDIP(5)))));
    m_topBar->SetOpenItem(TopBar::Item::None);
}

void MainFrame::ShowFileMenu()
{
    wxMenu menu;
    menu.Append(wxID_NEW, "New board\tCtrl+N");
    menu.Append(wxID_SAVEAS, L"Save as…\tCtrl+S");
    menu.Append(ID_QuickSave, "Quick save\tCtrl+Shift+S");
    menu.AppendSeparator();
    // Where the board's save button and Quick save put files, and in what format.
    auto* format = new wxMenu;
    for (int i = 0; i < FormatCount; ++i)
        format->AppendRadioItem(ID_Format + i, kSaveFormats[i].label)->Check(i == m_saveFormat);
    menu.AppendSubMenu(format, "Quick save format");
    auto* folder = new wxMenu;
    folder->Append(wxID_ANY, m_saveDir)->Enable(false);
    folder->AppendSeparator();
    folder->Append(ID_ChooseSaveFolder, L"Change…");
    folder->Append(ID_OpenSaveFolder, "Open in Explorer");
    menu.AppendSubMenu(folder, "Quick save folder");
    menu.AppendSeparator();
    menu.Append(wxID_ABOUT, "About FlowBoard");
    menu.Append(wxID_EXIT, "Exit");
    PopupTabMenu(TopBar::Item::File, menu);
}

void MainFrame::ShowViewMenu()
{
    wxMenu menu;
    menu.AppendCheckItem(ID_DarkBoard, "Dark board\tCtrl+D")->Check(m_board->IsDarkTheme());
    menu.AppendCheckItem(ID_ShowGrid, "Dot grid\tCtrl+G")->Check(m_board->IsGridShown());
    menu.AppendCheckItem(ID_ShowHands, "Show my hands")->Check(m_showHands);
    menu.AppendSeparator();
    menu.AppendCheckItem(ID_TogglePanel, "Camera panel\tF2")->Check(m_side->IsShown());
    menu.AppendCheckItem(ID_FullScreen, "Full screen\tF11")->Check(IsFullScreen());
    menu.Append(ID_ResetView, "Reset view\tCtrl+0");
    PopupTabMenu(TopBar::Item::View, menu);
}

void MainFrame::ShowEffectsMenu()
{
    wxMenu menu;
    menu.AppendCheckItem(ID_Rainbow, "Rainbow ink")->Check(m_fx.rainbow);
    menu.AppendCheckItem(ID_Glow, "Neon glow")->Check(m_fx.glow);
    menu.AppendCheckItem(ID_Mirror, "Mirror drawing")->Check(m_fx.mirror);
    menu.AppendCheckItem(ID_Vanish, "Vanishing ink")->Check(m_fx.vanish);
    PopupTabMenu(TopBar::Item::Effects, menu);
}

InfoCard* MainFrame::CreateCard(TopBar::Item item)
{
    switch (item) {
    case TopBar::Item::Gestures:
        return new InfoCard(this, {
            {"Pinch thumb to", {{"Index", "Draw, or press a button"},
                                {"Middle", "Undo"},
                                {"Ring", "Open the quick menu"},
                                {"Pinky", "Clear the board"}}},
            {"Whole hand", {{"Fist", "Grab and move the board"},
                            {"Both hands", "Pinch and pull apart to zoom"},
                            {"Palm to you", "Pinch to recenter the view"}}},
            {"Mouse and keys", {{"Mouse", "Drag to draw, wheel to zoom"},
                                {"Right-click", "Quick menu"},
                                {"Keys", L"P H E L R O tools, 1–8 colours"}}},
        });
    case TopBar::Item::Tips:
        return new InfoCard(this, {{"", {
            {"Distance", L"About an arm’s length from the camera"},
            {"Framing", "Keep your hand in the middle of the view"},
            {"Reach", "Lower it in the camera panel to move less"},
            {"Ring tap", "Palm to the camera, thumb firmly on the tip"},
            {"Quick save", "Saves to the folder set in the File menu"},
            {"Neon glow", "Looks best on the dark board (Ctrl+D)"},
        }}});
    default: {   // the camera status
        std::vector<InfoCard::Row> rows;
        const TrackerState state = m_tracker.GetState();
        rows.push_back({"Status", state == TrackerState::Stopped ? wxString("Off")
                                                                  : wxString::FromUTF8(m_tracker.GetStatus())});
        if (state == TrackerState::Running) {
            rows.push_back({"Speed", wxString::Format(L"%.0f fps  ·  %.0f ms per frame", m_fps, m_inferenceMs)});
            const bool gpu = m_tracker.Device() == "GPU";
            rows.push_back({"Runs on", gpu ? wxString("Graphics card (DirectML)") : wxString("Processor")});
            if (!gpu && !m_tracker.GpuError().empty())
                rows.push_back({"GPU error", wxString::FromUTF8(m_tracker.GpuError())});
        }
        return new InfoCard(this, {{"Camera", rows}},
                            "F5 turns the camera on or off. Its settings are in the camera panel (F2).");
    }
    }
}

void MainFrame::ShowCard(TopBar::Item item)
{
    if (item == m_cardItem && HandTracker::Now() - m_cardClosed < 0.3) {
        m_cardItem = TopBar::Item::None;   // this click closed the card; leave it closed
        return;
    }
    InfoCard* card = CreateCard(item);
    m_cardItem = item;
    m_topBar->SetOpenItem(item);
    card->onDismiss = [this, card] {
        m_cardClosed = HandTracker::Now();
        m_topBar->SetOpenItem(TopBar::Item::None);
        CallAfter([card] { card->Destroy(); });
    };
    const wxRect tab = m_topBar->ItemRect(item);
    card->ShowBelow(wxRect(m_topBar->ClientToScreen(tab.GetTopLeft()), tab.GetSize()),
                    item == TopBar::Item::Status);
}

void MainFrame::TogglePanel()
{
    m_side->Show(!m_side->IsShown());
    m_topBar->SetPanelShown(m_side->IsShown());
    Layout();
}

// --- Camera and tracking --------------------------------------------------------

void MainFrame::StartCamera()
{
    const double now = HandTracker::Now();
    m_gestures.Reset();
    m_board->UpdateHands({}, now);
    m_lastSeq = 0;
    m_cameraStarted = now;
    if (!m_tracker.Start(m_cameraIndex->GetSelection(), m_modelDir, m_handCount->GetSelection() + 1)) {
        wxMessageBox(wxString::FromUTF8(m_tracker.GetStatus()) +
                         "\n\nThe model files are expected in:\n" + wxString(m_modelDir.wstring()),
                     "FlowBoard", wxOK | wxICON_ERROR, this);
    }
    UpdateTrackingStatus(now);
}

void MainFrame::StopCamera()
{
    m_tracker.Stop();
    UpdateTrackingStatus(HandTracker::Now());
}

void MainFrame::ToggleCamera()
{
    if (m_tracker.IsActive()) StopCamera();
    else StartCamera();
}

void MainFrame::ApplyTrackingSettings()
{
    GestureSettings& s = m_gestures.settings;
    s.smoothing = m_smoothing->GetValue() / 100.0;
    s.reach = m_reach->GetValue() / 100.0;
    s.pinchSensitivity = m_pinch->GetValue() / 100.0;
    m_board->SetShowSkeleton(m_showHands);
    m_tracker.SetMaxHands(m_handCount->GetSelection() + 1);
}

void MainFrame::ApplyInkEffects()
{
    m_board->SetInkEffects(m_fx);
    m_topBar->SetEffectsOn(m_fx.rainbow || m_fx.glow || m_fx.mirror || m_fx.vanish);
}

void MainFrame::ToggleEffect(bool InkEffects::*effect, const wxString& name)
{
    m_fx.*effect = !(m_fx.*effect);
    ApplyInkEffects();
    m_board->ShowToast(name + (m_fx.*effect ? " on" : " off"));
}

void MainFrame::OnTimer(wxTimerEvent&)
{
    const double now = HandTracker::Now();
    TrackingFrame frame;
    if (m_tracker.PollFrame(frame, m_lastSeq)) {
        const std::vector<PointerInput> inputs = m_gestures.Update(frame, m_board->GetClientSize());
        m_board->UpdateHands(inputs, now);
        m_preview->SetFrame(frame.preview);
        m_fps = frame.fps;
        m_inferenceMs = frame.inferenceMs;
        if (!frame.hands.empty())
            m_lastHandSeen = now;

        // A hand at the edge of the camera's view is partly cut off and tracks poorly
        // (recordings showed hands held at the bottom edge), so say which way to move.
        wxString edge;
        if (!frame.hands.empty() && frame.imageSize.width > 0) {
            const auto& L = frame.hands.front().lm;
            const float x = (L[WRIST].x + L[INDEX_MCP].x + L[PINKY_MCP].x) / 3 / frame.imageSize.width;
            const float y = (L[WRIST].y + L[INDEX_MCP].y + L[PINKY_MCP].y) / 3 / frame.imageSize.height;
            if (y > 0.86f) edge = L"Raise your hand a little: it's at the bottom edge of the camera's view";
            else if (y < 0.12f) edge = L"Lower your hand a little: it's at the top edge of the camera's view";
            else if (x < 0.1f || x > 0.9f) edge = L"Move your hand toward the middle of the camera's view";
        }
        if (edge != m_edgeHint) {
            m_edgeHint = edge;
            m_edgeSince = now;
        }

        wxString text;
        for (const PointerInput& in : inputs) {
            const wchar_t* state = in.system ? L"recentering"
                                 : in.palmFacing ? L"palm toward you"
                                 : in.down ? L"pinch"
                                 : in.grab ? L"fist"
                                 : L"open";
            if (!text.empty())
                text += L"     ";
            text += wxString::Format(L"%s · %s", in.isRight ? L"Right" : L"Left", state);
        }
        m_gestureText = text;
    }
    if (now - m_lastStatusUpdate > 0.2) {
        m_lastStatusUpdate = now;
        UpdateTrackingStatus(now);
    }
    m_board->Tick(now);
}

void MainFrame::UpdateTrackingStatus(double now)
{
    const TrackerState state = m_tracker.GetState();
    if (state != m_lastState) {
        if (state == TrackerState::Failed || state == TrackerState::Stopped) {
            m_gestures.Reset();
            m_board->UpdateHands({}, now);
            m_preview->ClearFrame();
            m_gestureText.clear();
        }
        m_lastState = state;
    }

    using Status = TopBar::Status;
    wxString hint;
    switch (state) {
    case TrackerState::Running:
        m_topBar->SetStatus(Status::Running, wxString::Format(L"%.0f fps  ·  %s", m_fps,
                                                              wxString::FromUTF8(m_tracker.Device())));
        if (now - m_lastHandSeen > 2.0 && now - m_cameraStarted > 1.5)
            hint = L"Hold a hand up to the camera, then pinch thumb and index to draw";
        else if (!m_edgeHint.empty() && now - m_edgeSince > 0.6)
            hint = m_edgeHint;
        break;
    case TrackerState::Starting:
        m_topBar->SetStatus(Status::Starting, L"Starting…");
        m_preview->SetMessage(L"Starting the camera…", wxEmptyString, false);
        hint = L"Starting the camera…";
        break;
    case TrackerState::Failed:
        m_topBar->SetStatus(Status::Failed, "Camera error");
        m_preview->SetMessage("Camera unavailable", "Press F5 to try again", true);
        hint = L"Hand tracking unavailable  ·  click the camera status at the top right for details";
        break;
    case TrackerState::Stopped:
        m_topBar->SetStatus(Status::Off, "Camera off");
        m_preview->SetMessage("Camera off", "Press F5 to start", true);
        hint = L"Camera is off  ·  press F5 to start hand tracking";
        break;
    }
    m_preview->SetCaption(m_gestureText);
    m_topBar->SetCameraOn(m_tracker.IsActive());
    m_topBar->SetZoom(int(std::lround(m_board->GetZoom() * 100)));   // hands zoom without notifying
    m_board->SetHint(hint);
}

// --- Saving ---------------------------------------------------------------------

bool MainFrame::ExportBoard(const wxString& path, int format)
{
    switch (format) {
    case FormatJpeg: return m_board->ExportImage(path, wxBITMAP_TYPE_JPEG);
    case FormatPdf: return m_board->ExportPdf(path);
    default: return m_board->ExportImage(path, wxBITMAP_TYPE_PNG);
    }
}

bool MainFrame::SaveAs()
{
    // Same order as kSaveFormats.
    const wxString wildcard = "PNG image (*.png)|*.png|JPEG image (*.jpg)|*.jpg;*.jpeg|"
                              "PDF document (*.pdf)|*.pdf";
    wxFileDialog dialog(this, "Save board", m_saveDir,
                        wxDateTime::Now().Format("board-%Y%m%d-%H%M%S.") + kSaveFormats[m_saveFormat].ext,
                        wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    dialog.SetFilterIndex(m_saveFormat);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    // The file's extension decides the format; without a known one, use the chosen filter.
    wxFileName file(dialog.GetPath());
    int format = FormatFromExtension(file.GetExt());
    if (format < 0) {
        format = std::clamp(dialog.GetFilterIndex(), 0, FormatCount - 1);
        file.SetFullName(file.GetFullName() + "." + kSaveFormats[format].ext);
        if (file.FileExists() &&
            wxMessageBox(file.GetFullName() + " already exists. Replace it?", "Save board",
                         wxYES_NO | wxICON_WARNING, this) != wxYES)
            return false;
    }
    if (!ExportBoard(file.GetFullPath(), format)) {
        wxMessageBox("Could not save " + file.GetFullPath(), "FlowBoard", wxOK | wxICON_ERROR, this);
        return false;
    }
    // Remember the place and format for next time (and for the board's save button).
    m_saveDir = file.GetPath();
    m_saveFormat = format;
    m_savedRevision = m_board->Revision();
    m_board->ShowToast("Saved " + file.GetFullName());
    return true;
}

void MainFrame::QuickSave()
{
    // Hands can't drive a file dialog, so this saves straight into the save folder.
    const wxFileName dir = wxFileName::DirName(m_saveDir);
    if (!dir.DirExists() && !dir.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        m_board->ShowToast("Could not create " + m_saveDir);
        return;
    }
    const wxFileName file(m_saveDir,
                          wxDateTime::Now().Format("board-%Y%m%d-%H%M%S.") + kSaveFormats[m_saveFormat].ext);
    if (ExportBoard(file.GetFullPath(), m_saveFormat)) {
        m_savedRevision = m_board->Revision();
        m_board->ShowToast("Saved " + file.GetFullPath());
    } else {
        m_board->ShowToast("Could not save to " + m_saveDir);
    }
}

bool MainFrame::ConfirmClose()
{
    // Nothing drawn, or nothing new since the last save: close without asking.
    if (m_board->IsEmpty() || m_board->Revision() == m_savedRevision)
        return true;
    wxMessageDialog ask(this, "Do you want to save your board before closing?", "FlowBoard",
                        wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_QUESTION);
    ask.SetExtendedMessage("If you don't save, your drawing will be lost.");
    ask.SetYesNoCancelLabels("&Save...", "Do&n't save", "Cancel");
    switch (ask.ShowModal()) {
    case wxID_YES: return SaveAs();   // cancelling the file dialog keeps FlowBoard open
    case wxID_NO: return true;
    default: return false;
    }
}

void MainFrame::ChooseSaveFolder()
{
    wxDirDialog dialog(this, "Choose where FlowBoard saves your boards", m_saveDir,
                       wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
    if (dialog.ShowModal() != wxID_OK)
        return;
    m_saveDir = dialog.GetPath();
    m_board->ShowToast("Quick save folder: " + m_saveDir);
}

// --- Settings -------------------------------------------------------------------

void MainFrame::LoadSettings()
{
    wxConfigBase* cfg = wxConfigBase::Get();
    m_cameraIndex->SetSelection(std::clamp(int(cfg->ReadLong("Camera", 0)), 0, int(m_cameraIndex->GetCount()) - 1));
    m_handCount->SetSelection(std::clamp(int(cfg->ReadLong("Hands", 2)), 1, 2) - 1);
    m_smoothing->SetValue(int(cfg->ReadLong("Smoothing", 50)));
    m_reach->SetValue(int(cfg->ReadLong("Reach", 70)));
    m_pinch->SetValue(int(cfg->ReadLong("Pinch", 50)));
    m_showHands = cfg->ReadBool("ShowHands", true);
    m_fx.rainbow = cfg->ReadBool("RainbowInk", false);
    m_fx.glow = cfg->ReadBool("NeonGlow", false);
    m_fx.mirror = cfg->ReadBool("MirrorDrawing", false);
    m_fx.vanish = cfg->ReadBool("VanishingInk", false);
    m_board->SetDarkTheme(cfg->ReadBool("DarkBoard", false));
    m_board->SetShowGrid(cfg->ReadBool("Grid", true));
    const bool panel = cfg->ReadBool("CameraPanel", true);
    m_side->Show(panel);
    m_topBar->SetPanelShown(panel);
    m_saveDir = cfg->Read("SaveFolder", DefaultSaveDir());
    m_saveFormat = std::clamp(int(cfg->ReadLong("SaveFormat", FormatPng)), 0, FormatCount - 1);
}

void MainFrame::SaveSettings()
{
    wxConfigBase* cfg = wxConfigBase::Get();
    cfg->Write("Camera", long(m_cameraIndex->GetSelection()));
    cfg->Write("Hands", long(m_handCount->GetSelection() + 1));
    cfg->Write("Smoothing", long(m_smoothing->GetValue()));
    cfg->Write("Reach", long(m_reach->GetValue()));
    cfg->Write("Pinch", long(m_pinch->GetValue()));
    cfg->Write("ShowHands", m_showHands);
    cfg->Write("RainbowInk", m_fx.rainbow);
    cfg->Write("NeonGlow", m_fx.glow);
    cfg->Write("MirrorDrawing", m_fx.mirror);
    cfg->Write("VanishingInk", m_fx.vanish);
    cfg->Write("DarkBoard", m_board->IsDarkTheme());
    cfg->Write("Grid", m_board->IsGridShown());
    cfg->Write("CameraPanel", m_side->IsShown());
    cfg->Write("SaveFolder", m_saveDir);
    cfg->Write("SaveFormat", long(m_saveFormat));
    cfg->Flush();
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    // Pause hand input while asking, so a pinch can't change the drawing meanwhile.
    m_timer.Stop();
    if (event.CanVeto() && !ConfirmClose()) {
        event.Veto();
        m_timer.Start();   // same interval as before
        return;
    }
    SaveSettings();
    m_tracker.Stop();
    event.Skip();   // let the default handler destroy the frame
}
