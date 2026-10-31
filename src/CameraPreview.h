#pragma once

#include <opencv2/core.hpp>
#include <wx/wx.h>

// Shows the camera image (with the tracked hand skeleton) in the camera panel, with
// rounded corners and the live gesture state over its bottom edge.
class CameraPreview : public wxWindow {
public:
    explicit CameraPreview(wxWindow* parent);

    void SetFrame(const cv::Mat& rgb);
    void ClearFrame();
    // Shown in place of the image while there is none ("Camera off" / "F5 to start").
    void SetMessage(const wxString& title, const wxString& detail, bool cameraOff);
    // A label over the bottom of the image (the live gesture state); empty for none.
    void SetCaption(const wxString& caption);

private:
    void OnPaint(wxPaintEvent& event);

    wxBitmap m_bitmap;
    wxString m_title = "Camera off";
    wxString m_detail;
    bool m_cameraOff = true;
    wxString m_caption;
};
