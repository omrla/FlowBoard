#include "AppIcon.h"
#include "Chrome.h"
#include "MainFrame.h"

#include <wx/wx.h>

class FlowBoardApp : public wxApp {
public:
    bool OnInit() override
    {
        SetAppName("FlowBoard");
        SetVendorName("FlowBoard");
        wxInitAllImageHandlers();
        chrome::EnableDarkMode();

        // `--make-icon <file.ico>` writes the .exe's icon from the embedded SVG.
        if (argc >= 3 && argv[1] == "--make-icon") {
            m_iconPath = argv[2];
            return true;
        }
        if (!wxApp::OnInit())
            return false;

        auto* frame = new MainFrame();
        frame->Maximize();
        frame->Show();
        return true;
    }

    int OnRun() override
    {
        if (!m_iconPath.empty())
            return WriteAppIconFile(m_iconPath) ? 0 : 1;
        return wxApp::OnRun();
    }

private:
    wxString m_iconPath;
};

wxIMPLEMENT_APP(FlowBoardApp);
